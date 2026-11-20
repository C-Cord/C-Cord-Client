// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
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
#include <ccord/net/live_transport.h>
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

// Numeric order is the sidebar order: USER SETTINGS, APP SETTINGS, C-CORD, EXTENSIONS.
enum settings_category { CAT_USER = 0, CAT_APP, CAT_CCORD, CAT_EXT, CAT_COUNT };

typedef struct page_def {
  const char *label;
  int category;
} page_def;

static const page_def k_pages[SP_COUNT] = {
  {"My Account", CAT_USER},
  {nullptr, -1},   // SP_RETIRED_1
  {"Appearance", CAT_APP},
  {"Voice & Video", CAT_APP},
  {"Streaming & Encoders", CAT_APP},
  {"Text & Images", CAT_APP},
  {"Notifications", CAT_APP},
  {"Keybinds", CAT_APP},
  {"Plugins", CAT_EXT},
  {"Privacy & Safety", CAT_USER},
  {"Devices", CAT_USER},
  {"Connections", CAT_USER},
  {"Accessibility", CAT_APP},
  {"Streamer Mode", CAT_APP},
  {"Language", CAT_APP},
  {"Advanced", CAT_APP},
  {"C-Cord", CAT_CCORD},
  {"Profile", CAT_USER},
};

static const char *const k_categories[CAT_COUNT] = {"USER SETTINGS", "APP SETTINGS", "C-CORD", "EXTENSIONS"};

// M5: device lists come from the platform (SDL) enumeration, never hardcoded names.
// Entry 0 of every selector is "Default"; entries 1..N are the enumerated devices.
#define VOICE_DEV_MAX 16
#if defined(_WIN32)
#define STREAM_ENC_COUNT 5
static const char *const k_encoders[STREAM_ENC_COUNT] = {"NVENC", "AMF", "Quick Sync", "Media Found.", "OpenH264"};
static const int k_encoder_vals[STREAM_ENC_COUNT] = {
  (int)CCORD_STREAM_ENCODER_NVENC, (int)CCORD_STREAM_ENCODER_AMF, (int)CCORD_STREAM_ENCODER_QSV,
  (int)CCORD_STREAM_ENCODER_MF, (int)CCORD_STREAM_ENCODER_OPENH264,
};
#define STREAM_FW_COUNT 3
static const char *const k_frameworks[STREAM_FW_COUNT] = {"Win. Capture", "DXGI Dup", "GDI"};
static const int k_framework_vals[STREAM_FW_COUNT] = {
  (int)CCORD_STREAM_FRAMEWORK_WGC, (int)CCORD_STREAM_FRAMEWORK_DXGI, (int)CCORD_STREAM_FRAMEWORK_GDI,
};
#elif defined(__APPLE__)
#define STREAM_ENC_COUNT 3
static const char *const k_encoders[STREAM_ENC_COUNT] = {"VideoToolbox", "x264", "OpenH264"};
static const int k_encoder_vals[STREAM_ENC_COUNT] = {
  (int)CCORD_STREAM_ENCODER_VIDEOTOOLBOX, (int)CCORD_STREAM_ENCODER_X264, (int)CCORD_STREAM_ENCODER_OPENH264,
};
#define STREAM_FW_COUNT 2
static const char *const k_frameworks[STREAM_FW_COUNT] = {"ScreenCaptureKit", "CoreGraphics"};
static const int k_framework_vals[STREAM_FW_COUNT] = {
  (int)CCORD_STREAM_FRAMEWORK_SCK, (int)CCORD_STREAM_FRAMEWORK_CG,
};
#else
#define STREAM_ENC_COUNT 5
static const char *const k_encoders[STREAM_ENC_COUNT] = {"NVENC", "VAAPI", "AMF", "Vulkan", "OpenH264"};
static const int k_encoder_vals[STREAM_ENC_COUNT] = {
  (int)CCORD_STREAM_ENCODER_NVENC, (int)CCORD_STREAM_ENCODER_VAAPI, (int)CCORD_STREAM_ENCODER_AMF,
  (int)CCORD_STREAM_ENCODER_VULKAN, (int)CCORD_STREAM_ENCODER_OPENH264,
};
#define STREAM_FW_COUNT 5
static const char *const k_frameworks[STREAM_FW_COUNT] = {"PipeWire", "DMA-BUF", "NVFBC", "Vulkan", "X11"};
static const int k_framework_vals[STREAM_FW_COUNT] = {
  (int)CCORD_STREAM_FRAMEWORK_PIPEWIRE, (int)CCORD_STREAM_FRAMEWORK_DMABUF, (int)CCORD_STREAM_FRAMEWORK_NVFBC,
  (int)CCORD_STREAM_FRAMEWORK_VULKAN, (int)CCORD_STREAM_FRAMEWORK_X11,
};
#endif
static int find_val_index(const int *vals, int count, int value)
{
  for (int i = 0; i < count; i++) {
    if (vals[i] == value) return i;
  }
  return -1;
}
static const char *const k_codecs[3] = {"H.264", "HEVC", "AV1"};
static const char *const k_resolutions[4] = {"720p", "1080p", "1440p", "Source"};
static const char *const k_fps[3] = {"15", "30", "60"};
static const char *const k_display_mode[2] = {"Compact", "Cozy"};
static const char *const k_voice_mode[2] = {"Voice Activity", "Push to Talk"};
static const char *const k_web_labels[5] = {"Billing", "Nitro", "Shop", "Server Boost", "Account"};
static const char *const k_web_urls[5] = {
  "https://discord.com/settings/billing",
  "https://discord.com/nitro",
  "https://discord.com/shop",
  "https://discord.com/nitro",
  // Account credential edits (email/password/username/avatar/2FA) are excluded from the client:
  // they stay browser redirects to discord.com (design D6).
  "https://discord.com/settings/account",
};

// ---- M5 voice & video: device cache, PTT capture, page scroll ------------------------------------
// The settings modal is process-wide (one ccord_ui at a time); this transient state is reset by
// ui_settings_init. Device lists are probed when opening Voice & Video, when interacting with a
// device selector, or after a long idle interval so hot-plugged devices appear without render-thread
// stalls every second.
#define VOICE_DEV_REFRESH_MS 15000u
#define SETTINGS_SAVE_DEBOUNCE_MS 500u
static struct {
  ccord_audio_device_info in[VOICE_DEV_MAX], out[VOICE_DEV_MAX];
  size_t n_in, n_out;
  ccord_camera_info cam[VOICE_DEV_MAX];
  size_t n_cam;
  bool valid;
  uint64_t stamp_ms;
} s_dev;
static bool s_ptt_armed;        // keybind capture: the next key press binds Push to Talk
static int s_kb_armed = -1;     // keybind capture on the Keybinds page: settings_keybinds index, -1 = none
// Press feedback: Settings never sees the button release, so a left press holds the "pressed" look for a short time.
#define SETTINGS_PRESS_MS 140u
static float s_press_x, s_press_y;
static uint64_t s_press_until_ms;
static float s_voice_scroll;    // Voice & Video page scroll offset in px
static ccord_scroller s_voice_glide;   // wheel glide of s_voice_scroll (scroll.h)
static bool s_save_pending;
static ccord_ui *s_save_ui;
static uint64_t s_save_due_ms;
static int64_t s_hover_key = -1;
static int s_last_drawn_page = -1;

// The persisted device id is the stable identity; the legacy dropdown index is presentation only.
// After a refresh the index is reconciled from the id; an unplugged device falls back to Default.
static int voice_audio_index_of_id(const char *id, const ccord_audio_device_info *list, size_t n)
{
  if (!id[0]) return 0;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i].id, id) == 0) return (int)i + 1;
  }
  return -1;
}

static int voice_camera_index_of_id(const char *id, const ccord_camera_info *list, size_t n)
{
  if (!id[0]) return 0;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(list[i].id, id) == 0) return (int)i + 1;
  }
  return -1;
}

static void voice_devices_refresh(ccord_ui *ui, bool force)
{
  if (s_dev.valid && !force && ui->now_ms >= s_dev.stamp_ms && ui->now_ms - s_dev.stamp_ms < VOICE_DEV_REFRESH_MS) {
    return;
  }
  s_dev.n_in = ccord_audio_devices_list(true, s_dev.in, VOICE_DEV_MAX);
  s_dev.n_out = ccord_audio_devices_list(false, s_dev.out, VOICE_DEV_MAX);
  s_dev.n_cam = ccord_camera_list(s_dev.cam, VOICE_DEV_MAX);
  s_dev.valid = true;
  s_dev.stamp_ms = ui->now_ms;
  // Reconcile the dropdown index from the persisted id when that device is present. This is
  // deliberately non-destructive: a missing id (unplugged device, or a transient dummy-driver id)
  // must not clobber the user's selection back to Default.
  int idx = voice_audio_index_of_id(ui->settings_mic_device_id, s_dev.in, s_dev.n_in);
  if (idx >= 0) ui->settings_mic_device = idx;
  if (ui->settings_mic_device > (int)s_dev.n_in) ui->settings_mic_device = 0;
  idx = voice_audio_index_of_id(ui->settings_speaker_device_id, s_dev.out, s_dev.n_out);
  if (idx >= 0) ui->settings_speaker_device = idx;
  if (ui->settings_speaker_device > (int)s_dev.n_out) ui->settings_speaker_device = 0;
  idx = voice_camera_index_of_id(ui->settings_camera_device_id, s_dev.cam, s_dev.n_cam);
  if (idx >= 0) ui->settings_camera_device = idx;
  if (ui->settings_camera_device > (int)s_dev.n_cam) ui->settings_camera_device = 0;
}

static int voice_in_count(void) { return 1 + (int)s_dev.n_in; }
static int voice_out_count(void) { return 1 + (int)s_dev.n_out; }
static int voice_cam_count(void) { return 1 + (int)s_dev.n_cam; }

static const char *voice_input_name(const ccord_ui *ui)
{
  int idx = ui->settings_mic_device;
  if (idx <= 0 || idx > (int)s_dev.n_in) return "Default";
  return s_dev.in[idx - 1].name;
}

static const char *voice_output_name(const ccord_ui *ui)
{
  int idx = ui->settings_speaker_device;
  if (idx <= 0 || idx > (int)s_dev.n_out) return "Default";
  return s_dev.out[idx - 1].name;
}

static const char *voice_camera_name(const ccord_ui *ui)
{
  if (s_dev.n_cam == 0) return "No camera detected";
  int idx = ui->settings_camera_device;
  if (idx <= 0 || idx > (int)s_dev.n_cam) return "Default";
  return s_dev.cam[idx - 1].name;
}

static void voice_select_input(ccord_ui *ui, int idx)
{
  ui->settings_mic_device = idx;
  if (idx <= 0) ui->settings_mic_device_id[0] = '\0';
  else snprintf(ui->settings_mic_device_id, sizeof ui->settings_mic_device_id, "%s", s_dev.in[idx - 1].id);
}

static void voice_select_output(ccord_ui *ui, int idx)
{
  ui->settings_speaker_device = idx;
  if (idx <= 0) ui->settings_speaker_device_id[0] = '\0';
  else snprintf(ui->settings_speaker_device_id, sizeof ui->settings_speaker_device_id, "%s", s_dev.out[idx - 1].id);
}

static void voice_select_camera(ccord_ui *ui, int idx)
{
  ui->settings_camera_device = idx;
  if (idx <= 0) ui->settings_camera_device_id[0] = '\0';
  else snprintf(ui->settings_camera_device_id, sizeof ui->settings_camera_device_id, "%s", s_dev.cam[idx - 1].id);
}

// SDL scancode for a UI key event. The platform only forwards the ccord_key set, so capture binds
// exactly those keys; the stored value is a real SDL scancode the app polls with SDL_GetKeyboardState.
static SDL_Scancode key_to_scancode(ccord_key k)
{
  switch (k) {
    case CCORD_KEY_RETURN: return SDL_SCANCODE_RETURN;
    case CCORD_KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case CCORD_KEY_DELETE: return SDL_SCANCODE_DELETE;
    case CCORD_KEY_TAB: return SDL_SCANCODE_TAB;
    case CCORD_KEY_LEFT: return SDL_SCANCODE_LEFT;
    case CCORD_KEY_RIGHT: return SDL_SCANCODE_RIGHT;
    case CCORD_KEY_UP: return SDL_SCANCODE_UP;
    case CCORD_KEY_DOWN: return SDL_SCANCODE_DOWN;
    case CCORD_KEY_HOME: return SDL_SCANCODE_HOME;
    case CCORD_KEY_END: return SDL_SCANCODE_END;
    case CCORD_KEY_PAGEUP: return SDL_SCANCODE_PAGEUP;
    case CCORD_KEY_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
    case CCORD_KEY_A: return SDL_SCANCODE_A;
    case CCORD_KEY_C: return SDL_SCANCODE_C;
    case CCORD_KEY_V: return SDL_SCANCODE_V;
    case CCORD_KEY_X: return SDL_SCANCODE_X;
    case CCORD_KEY_Z: return SDL_SCANCODE_Z;
    case CCORD_KEY_Y: return SDL_SCANCODE_Y;
    case CCORD_KEY_B: return SDL_SCANCODE_B;
    case CCORD_KEY_I: return SDL_SCANCODE_I;
    case CCORD_KEY_U: return SDL_SCANCODE_U;
    case CCORD_KEY_S: return SDL_SCANCODE_S;
    case CCORD_KEY_E: return SDL_SCANCODE_E;
    case CCORD_KEY_F: return SDL_SCANCODE_F;
    case CCORD_KEY_K: return SDL_SCANCODE_K;
    case CCORD_KEY_P: return SDL_SCANCODE_P;
    case CCORD_KEY_G: return SDL_SCANCODE_G;
    case CCORD_KEY_M: return SDL_SCANCODE_M;
    case CCORD_KEY_D: return SDL_SCANCODE_D;
    case CCORD_KEY_T: return SDL_SCANCODE_T;
    case CCORD_KEY_L: return SDL_SCANCODE_L;
    case CCORD_KEY_Q: return SDL_SCANCODE_Q;
    case CCORD_KEY_SLASH: return SDL_SCANCODE_SLASH;
    default: return SDL_SCANCODE_UNKNOWN;   // Escape is reserved (closes the modal) and never binds
  }
}

static void ptt_binding_name(const ccord_ui *ui, char *out, size_t cap)
{
  if (ui->settings_ptt_scancode <= 0 || ui->settings_ptt_scancode >= SDL_SCANCODE_COUNT) {
    snprintf(out, cap, "Not bound");
    return;
  }
  const char *name = SDL_GetScancodeName((SDL_Scancode)ui->settings_ptt_scancode);
  snprintf(out, cap, "%s", name && name[0] ? name : "Unknown key");
}

// ---- rebindable action keybinds (Keybinds page) -------------------------------------------------

static void keybind_name(int scancode, uint8_t mods, char *out, size_t cap)
{
  if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) {
    snprintf(out, cap, "Not bound");
    return;
  }
  const char *key = SDL_GetScancodeName((SDL_Scancode)scancode);
  if (!key || !key[0]) key = "Unknown key";
#if defined(__APPLE__)
  const char *alt_lbl = "Option + ";
  const char *super_lbl = "Cmd + ";
#elif defined(_WIN32)
  const char *alt_lbl = "Alt + ";
  const char *super_lbl = "Win + ";
#else
  const char *alt_lbl = "Alt + ";
  const char *super_lbl = "Super + ";
#endif
  snprintf(out, cap, "%s%s%s%s%s",
           (mods & CCORD_MOD_CTRL) ? "Ctrl + " : "",
           (mods & CCORD_MOD_SHIFT) ? "Shift + " : "",
           (mods & CCORD_MOD_ALT) ? alt_lbl : "",
           (mods & CCORD_MOD_SUPER) ? super_lbl : "", key);
}

bool ui_keybind_match(const ccord_ui *ui, int which, const ccord_input_event *ev)
{
  if (!ui || !ev || ev->type != CCORD_IN_KEY_DOWN) return false;
  if (which == CCORD_UI_KEYBIND_PTT) {
    if (ui->settings_voice_mode != 1) return false;
    int sc = ui->settings_ptt_scancode;
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return false;
    if (key_to_scancode(ev->key) != (SDL_Scancode)sc) return false;
    if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_ALT | CCORD_MOD_SUPER)) != 0) return false;
    if (ui->edit.len > 0 || ui->editing_message != CCORD_ID_NONE) return false;
    return true;
  }
  if (which < 0 || which >= 3) return false;
  int sc = ui->settings_keybinds[which].scancode;
  if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return false;
  if (key_to_scancode(ev->key) != (SDL_Scancode)sc) return false;
  uint8_t mods = ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT | CCORD_MOD_SUPER);
  if (mods != ui->settings_keybinds[which].mods) return false;
  // A bare-key bind must not steal composer typing or message edits.
  if (mods == 0 && (ui->edit.len > 0 || ui->editing_message != CCORD_ID_NONE)) return false;
  return true;
}

void ccord_ui_get_keybinds(const ccord_ui *ui, ccord_ui_keybind out[CCORD_UI_KEYBIND_COUNT])
{
  if (!out) return;
  for (int i = 0; i < CCORD_UI_KEYBIND_COUNT; i++) out[i] = (ccord_ui_keybind){0, 0};
  if (!ui) return;
  for (int i = 0; i < 3; i++) {
    out[i].scancode = ui->settings_keybinds[i].scancode;
    out[i].mods = ui->settings_keybinds[i].mods;
  }
  out[CCORD_UI_KEYBIND_PTT].scancode = ui->settings_ptt_scancode;
}

void ccord_ui_keybind_trigger(const ccord_ui_keybind *kb, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!kb || kb->scancode <= 0 || kb->scancode >= SDL_SCANCODE_COUNT) return;
  const char *key = SDL_GetScancodeName((SDL_Scancode)kb->scancode);
  if (!key || !key[0]) return;
  char name[32];
  size_t n = strlen(key);
  if (n >= sizeof name) n = sizeof name - 1;
  for (size_t i = 0; i < n; i++) {
    char c = key[i];
    name[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
  }
  name[n] = '\0';
  snprintf(out, cap, "%s%s%s%s%s",
           (kb->mods & CCORD_MOD_CTRL) ? "CTRL+" : "",
           (kb->mods & CCORD_MOD_SHIFT) ? "SHIFT+" : "",
           (kb->mods & CCORD_MOD_ALT) ? "ALT+" : "",
           (kb->mods & CCORD_MOD_SUPER) ? "SUPER+" : "", name);
}

// Sidebar scroll offset in px (the up and down buttons below the list move it one row at a time; geometry() clamps).
static float s_sidebar_scroll;
#define SIDEBAR_HEAD_H 48.0f    // space above the first sidebar row
#define SIDEBAR_SCROLL_H 40.0f  // strip under the list that holds the scroll buttons, when the list overflows

typedef struct sgeom {
  ccord_rect panel, sidebar, close, content, back;
  ccord_rect list;             // the scrolling page list in the sidebar; rows are clipped and hit-tested inside it
  ccord_rect sb_up, sb_down;   // sidebar scroll buttons (zero size when every page fits)
  float step;                  // one sidebar scroll step: one row pitch
  ccord_rect page[SP_COUNT];
} sgeom;

static void account_buttons(const ccord_ui *ui, const sgeom *g, ccord_rect *primary, ccord_rect *secondary);
static ccord_rect account_web_btn_rect(const sgeom *g, int idx);

// Sidebar display order: pages grouped by category (USER, APP, EXTENSIONS), enum order within a
// category. The M6 account pages are appended to the enum but belong under USER SETTINGS.
static int sidebar_order(int out[SP_COUNT])
{
  int n = 0;
  for (int c = 0; c < CAT_COUNT; c++) {
    for (int i = 0; i < SP_COUNT; i++) {
      if (i != SP_PROFILE_EDIT && k_pages[i].category == c) out[n++] = i;
      if (i == SP_ACCOUNT && k_pages[SP_PROFILE_EDIT].category == c) out[n++] = SP_PROFILE_EDIT;   // right after My Account
    }
  }
  return n;
}

// What the last settings draw produced, read through ccord_ui_get_settings_debug.
static struct {
  int count;
  int page[24];
  char label[24][40];
  ccord_rect rect[24];
  int body;
} s_drawn = {.body = -1};

// A page index that has a sidebar entry. A stale index (the retired slot, out of range) falls back to My Account.
static bool page_valid(int page)
{
  return page >= 0 && page < SP_COUNT && k_pages[page].label != nullptr;
}

static void settings_fix_page(ccord_ui *ui)
{
  if (page_valid(ui->settings_page)) return;
  ui->settings_page = SP_ACCOUNT;
  ui->settings_category = k_pages[SP_ACCOUNT].category;
  ui->dirty = true;
}

// Up/Down on the sidebar: previous/next entry in the order the sidebar draws.
static bool sidebar_key_step(ccord_ui *ui, int dir)
{
  int order[SP_COUNT];
  int n = sidebar_order(order);
  int cur = -1;
  for (int k = 0; k < n; k++) {
    if (order[k] == ui->settings_page) cur = k;
  }
  int next = cur < 0 ? 0 : cur + dir;
  if (next < 0 || next >= n) return true;
  ui->settings_page = order[next];
  ui->settings_category = k_pages[order[next]].category;
  ui->dirty = true;
  return true;
}

static sgeom geometry(const ccord_ui *ui)
{
  sgeom g = {0};   // every page rect starts empty: the retired slot never gets one
  // The settings page is full-window, like server and channel settings: no floating frame and no scrim.
  float pw = ui->win_w, ph = ui->win_h;
  g.panel = (ccord_rect){0.0f, 0.0f, pw, ph};
  if (ui->mobile_layout) {
    float sl = ui->safe_insets.left;
    float sr = ui->safe_insets.right;
    float st = ui->safe_insets.top;
    float sb_in = ui->keyboard_inset > ui->safe_insets.bottom ? ui->keyboard_inset : ui->safe_insets.bottom;
    float uw = pw - sl - sr;
    float uh = ph - st - sb_in;
    if (uw < 200.0f) uw = pw;
    if (uh < 160.0f) uh = ph;
    g.close = (ccord_rect){sl + uw - UIS_ICON_BTN - UIS_SP_3, st + UIS_SP_3, UIS_ICON_BTN, UIS_ICON_BTN};
    if (ui->mobile_settings_detail) {
      g.back = (ccord_rect){sl + UIS_SP_3, st + UIS_SP_3, 76.0f, UIS_ICON_BTN};
      float top = st + UIS_SP_3 + UIS_ICON_BTN + UIS_SP_3;
      g.content = (ccord_rect){sl + UIS_SP_4, top, uw - 2.0f * UIS_SP_4, uh - (top - st) - UIS_SP_4};
      return g;
    }
    // Level 1: full-width category & page list.
    g.sidebar = (ccord_rect){sl, st, uw, uh};
    float row_h = M(channel_item_height);
    if (row_h < 42.0f) row_h = 42.0f;
    float pitch = row_h + 2.0f;
    int order[SP_COUNT];
    int n = sidebar_order(order);
    float total = 0.0f;
    for (int k = 0; k < n; k++) {
      if (k == 0 || k_pages[order[k]].category != k_pages[order[k - 1]].category) total += (k != 0 ? 12.0f : 0.0f) + 24.0f;
      total += pitch;
    }
    float top = st + UIS_SP_3 + UIS_ICON_BTN + UIS_SP_2;
    float view_h = uh - (top - st) - UIS_SP_2;
    g.list = (ccord_rect){sl, top, uw, view_h};
    g.step = pitch;
    float max_scroll = total > view_h ? total - view_h : 0.0f;
    if (s_sidebar_scroll > max_scroll) s_sidebar_scroll = max_scroll;
    if (s_sidebar_scroll < 0.0f) s_sidebar_scroll = 0.0f;
    float x = sl + UIS_SP_4, w = uw - 2.0f * UIS_SP_4;
    float y = top - s_sidebar_scroll;
    for (int k = 0; k < n; k++) {
      int i = order[k];
      if (k == 0 || k_pages[i].category != k_pages[order[k - 1]].category) {
        if (k != 0) y += 12.0f;
        y += 24.0f;
      }
      g.page[i] = (ccord_rect){x, y, w, row_h};
      y += pitch;
    }
    return g;
  }
  float sb = M(settings_sidebar_width);
  if (sb > pw * 0.42f) sb = pw * 0.42f;
  if (sb < 120.0f) sb = 120.0f;
  g.sidebar = (ccord_rect){g.panel.x, g.panel.y, sb, ph};
  g.content = (ccord_rect){g.panel.x + sb + 28.0f, g.panel.y + 24.0f, pw - sb - 72.0f, ph - 48.0f};
  g.close = (ccord_rect){g.panel.x + pw - 56.0f, g.panel.y + 16.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  // Rows are channel_item_height tall with a 2 px gap. A category heading adds 24 px, and 12 px more before
  // every heading after the first.
  float row_h = M(channel_item_height), pitch = row_h + 2.0f;
  int order[SP_COUNT];
  int n = sidebar_order(order);
  float total = 0.0f;
  for (int k = 0; k < n; k++) {
    if (k == 0 || k_pages[order[k]].category != k_pages[order[k - 1]].category) total += (k != 0 ? 12.0f : 0.0f) + 24.0f;
    total += pitch;
  }
  float top = g.panel.y + SIDEBAR_HEAD_H;
  float full_h = ph - SIDEBAR_HEAD_H - 8.0f;
  bool scrolls = total > full_h;
  float view_h = scrolls ? full_h - SIDEBAR_SCROLL_H : full_h;
  g.list = (ccord_rect){g.sidebar.x, top, sb, view_h};
  g.step = pitch;
  float max_scroll = total > view_h ? total - view_h : 0.0f;
  if (s_sidebar_scroll > max_scroll) s_sidebar_scroll = max_scroll;
  if (s_sidebar_scroll < 0.0f) s_sidebar_scroll = 0.0f;
  if (scrolls) {
    float by = top + view_h + 4.0f;
    g.sb_up = (ccord_rect){g.sidebar.x + 14.0f, by, UIS_ICON_BTN, UIS_ICON_BTN};
    g.sb_down = (ccord_rect){g.sidebar.x + 14.0f + UIS_ICON_BTN + 4.0f, by, UIS_ICON_BTN, UIS_ICON_BTN};
  } else {
    g.sb_up = (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
    g.sb_down = g.sb_up;
  }
  float x = g.sidebar.x + 14.0f, w = sb - 28.0f;
  float y = top - s_sidebar_scroll;
  for (int k = 0; k < n; k++) {
    int i = order[k];
    if (k == 0 || k_pages[i].category != k_pages[order[k - 1]].category) {
      if (k != 0) y += 12.0f;
      y += 24.0f;   // category heading
    }
    g.page[i] = (ccord_rect){x, y, w, row_h};
    y += pitch;
  }
  return g;
}

// Legacy mapping kept for ui.c's debug fields and the screenshot driver:
// 1 = Plugins tab, 2 = My Account tab.  Any other value is a real page index.
ccord_rect ui_settings_page_rect(const ccord_ui *ui, int page)
{
  sgeom g = geometry(ui);
  if (page == 1) return g.page[SP_PLUGINS];
  if (page == 2) return g.page[SP_ACCOUNT];
  if (page < 0 || page >= SP_COUNT) return (ccord_rect){0, 0, 0, 0};
  return g.page[page];
}

// M6 sidebar wiring (settings_m6_devices.h): tab rects of the three account pages plus the
// Account-page web redirect button for the excluded credential edits.
void ui_settings_m6_host_rects(const ccord_ui *ui, ccord_rect out_tabs[CCORD_M6_PAGE_COUNT],
                               ccord_rect *out_account_web_btn)
{
  sgeom g = geometry(ui);
  if (out_tabs) {
    out_tabs[CCORD_M6_PAGE_PRIVACY] = g.page[SP_PRIVACY];
    out_tabs[CCORD_M6_PAGE_DEVICES] = g.page[SP_DEVICES];
    out_tabs[CCORD_M6_PAGE_CONNECTIONS] = g.page[SP_CONNECTIONS];
  }
  if (out_account_web_btn) *out_account_web_btn = account_web_btn_rect(&g, 4);
}

// M6 preference-page sidebar wiring (settings_m6_prefs.h): tab rects of the five preference pages.
void ui_settings_m6_prefs_host_rects(const ccord_ui *ui, ccord_rect out_tabs[CCORD_M6_PREFS_COUNT])
{
  if (!out_tabs) return;
  sgeom g = geometry(ui);
  out_tabs[CCORD_M6_PREFS_NOTIFICATIONS] = g.page[SP_NOTIFICATIONS];
  out_tabs[CCORD_M6_PREFS_ACCESSIBILITY] = g.page[SP_ACCESSIBILITY];
  out_tabs[CCORD_M6_PREFS_STREAMER] = g.page[SP_STREAMER];
  out_tabs[CCORD_M6_PREFS_LANGUAGE] = g.page[SP_LANGUAGE];
  out_tabs[CCORD_M6_PREFS_ADVANCED] = g.page[SP_ADVANCED];
  out_tabs[CCORD_M6_PREFS_CCORD] = g.page[SP_CCORD];
}

size_t ui_settings_sidebar_pages(const ccord_ui *ui, const char **out_category, ccord_rect *out_rect, int *out_page, size_t cap)
{
  sgeom g = geometry(ui);
  int order[SP_COUNT];
  int n = sidebar_order(order);
  size_t w = 0;
  for (int k = 0; k < n && w < cap; k++, w++) {
    if (out_category) out_category[w] = k_categories[k_pages[order[k]].category];
    if (out_rect) out_rect[w] = g.page[order[k]];
    if (out_page) out_page[w] = order[k];
  }
  return w;
}

// Controls by page, for the duplicate check in --verify-slice ccord-settings. The C-CORD entries come
// from settings_m6_prefs.c (the single source its page draws from); the rest mirror the labels the
// other pages draw.
size_t ui_settings_control_table(ui_settings_control_entry *out, size_t cap)
{
  static const ui_settings_control_entry others[] = {
    {SP_APPEARANCE, {"theme", "THEME"}},
    {SP_APPEARANCE, {"zoom", "UI ZOOM"}},
    {SP_APPEARANCE, {"quickcss_enabled", "CUSTOM THEME OVERRIDES"}},
    {SP_APPEARANCE, {"quickcss", "STYLE OVERRIDE SNIPPET"}},
    {SP_TEXT_IMAGES, {"embeds", "INLINE EMBEDS"}},
    {SP_TEXT_IMAGES, {"attachments", "ATTACHMENT PREVIEWS"}},
    {SP_NOTIFICATIONS, {"notify_mentions", "MENTIONS"}},
    {SP_NOTIFICATIONS, {"notify_messages", "ALL MESSAGES"}},
    {SP_NOTIFICATIONS, {"notify_sounds", "SOUNDS"}},
    {SP_NOTIFICATIONS, {"notify_desktop", "DESKTOP NOTIFICATIONS"}},
    {SP_NOTIFICATIONS, {"notify_timeout", "NOTIFICATION TIMEOUT"}},
    {SP_NOTIFICATIONS, {"tts_command", "ENABLE /TTS COMMAND"}},
    {SP_ACCESSIBILITY, {"gif_auto_play", "AUTOMATICALLY PLAY GIFS"}},
    {SP_ACCESSIBILITY, {"animate_emoji", "PLAY ANIMATED EMOJI"}},
    {SP_ACCESSIBILITY, {"animate_stickers", "PLAY ANIMATED STICKERS"}},
    {SP_STREAMER, {"streamer_mode", "ENABLE STREAMER MODE"}},
    {SP_STREAMER, {"streamer_notifications", "ENABLE NOTIFICATIONS WHILE STREAMING"}},
    {SP_STREAMER, {"streamer_hide_personal", "HIDE PERSONAL INFORMATION"}},
    {SP_STREAMER, {"streamer_hide_invites", "HIDE INSTANT INVITE LINKS"}},
    {SP_STREAMER, {"streamer_disable_sounds", "DISABLE SOUNDS"}},
    {SP_STREAMER, {"streamer_disable_notifications", "DISABLE NOTIFICATIONS"}},
    {SP_LANGUAGE, {"locale", "LANGUAGE"}},
    {SP_ADVANCED, {"developer_mode", "DEVELOPER MODE"}},
    {SP_ADVANCED, {"hardware_accel", "HARDWARE ACCELERATION"}},
    {SP_ADVANCED, {"close_to_tray", "CLOSE TO TRAY"}},
    {SP_ADVANCED, {"data_dir", "DATA DIRECTORY"}},
  };
  size_t w = 0;
  for (size_t i = 0; i < sizeof others / sizeof others[0] && w < cap; i++) out[w++] = others[i];
  const ui_settings_control *cc;
  size_t n = ui_settings_ccord_controls(&cc);
  for (size_t i = 0; i < n && w < cap; i++) out[w++] = (ui_settings_control_entry){SP_CCORD, cc[i]};
  return w;
}

// ---- shared row geometry (used by both drawing and hit-testing) ----

static float row_y(const sgeom *g, int row)
{
  return g->content.y + 44.0f + (float)row * 54.0f;
}

// The row a toggle sits in: UIS_H_BTN_SM tall, the switch is centred in it and flush right.
static ccord_rect row_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x, row_y(g, row), g->content.w, UIS_H_BTN_SM};
}

static ccord_rect toggle_rect(const sgeom *g, int row)
{
  return uis_toggle_rect(row_rect(g, row));
}

static ccord_rect chips_rect(const sgeom *g, int row, int opt, int count)
{
  if (count < 1) return (ccord_rect){0, 0, 0, 0};
  float gap = 8.0f;
  float cw = (g->content.w - gap * (float)(count - 1)) / (float)count;
  if (cw > 132.0f) cw = 132.0f;
  if (cw < 44.0f) cw = 44.0f;
  return (ccord_rect){g->content.x + (float)opt * (cw + gap), row_y(g, row) + 20.0f, cw, UIS_H_CHIP};
}

static ccord_rect device_rect(const sgeom *g, int row)
{
  float w = g->content.w * 0.7f;
  if (w > 300.0f) w = 300.0f;
  return (ccord_rect){g->content.x, row_y(g, row) + 18.0f, w, UIS_H_BTN_SM};
}

// Volume row: [-] track [+] value. The buttons are UIS_H_BTN_SM squares; the track is UIS_SLIDER_H tall and
// centred on the buttons.
static ccord_rect vol_minus_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x, row_y(g, row) + 18.0f, UIS_H_BTN_SM, UIS_H_BTN_SM};
}

static ccord_rect vol_track_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x + 36.0f, row_y(g, row) + 28.0f, 200.0f, UIS_SLIDER_H};
}

static ccord_rect vol_plus_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x + 244.0f, row_y(g, row) + 18.0f, UIS_H_BTN_SM, UIS_H_BTN_SM};
}

// Keybind box (click it, then press the chord) and the "Automatic" pill at the end of a streaming chip row.
static ccord_rect keybind_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x, row_y(g, row) + 18.0f, 168.0f, UIS_H_BTN_SM};
}

static ccord_rect auto_pill_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x + g->content.w - 92.0f, row_y(g, row) - 4.0f, 92.0f, UIS_H_CHIP};
}

static ccord_rect extensions_toggle_rect(const sgeom *g)
{
  return toggle_rect(g, 0);
}

static ccord_rect account_web_btn_rect(const sgeom *g, int idx)
{
  float gap = 8.0f;
  float bw = (g->content.w - 4.0f * gap) / 5.0f;
  if (bw < 60.0f) bw = 60.0f;
  float by = g->content.y + g->content.h - 80.0f;
  return (ccord_rect){g->content.x + (float)idx * (bw + gap), by, bw, UIS_H_BTN};
}

static ccord_rect account_email_box_rect(const sgeom *g)
{
  float w = (g->content.w - 8.0f) * 0.52f;
  return (ccord_rect){g->content.x, g->content.y + 114.0f, w, UIS_H_INPUT};
}

static ccord_rect account_password_box_rect(const sgeom *g)
{
  ccord_rect em = account_email_box_rect(g);
  float x = em.x + em.w + 8.0f;
  float w = g->content.x + g->content.w - x;
  return (ccord_rect){x, g->content.y + 114.0f, w, UIS_H_INPUT};
}

static ccord_rect account_login_btn_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x, g->content.y + 168.0f, 180.0f, UIS_H_BTN};
}

static ccord_rect account_token_box_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x, g->content.y + 236.0f, g->content.w, UIS_H_INPUT};
}

static ccord_rect account_token_paste_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x, g->content.y + 284.0f, 68.0f, UIS_H_BTN};
}

static ccord_rect account_token_save_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x + 76.0f, g->content.y + 284.0f, 136.0f, UIS_H_BTN};
}

static ccord_rect account_token_clear_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x + 220.0f, g->content.y + 284.0f, 96.0f, UIS_H_BTN};
}

// QuickCSS snippet editor (Appearance, row 6).
static ccord_rect quickcss_box_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x, row_y(g, 6) + 18.0f, g->content.w, UIS_H_INPUT};
}

// Multi-account switcher (My Account): roster rows between the token buttons and the web links.
#define SAVED_ACCOUNT_ROWS 4
static ccord_rect account_add_btn_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x + g->content.w - 136.0f, g->content.y + 330.0f, 136.0f, UIS_H_BTN};
}

static ccord_rect saved_account_row_rect(const sgeom *g, int i)
{
  return (ccord_rect){g->content.x, g->content.y + 372.0f + (float)i * 40.0f, g->content.w, UIS_H_BTN};
}

static ccord_rect saved_account_switch_rect(const sgeom *g, int i)
{
  ccord_rect row = saved_account_row_rect(g, i);
  return (ccord_rect){row.x + row.w - 156.0f, row.y, 72.0f, UIS_H_BTN};
}

static ccord_rect saved_account_remove_rect(const sgeom *g, int i)
{
  ccord_rect row = saved_account_row_rect(g, i);
  return (ccord_rect){row.x + row.w - 80.0f, row.y, 72.0f, UIS_H_BTN};
}

bool ui_is_login_screen(const ccord_ui *ui)
{
  if (!ui || !ui->settings_open) return false;
  if (ui->login_screen) return true;
  return ccord_store_self_id(ui->d.store) == CCORD_ID_NONE && !ui->account_view.signed_in;
}

static ccord_rect login_card_rect(const ccord_ui *ui)
{
  float sl = ui->mobile_layout ? ui->safe_insets.left : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float st = ui->mobile_layout ? ui->safe_insets.top : 0.0f;
  float sb = ui->mobile_layout ? (ui->keyboard_inset > ui->safe_insets.bottom ? ui->keyboard_inset : ui->safe_insets.bottom) : 0.0f;
  float uw = ui->win_w - sl - sr;
  float uh = ui->win_h - st - sb;
  float cw = 480.0f, ch = 420.0f;
  float pad = ui->mobile_layout ? UIS_SP_3 : 16.0f;
  if (cw > uw - 2.0f * pad) cw = uw - 2.0f * pad;
  if (ch > uh - 2.0f * pad) ch = uh - 2.0f * pad;
  if (cw < 240.0f) cw = uw > 0.0f ? uw : ui->win_w;
  if (ch < 240.0f) ch = uh > 0.0f ? uh : ui->win_h;
  return (ccord_rect){roundf(sl + (uw - cw) * 0.5f), roundf(st + (uh - ch) * 0.5f), cw, ch};
}

static ccord_rect login_tab_rect(const ccord_rect *card, int tab)
{
  float cx = card->x + 32.0f;
  float cw = card->w - 64.0f;
  float tw = (cw - 8.0f) * 0.5f;
  return (ccord_rect){cx + (float)tab * (tw + 8.0f), card->y + 96.0f, tw, UIS_H_BTN};
}

static ccord_rect login_token_box_rect(const ccord_rect *card)
{
  return (ccord_rect){card->x + 32.0f, card->y + 198.0f, card->w - 64.0f, UIS_H_INPUT};
}

static ccord_rect login_token_paste_rect(const ccord_rect *card)
{
  return (ccord_rect){card->x + 32.0f, card->y + 246.0f, card->w - 64.0f, UIS_H_BTN};
}

static ccord_rect login_token_save_rect(const ccord_rect *card)
{
  return (ccord_rect){card->x + 32.0f, card->y + 290.0f, card->w - 64.0f, UIS_H_BTN_LG};
}

static ccord_rect login_email_box_rect(const ccord_rect *card)
{
  return (ccord_rect){card->x + 32.0f, card->y + 196.0f, card->w - 64.0f, UIS_H_INPUT};
}

static ccord_rect login_password_box_rect(const ccord_rect *card)
{
  return (ccord_rect){card->x + 32.0f, card->y + 262.0f, card->w - 64.0f, UIS_H_INPUT};
}

static ccord_rect login_credentials_btn_rect(const ccord_rect *card)
{
  return (ccord_rect){card->x + 32.0f, card->y + 320.0f, card->w - 64.0f, UIS_H_BTN_LG};
}

// Secondary 2FA / TOTP popup modal (only shown after Discord requests 2FA for the account).
static ccord_rect mfa_popup_rect(const ccord_ui *ui)
{
  float sl = ui->mobile_layout ? ui->safe_insets.left : 0.0f;
  float sr = ui->mobile_layout ? ui->safe_insets.right : 0.0f;
  float st = ui->mobile_layout ? ui->safe_insets.top : 0.0f;
  float sb = ui->mobile_layout ? (ui->keyboard_inset > ui->safe_insets.bottom ? ui->keyboard_inset : ui->safe_insets.bottom) : 0.0f;
  float uw = ui->win_w - sl - sr;
  float uh = ui->win_h - st - sb;
  float cw = 400.0f, ch = 296.0f;
  float pad = ui->mobile_layout ? UIS_SP_3 : 16.0f;
  if (cw > uw - 2.0f * pad) cw = uw - 2.0f * pad;
  if (ch > uh - 2.0f * pad) ch = uh - 2.0f * pad;
  if (cw < 240.0f) cw = uw > 0.0f ? uw : ui->win_w;
  if (ch < 220.0f) ch = uh > 0.0f ? uh : ui->win_h;
  return (ccord_rect){roundf(sl + (uw - cw) * 0.5f), roundf(st + (uh - ch) * 0.5f), cw, ch};
}

static ccord_rect mfa_popup_input_rect(const ccord_rect *pop)
{
  return (ccord_rect){pop->x + 28.0f, pop->y + 148.0f, pop->w - 56.0f, UIS_H_INPUT};
}

static ccord_rect mfa_popup_verify_rect(const ccord_rect *pop)
{
  return (ccord_rect){pop->x + 28.0f, pop->y + 196.0f, pop->w - 56.0f, UIS_H_BTN_LG};
}

static ccord_rect mfa_popup_back_rect(const ccord_rect *pop)
{
  return (ccord_rect){pop->x + 28.0f, pop->y + 244.0f, pop->w - 56.0f, UIS_H_BTN};
}

void ui_settings_auth_autofill_rects(const ccord_ui *ui, ccord_rect *email_r, ccord_rect *pw_r, ccord_rect *otp_r)
{
  if (email_r) *email_r = (ccord_rect){0};
  if (pw_r) *pw_r = (ccord_rect){0};
  if (otp_r) *otp_r = (ccord_rect){0};
  if (!ui) return;
  if (ui->account_mfa_required) {
    ccord_rect pop = mfa_popup_rect(ui);
    if (otp_r) *otp_r = mfa_popup_input_rect(&pop);
    return;
  }
  if (ui_is_login_screen(ui)) {
    ccord_rect card = login_card_rect(ui);
    if (email_r) *email_r = login_email_box_rect(&card);
    if (pw_r) *pw_r = login_password_box_rect(&card);
  } else if (ui->settings_open && ui->settings_page == SP_ACCOUNT && !ui->account_view.signed_in) {
    sgeom g = geometry(ui);
    if (email_r) *email_r = account_email_box_rect(&g);
    if (pw_r) *pw_r = account_password_box_rect(&g);
  }
}

void ui_settings_dismiss_mfa(ccord_ui *ui)
{
  if (!ui || !ui->account_mfa_required) return;
  ccord_text_edit_clear(&ui->account_mfa_edit);
  ui->account_mfa_required = false;
  ui->account_login_is_error = false;
  ui->account_login_status[0] = '\0';
  ui->account_focus_field = 2;
  ui->touch_text_focused = false;
  if (ui->d.host.account_action) {
    ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_MFA_CANCEL);
  }
  ui->dirty = true;
}

ccord_rect ui_settings_rect(const ccord_ui *ui, bool plugins_tab)
{
  sgeom g = geometry(ui);
  if (plugins_tab) return g.page[SP_PLUGINS];
  return extensions_toggle_rect(&g);
}

// The one place the Extensions switch changes state: UI flag first, then the app's callback (start or stop hosts).
static void set_extensions(ccord_ui *ui, bool on)
{
  ui->extensions_on = on;
  ui->dirty = true;
  if (ui->d.host.set_extensions_enabled) ui->d.host.set_extensions_enabled(ui->d.host.ud, on);
}

void ccord_ui_set_extensions_enabled(ccord_ui *ui, bool on)
{
  ui->extensions_on = on;
  ui->dirty = true;
}

static void settings_init_defaults(ccord_ui *ui)
{
  if (ui->settings_initialized) return;
  ui->settings_initialized = true;
  ui->settings_page = SP_ACCOUNT;
  ui->settings_category = CAT_USER;
  ui->settings_display_mode = 1;   // cozy
  ui->settings_voice_mode = 0;
  ui->settings_ptt_scancode = 0;      // unbound until captured
  // Action keybind defaults match Discord; every row is rebindable on the Keybinds page.
  ui->settings_keybinds[0].scancode = SDL_SCANCODE_M;
  ui->settings_keybinds[0].mods = CCORD_MOD_CTRL | CCORD_MOD_SHIFT;
  ui->settings_keybinds[1].scancode = SDL_SCANCODE_D;
  ui->settings_keybinds[1].mods = CCORD_MOD_CTRL | CCORD_MOD_SHIFT;
  ui->settings_keybinds[2].scancode = SDL_SCANCODE_K;
  ui->settings_keybinds[2].mods = CCORD_MOD_CTRL;
  ccord_ui_pref_set_close_to_background(true);
  ui->settings_vad_sensitivity = 50;  // 0..100 slider midpoint
  ui->settings_mic_device = 0;
  ui->settings_speaker_device = 0;
  ui->settings_camera_device = 0;
  ui->settings_input_volume = 100;
  ui->settings_output_volume = 100;
  ui->settings_noise_suppression = true;
  ui->settings_echo_cancel = true;
  ui->settings_auto_gain = true;
  ui->settings_embeds = true;
  ui->settings_attachments = true;
  ui->settings_reduced_motion = false;
  ui->settings_hardware_accel = true;
  ui->settings_notify_mentions = true;
  ui->settings_notify_messages = true;
  ui->settings_notify_sounds = true;
  ui->settings_sounds_off = 0;   // every per-sound toggle on
  ui->settings_sounds_open = false;
  ui->settings_notify_desktop = true;
  ui->settings_notify_timeout_ms = 5000;
  ui->settings_quickcss_enabled = false;
  if (!ui->settings_quickcss[0]) {
    snprintf(ui->settings_quickcss, sizeof ui->settings_quickcss, "/* Custom Theme Overrides */");
  }
  if (ui->settings_win_w <= 0) ui->settings_win_w = 1280;
  if (ui->settings_win_h <= 0) ui->settings_win_h = 720;
  ui->settings_encoder = ui->gl_encoder;
  ui->settings_codec = ui->gl_codec;
  ui->settings_framework = ui->gl_framework;
  ui->settings_resolution = ui->gl_resolution;
  ui->settings_fps = ui->gl_fps;
}

// ---- INI persistence (XDG data dir; UI settings only, never credentials) ----

static bool settings_path(char *out, size_t cap)
{
  const char *dir = ccord_paths_data_dir();
  if (!dir || !dir[0]) return false;
  ccord_error err;
  return ccord_paths_join(out, cap, dir, "settings.ini", &err) == CCORD_OK;
}

static bool ini_int(const char *value, int *out)
{
  if (!value || !value[0]) return false;
  char *end = nullptr;
  long v = strtol(value, &end, 10);
  if (end == value || (end && *end != '\0') || v < INT_MIN || v > INT_MAX) return false;
  *out = (int)v;
  return true;
}

static bool ini_bool(const char *value, bool *out)
{
  if (!value) return false;
  if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0) {
    *out = false;
    return true;
  }
  return false;
}

static void clamp_int(int *v, int lo, int hi)
{
  if (*v < lo) *v = lo;
  if (*v > hi) *v = hi;
}

// Device ids are decimal SDL ids (or empty = default). Anything else is rejected so a hand-edited
// INI cannot push garbage into the audio open path.
static bool ini_device_id(const char *value)
{
  if (!value) return false;
  size_t n = strlen(value);
  if (n >= 64) return false;
  for (size_t i = 0; i < n; i++) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

// Keybinds persist as "scancode,mods" (SDL scancode + CCORD_MOD_* bitmask).
static bool ini_keybind(const char *value, int *scancode, uint8_t *mods)
{
  int sc = 0, md = 0;
  if (!value || sscanf(value, "%d,%d", &sc, &md) != 2) return false;
  if (sc < 0 || sc >= SDL_SCANCODE_COUNT || md < 0 || md > 15) return false;
  *scancode = sc;
  *mods = (uint8_t)md;
  return true;
}

// [ui] warm_cache belongs to the warm-start cache (src/app/warm_cache.c reads it); there is no Settings row, so the
// raw value is carried through every save. Empty = the key was never set, so none is written (neighbour behaviour).
static char s_warm_cache_value[16];

static bool settings_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  ccord_ui *ui = ud;
  if (strcmp(section, "saved_messages") == 0) {
    ui_saved_messages_load_ini(ui, key, value);
    return true;
  }
  if (strcmp(section, "pinned_dms") == 0 || strcmp(section, "favorite_dms") == 0 ||
      strcmp(section, "friend_nicknames") == 0 || strcmp(section, "ignored_users") == 0) {
    ui_dm_prefs_load_ini(ui, section, key, value);
    return true;
  }
  if (strcmp(section, "ui") != 0) return true;
  if (strcmp(key, "warm_cache") == 0 && value) {
    snprintf(s_warm_cache_value, sizeof s_warm_cache_value, "%s", value);
    return true;
  }
  if (ui_settings_m6_prefs_ini_visit(ui, key, value)) return true;   // M6 theme/zoom/streamer/notify keys
  int iv;
  bool bv;
  uint8_t bv_mods = 0;
  if (strcmp(key, "display_mode") == 0 && ini_int(value, &iv)) {
    ui->settings_display_mode = iv ? 1 : 0;
  } else if (strcmp(key, "embeds") == 0 && ini_bool(value, &bv)) {
    ui->settings_embeds = bv;
  } else if (strcmp(key, "attachments") == 0 && ini_bool(value, &bv)) {
    ui->settings_attachments = bv;
  } else if (strcmp(key, "reduced_motion") == 0 && ini_bool(value, &bv)) {
    ui->settings_reduced_motion = bv;
  } else if (strcmp(key, "members_hidden") == 0 && ini_bool(value, &bv)) {
    ui->members_hidden = bv;
  } else if (strcmp(key, "dm_profile_hidden") == 0 && ini_bool(value, &bv)) {
    ui->dm_profile_hidden = bv;
  } else if (strcmp(key, "voice_mode") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 1);
    ui->settings_voice_mode = iv;
  } else if (strcmp(key, "mic_device") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, VOICE_DEV_MAX);
    ui->settings_mic_device = iv;
  } else if (strcmp(key, "speaker_device") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, VOICE_DEV_MAX);
    ui->settings_speaker_device = iv;
  } else if (strcmp(key, "camera_device") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, VOICE_DEV_MAX);
    ui->settings_camera_device = iv;
  } else if (strcmp(key, "mic_device_id") == 0 && ini_device_id(value)) {
    snprintf(ui->settings_mic_device_id, sizeof ui->settings_mic_device_id, "%s", value);
  } else if (strcmp(key, "speaker_device_id") == 0 && ini_device_id(value)) {
    snprintf(ui->settings_speaker_device_id, sizeof ui->settings_speaker_device_id, "%s", value);
  } else if (strcmp(key, "camera_device_id") == 0 && ini_device_id(value)) {
    snprintf(ui->settings_camera_device_id, sizeof ui->settings_camera_device_id, "%s", value);
  } else if (strcmp(key, "ptt_scancode") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, SDL_SCANCODE_COUNT - 1);
    ui->settings_ptt_scancode = iv;
  } else if (strcmp(key, "keybind_mute") == 0 && ini_keybind(value, &iv, &bv_mods)) {
    ui->settings_keybinds[0].scancode = iv;
    ui->settings_keybinds[0].mods = bv_mods;
  } else if (strcmp(key, "keybind_deafen") == 0 && ini_keybind(value, &iv, &bv_mods)) {
    ui->settings_keybinds[1].scancode = iv;
    ui->settings_keybinds[1].mods = bv_mods;
  } else if (strcmp(key, "keybind_quick_switcher") == 0 && ini_keybind(value, &iv, &bv_mods)) {
    ui->settings_keybinds[2].scancode = iv;
    ui->settings_keybinds[2].mods = bv_mods;
  } else if (strcmp(key, "close_to_tray") == 0 && ini_bool(value, &bv)) {
    ccord_ui_pref_set_close_to_background(bv);
  } else if (strcmp(key, "quickcss_enabled") == 0 && ini_bool(value, &bv)) {
    ui->settings_quickcss_enabled = bv;
  } else if (strcmp(key, "quickcss") == 0 && value) {
    snprintf(ui->settings_quickcss, sizeof ui->settings_quickcss, "%s", value);
  } else if (strcmp(key, "vad_sensitivity") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 100);
    ui->settings_vad_sensitivity = iv;
  } else if (strncmp(key, "user_volume_", 12) == 0 && ini_int(value, &iv)) {
    // Per-user playback gain (0..200, 0 = muted); a local preference, never Discord state.
    ccord_id uid = CCORD_ID_NONE;
    if (ccord_id_parse(key + 12, strlen(key + 12), &uid)) {
      clamp_int(&iv, 0, 200);
      ccord_ui_set_user_volume(ui, uid, (uint8_t)iv);
    }
  } else if (strcmp(key, "input_volume") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 100);
    ui->settings_input_volume = iv;
  } else if (strcmp(key, "output_volume") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 100);
    ui->settings_output_volume = iv;
  } else if (strcmp(key, "noise_suppression") == 0 && ini_bool(value, &bv)) {
    ui->settings_noise_suppression = bv;
  } else if (strcmp(key, "echo_cancel") == 0 && ini_bool(value, &bv)) {
    ui->settings_echo_cancel = bv;
  } else if (strcmp(key, "auto_gain") == 0 && ini_bool(value, &bv)) {
    ui->settings_auto_gain = bv;
  } else if (strcmp(key, "hardware_accel") == 0 && ini_bool(value, &bv)) {
    ui->settings_hardware_accel = bv;
  } else if (strcmp(key, "notify_mentions") == 0 && ini_bool(value, &bv)) {
    ui->settings_notify_mentions = bv;
  } else if (strcmp(key, "notify_messages") == 0 && ini_bool(value, &bv)) {
    ui->settings_notify_messages = bv;
  } else if (strcmp(key, "notify_sounds") == 0 && ini_bool(value, &bv)) {
    ui->settings_notify_sounds = bv;
  } else if (strcmp(key, "encoder") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, (int)CCORD_STREAM_ENCODER_AUTO);
    ui->settings_encoder = ui->gl_encoder = iv;
  } else if (strcmp(key, "codec") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 2);
    ui->settings_codec = ui->gl_codec = iv;
  } else if (strcmp(key, "framework") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, (int)CCORD_STREAM_FRAMEWORK_AUTO);
    ui->settings_framework = ui->gl_framework = iv;
  } else if (strcmp(key, "resolution") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 3);
    ui->settings_resolution = ui->gl_resolution = iv;
  } else if (strcmp(key, "fps") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 2);
    ui->settings_fps = ui->gl_fps = iv;
  } else if (strcmp(key, "notify_desktop") == 0 && ini_bool(value, &bv)) {
    ui->settings_notify_desktop = bv;
  } else if (strcmp(key, "window_width") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 320, 7680);
    ui->settings_win_w = iv;
    ui->settings_win_saved = true;
  } else if (strcmp(key, "window_height") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 240, 4320);
    ui->settings_win_h = iv;
    ui->settings_win_saved = true;
  } else if (strcmp(key, "stream_stage_height_pct") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 0, 100);
    ui->stream_stage_height_frac = iv > 0 ? (float)iv / 100.0f : 0.0f;
  } else if (strcmp(key, "stage_split_pct") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 15, 85);
    ui->stage_split_ratio = (float)iv / 100.0f;
  } else if (strcmp(key, "stream_pip_width") == 0 && ini_int(value, &iv)) {
    clamp_int(&iv, 200, 960);
    ui->stream_pip_w = (float)iv;
  } else if (strcmp(key, "last_guild_id") == 0 && value) {
    ccord_id gid = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &gid)) ui->saved_guild_id = gid;
  } else if (strcmp(key, "last_channel_id") == 0 && value) {
    ccord_id cid = CCORD_ID_NONE;
    if (ccord_id_parse(value, strlen(value), &cid)) ui->saved_channel_id = cid;
  }
  return true;
}

static void settings_load(ccord_ui *ui)
{
  s_warm_cache_value[0] = '\0';
  char path[PATH_MAX];
  if (!settings_path(path, sizeof path)) return;
  FILE *f = fopen(path, "rb");
  if (!f) return;
  // Read the whole file: the [saved_messages] section can push it well past a fixed stack buffer.
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
  long sz = ftell(f);
  if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
  size_t cap = (size_t)sz + 1u;
  char *text = ccord_malloc(cap);
  if (!text) { fclose(f); return; }
  size_t n = fread(text, 1, (size_t)sz, f);
  fclose(f);
  text[n] = '\0';
  ui->nsaved_msgs = 0;   // rebuild the bookmark list from this file
  ccord_error err;
  if (ccord_ini_parse(text, n, settings_visit, ui, &err) != CCORD_OK) ccord_error_clear(&err);
  ccord_free(text);
  ui_saved_messages_apply_loaded(ui);   // push the loaded bookmarks into the store (newest first)
}

void ui_settings_init(ccord_ui *ui)
{
  s_ptt_armed = false;
  s_kb_armed = -1;
  s_voice_scroll = 0.0f;
  ccord_scroller_cancel(&s_voice_glide);
  s_dev.valid = false;
  s_save_pending = false;
  s_save_ui = nullptr;
  s_save_due_ms = 0;
  s_hover_key = -1;
  s_last_drawn_page = -1;
  ui_settings_m6_reset();
  ui_settings_m6_prefs_reset();
  settings_init_defaults(ui);
  settings_load(ui);
  ui_settings_m6_prefs_apply_loaded(ui);   // persisted theme/zoom reach the host once after load
  // QuickCSS: the snippet editor mirrors the persisted text; an enabled snippet applies at startup.
  ccord_text_edit_clear(&ui->quickcss_edit);
  ccord_error qerr;
  (void)ccord_text_edit_insert(&ui->quickcss_edit, ui->settings_quickcss, strlen(ui->settings_quickcss), &qerr);
  if (ui->settings_quickcss_enabled && ui->d.host.apply_quick_css) {
    ui->d.host.apply_quick_css(ui->d.host.ud, true, ui->settings_quickcss);
  }
  if (ccord_ui_pref_close_to_background() && ui->d.host.set_close_to_tray) {
    ui->d.host.set_close_to_tray(ui->d.host.ud, true);
  }
  ui->anim.reduced_motion = ui->settings_reduced_motion;
}

static void settings_write_ini(ccord_ui *ui, FILE *f)
{
  fprintf(f, "[ui]\n");
  fprintf(f, "display_mode = %d\n", ui->settings_display_mode ? 1 : 0);
  fprintf(f, "embeds = %d\n", ui->settings_embeds ? 1 : 0);
  fprintf(f, "attachments = %d\n", ui->settings_attachments ? 1 : 0);
  fprintf(f, "reduced_motion = %d\n", ui->settings_reduced_motion ? 1 : 0);
  fprintf(f, "members_hidden = %d\n", ui->members_hidden ? 1 : 0);
  fprintf(f, "dm_profile_hidden = %d\n", ui->dm_profile_hidden ? 1 : 0);
  fprintf(f, "voice_mode = %d\n", ui->settings_voice_mode);
  fprintf(f, "mic_device = %d\n", ui->settings_mic_device);
  fprintf(f, "speaker_device = %d\n", ui->settings_speaker_device);
  fprintf(f, "camera_device = %d\n", ui->settings_camera_device);
  fprintf(f, "input_volume = %d\n", ui->settings_input_volume);
  fprintf(f, "output_volume = %d\n", ui->settings_output_volume);
  fprintf(f, "noise_suppression = %d\n", ui->settings_noise_suppression ? 1 : 0);
  fprintf(f, "echo_cancel = %d\n", ui->settings_echo_cancel ? 1 : 0);
  fprintf(f, "auto_gain = %d\n", ui->settings_auto_gain ? 1 : 0);
  fprintf(f, "ptt_scancode = %d\n", ui->settings_ptt_scancode);
  fprintf(f, "keybind_mute = %d,%u\n", ui->settings_keybinds[0].scancode, (unsigned)ui->settings_keybinds[0].mods);
  fprintf(f, "keybind_deafen = %d,%u\n", ui->settings_keybinds[1].scancode, (unsigned)ui->settings_keybinds[1].mods);
  fprintf(f, "keybind_quick_switcher = %d,%u\n", ui->settings_keybinds[2].scancode,
          (unsigned)ui->settings_keybinds[2].mods);
  fprintf(f, "close_to_tray = %d\n", ccord_ui_pref_close_to_background() ? 1 : 0);
  fprintf(f, "quickcss_enabled = %d\n", ui->settings_quickcss_enabled ? 1 : 0);
  fprintf(f, "quickcss = %s\n", ui->settings_quickcss);
  fprintf(f, "vad_sensitivity = %d\n", ui->settings_vad_sensitivity);
  fprintf(f, "mic_device_id = %s\n", ui->settings_mic_device_id);
  fprintf(f, "speaker_device_id = %s\n", ui->settings_speaker_device_id);
  fprintf(f, "camera_device_id = %s\n", ui->settings_camera_device_id);
  for (size_t i = 0; i < ui->user_volume_count; i++) {
    fprintf(f, "user_volume_%llu = %u\n", (unsigned long long)ui->user_volume[i].user_id,
            (unsigned)ui->user_volume[i].pct);
  }
  fprintf(f, "hardware_accel = %d\n", ui->settings_hardware_accel ? 1 : 0);
  fprintf(f, "notify_mentions = %d\n", ui->settings_notify_mentions ? 1 : 0);
  fprintf(f, "notify_messages = %d\n", ui->settings_notify_messages ? 1 : 0);
  fprintf(f, "notify_sounds = %d\n", ui->settings_notify_sounds ? 1 : 0);
  fprintf(f, "encoder = %d\n", ui->settings_encoder);
  fprintf(f, "codec = %d\n", ui->settings_codec);
  fprintf(f, "framework = %d\n", ui->settings_framework);
  fprintf(f, "resolution = %d\n", ui->settings_resolution);
  fprintf(f, "fps = %d\n", ui->settings_fps);
  fprintf(f, "notify_desktop = %d\n", ui->settings_notify_desktop ? 1 : 0);
  fprintf(f, "window_width = %d\n", ui->settings_win_w);
  fprintf(f, "window_height = %d\n", ui->settings_win_h);
  fprintf(f, "stream_stage_height_pct = %d\n",
          ui->stream_stage_height_frac > 0.0f ? (int)lroundf(ui->stream_stage_height_frac * 100.0f) : 0);
  fprintf(f, "stage_split_pct = %d\n",
          (int)lroundf((ui->stage_split_ratio > 0.0f ? ui->stage_split_ratio : 0.55f) * 100.0f));
  fprintf(f, "stream_pip_width = %d\n",
          (int)lroundf(ui->stream_pip_w > 1.0f ? ui->stream_pip_w : 320.0f));
  if (ui->saved_guild_id != CCORD_ID_NONE) {
    fprintf(f, "last_guild_id = %llu\n", (unsigned long long)ui->saved_guild_id);
  }
  if (ui->saved_channel_id != CCORD_ID_NONE) {
    fprintf(f, "last_channel_id = %llu\n", (unsigned long long)ui->saved_channel_id);
  }
  if (s_warm_cache_value[0]) fprintf(f, "warm_cache = %s\n", s_warm_cache_value);
  ui_settings_m6_prefs_ini_save(ui, f);   // M6 theme/zoom/streamer/notify keys
  ui_saved_messages_save_ini(ui, f);      // [saved_messages] bookmarks
  ui_dm_prefs_save_ini(ui, f);            // [pinned_dms], [favorite_dms], [friend_nicknames], [ignored_users]
}

static void settings_save(ccord_ui *ui)
{
  s_save_pending = false;
  s_save_ui = nullptr;
  char path[PATH_MAX];
  if (!settings_path(path, sizeof path)) return;
  const char *dir = ccord_paths_data_dir();
  ccord_error derr;
  if (dir && dir[0] && ccord_mkdir_p(dir, 0700, &derr) != CCORD_OK) return;
#if !defined(_WIN32)
  char *buf = nullptr;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (!f) return;
  settings_write_ini(ui, f);
  bool ok = !ferror(f) && fclose(f) == 0 && buf != nullptr;
  if (ok) {
    ccord_error werr;
    (void)ccord_file_write_atomic(path, buf, len, 0600, false, &werr);
  }
  free(buf);
#else
  char stage[PATH_MAX];
  int sn = snprintf(stage, sizeof stage, "%s/.settings_stage.tmp", (dir && dir[0]) ? dir : ".");
  if (sn <= 0 || (size_t)sn >= sizeof stage) return;
  FILE *f = fopen(stage, "wb+");
  if (!f) return;
  settings_write_ini(ui, f);
  if (!ferror(f) && fflush(f) == 0 && fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
    if (sz >= 0 && fseek(f, 0, SEEK_SET) == 0) {
      size_t len = (size_t)sz;
      char *buf = ccord_malloc(len ? len : 1u);
      if (buf) {
        size_t got = len ? fread(buf, 1, len, f) : 0u;
        if (got == len) {
          ccord_error werr;
          (void)ccord_file_write_atomic(path, buf, len, 0600, false, &werr);
        }
        ccord_free(buf);
      }
    }
  }
  fclose(f);
  (void)remove(stage);
#endif
}

static void settings_save_flush(ccord_ui *ui)
{
  if (!s_save_pending || s_save_ui != ui) return;
  settings_save(ui);
}

void ui_settings_save(ccord_ui *ui)
{
  if (!ui) return;
  settings_init_defaults(ui);
  settings_save(ui);
}

// QuickCSS edit/toggle path: mirror the editor into the persisted string, apply live through the
// host (base theme restore + ccord_theme_apply_quick_css) and persist atomically to settings.ini.
void ui_quickcss_changed(ccord_ui *ui)
{
  snprintf(ui->settings_quickcss, sizeof ui->settings_quickcss, "%s", ccord_text_edit_cstr(&ui->quickcss_edit));
  ui->dirty = true;
  if (ui->d.host.apply_quick_css) {
    ui->d.host.apply_quick_css(ui->d.host.ud, ui->settings_quickcss_enabled, ui->settings_quickcss);
  }
  settings_save(ui);
}

// Multi-account roster push (app owns the keyring; the UI only renders names and dispatches ids).
void ccord_ui_set_saved_accounts(ccord_ui *ui, const ccord_ui_saved_account *list, size_t count)
{
  if (!ui) return;
  if (count > CCORD_UI_SAVED_ACCOUNTS_MAX) count = CCORD_UI_SAVED_ACCOUNTS_MAX;
  bool changed = count != ui->saved_account_count;
  for (size_t i = 0; !changed && i < count; i++) {
    const ccord_ui_saved_account *a = &ui->saved_accounts[i];
    changed = !list || a->user_id != list[i].user_id || a->active != list[i].active ||
              strcmp(a->display_name, list[i].display_name) != 0 || strcmp(a->username, list[i].username) != 0;
  }
  if (!changed) return;
  ui->saved_account_count = count;
  for (size_t i = 0; i < count; i++) ui->saved_accounts[i] = list[i];
  ui->dirty = true;
}

// ---- hit-testing ----

static int clamp_volume(const ccord_rect *track, float x)
{
  if (track->w <= 0.0f) return 0;
  float t = (x - track->x) / track->w;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return (int)(t * 100.0f + 0.5f);
}

// ---- Voice & Video page scrolling --------------------------------------------------------------
// The page is taller than the panel; ui.c blocks wheel events for modals, so a scrollbar in the
// right margin (up/down buttons + track paging) drives the offset. Rows are drawn and hit-tested
// through a content rect shifted by -s_voice_scroll; the scrollbar itself stays fixed.
#define VOICE_ROWS 12
#define VOICE_DIAG_H 122.0f
#define VOICE_SCROLL_STEP 108.0f

static float voice_content_height(void)
{
  return 44.0f + (float)VOICE_ROWS * 54.0f + VOICE_DIAG_H + 8.0f;
}

static float voice_max_scroll(const sgeom *g)
{
  float m = voice_content_height() - g->content.h;
  return m > 0.0f ? m : 0.0f;
}

static ccord_rect voice_scroll_track(const sgeom *g)
{
  return (ccord_rect){g->content.x + g->content.w + 12.0f, g->content.y + 44.0f, 8.0f, g->content.h - 44.0f};
}

static ccord_rect voice_scroll_up_rect(const sgeom *g)
{
  ccord_rect t = voice_scroll_track(g);
  return (ccord_rect){t.x - 4.0f, t.y - 24.0f, 16.0f, 20.0f};
}

static ccord_rect voice_scroll_down_rect(const sgeom *g)
{
  ccord_rect t = voice_scroll_track(g);
  return (ccord_rect){t.x - 4.0f, t.y + t.h + 4.0f, 16.0f, 20.0f};
}

static float voice_scroll_thumb_h(const sgeom *g)
{
  ccord_rect t = voice_scroll_track(g);
  float h = t.h * (g->content.h / voice_content_height());
  return h < 24.0f ? 24.0f : h;
}

// g is the unshifted geometry. Returns true when the click landed on a scroll control.
static bool voice_scroll_click(ccord_ui *ui, const sgeom *g, float x, float y)
{
  float max = voice_max_scroll(g);
  if (max <= 0.0f) return false;
  ccord_rect up = voice_scroll_up_rect(g), down = voice_scroll_down_rect(g), tr = voice_scroll_track(g);
  ccord_scroller_cancel(&s_voice_glide);   // a scrollbar press replaces any wheel glide
  if (ui_in_rect(x, y, up)) {
    s_voice_scroll -= VOICE_SCROLL_STEP;
  } else if (ui_in_rect(x, y, down)) {
    s_voice_scroll += VOICE_SCROLL_STEP;
  } else if (ui_in_rect(x, y, tr)) {
    float th = voice_scroll_thumb_h(g);
    float ty = tr.y + (tr.h - th) * (s_voice_scroll / max);
    if (y < ty) s_voice_scroll -= VOICE_SCROLL_STEP;
    else s_voice_scroll += VOICE_SCROLL_STEP;
  } else {
    return false;
  }
  if (s_voice_scroll < 0.0f) s_voice_scroll = 0.0f;
  if (s_voice_scroll > max) s_voice_scroll = max;
  ui->dirty = true;
  return true;
}

// Wheel over the settings panel: the Voice & Video page and the M6 list pages ease one notch per
// event (108 px, the scrollbar step). Returns true when the page moved.
bool ui_settings_handle_wheel(ccord_ui *ui, const ccord_input_event *ev)
{
  sgeom g = geometry(ui);
  if (!ui_in_rect(ev->x, ev->y, g.panel)) return false;
  if (g.list.w > 0.0f && ui_in_rect(ev->x, ev->y, g.list)) {
    s_sidebar_scroll -= ev->wheel * g.step;
    if (s_sidebar_scroll < 0.0f) s_sidebar_scroll = 0.0f;
    (void)geometry(ui);   // clamps s_sidebar_scroll to max_scroll
    ui->dirty = true;
    return true;
  }
  if (ui->settings_page == SP_VOICE) {
    float max = voice_max_scroll(&g);
    if (max <= 0.0f) return false;
    ccord_scroller_scroll(&s_voice_glide, &s_voice_scroll, -ev->wheel * VOICE_SCROLL_STEP, max, ui_wheel_direct(ui, ev));
    ui->dirty = true;
    return true;
  }
  if (ui->settings_page == SP_PROFILE_EDIT) return ui_settings_profile_wheel(ui, ev->wheel);
  if (ui->settings_page == SP_PRIVACY || ui->settings_page == SP_DEVICES || ui->settings_page == SP_CONNECTIONS) {
    return ui_settings_m6_wheel(ui, ui->settings_page - SP_PRIVACY, g.content, ev->wheel, ui_wheel_direct(ui, ev));
  }
  return false;
}

// Ticks the Voice & Video glide and the M6 list-page glides by fdt_ms (ccord_ui_update). Glides stop with the modal.
bool ui_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  bool moving = ui_settings_m6_scroll_tick(ui, fdt_ms, reduced);
  if (ui_settings_profile_tick(ui)) moving = true;   // Discord's answer to Save, and the brief success state
  if (s_save_pending && s_save_ui == ui) {
    if (!ui->settings_open || ui->now_ms >= s_save_due_ms) {
      settings_save(ui);
    } else {
      moving = true;
    }
  }
  if (!ui->settings_open) {
    s_hover_key = -1;
    s_last_drawn_page = -1;
  }
  if (s_voice_glide.pending == 0.0f) return moving;
  if (!ui->settings_open || ui->settings_page != SP_VOICE) {
    ccord_scroller_cancel(&s_voice_glide);
    return moving;
  }
  sgeom g = geometry(ui);
  moving = ccord_scroller_tick(&s_voice_glide, &s_voice_scroll, fdt_ms, voice_max_scroll(&g), reduced) || moving;
  ui->dirty = true;
  return moving;
}

// Voice preferences are local (never Discord state): they are stored first, then the effective
// config is forwarded to the session so the media plane picks it up. DSP toggles only take effect
// when the speexdsp backend is built in; without it the chain is a passthrough, so they are masked.
static void voice_config_apply(ccord_ui *ui)
{
  ui->dirty = true;
  if (!ui->d.session) return;
  bool dsp = ccord_dsp_available();
  ccord_voice_config cfg = {
    .ptt = ui->settings_voice_mode == 1,
    .vad_sensitivity_pct = (uint32_t)ui->settings_vad_sensitivity,
    .ns = ui->settings_noise_suppression && dsp,
    .aec = ui->settings_echo_cancel && dsp,
    .agc = ui->settings_auto_gain && dsp,
  };
  ccord_error err;
  if (ccord_session_set_voice_config(ui->d.session, &cfg, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't apply voice settings: %s", err.message);
  }
}

static char s_plugin_log_id[65];
static char s_plugin_consent_id[65];
static char s_plugin_detail_id[65];
static char s_plugin_status_msg[192];
static char s_plugin_search[96];
static bool s_plugin_search_focused;
static int s_plugin_subtab;       // 0=Installed, 1=Discover, 2=Updates
static int s_plugin_tier_filter;  // 0=All, 1=Official, 2=Verified, 3=Unverified
static int s_plugin_sort_idx;     // 0=popular, 1=trending, 2=tier, 3=updated, 4=name
static bool s_plugin_class1_only;

static const char *plugin_sort_key(int idx)
{
  static const char *const k_keys[5] = {"popular", "trending", "tier", "updated", "name"};
  return (idx >= 0 && idx < 5) ? k_keys[idx] : "popular";
}

static const char *plugin_sort_label(int idx)
{
  static const char *const k_lbls[5] = {"Popular", "Trending", "Tier", "Updated", "A-Z"};
  return (idx >= 0 && idx < 5) ? k_lbls[idx] : "Popular";
}

static void trigger_plugin_discover_query(ccord_ui *ui, bool append_next_page)
{
  if (!ui || !ui->d.contrib) return;
  ccord_contrib_catalog_query(ui->d.contrib, s_plugin_search, s_plugin_tier_filter,
                              s_plugin_class1_only ? 1 : 0, plugin_sort_key(s_plugin_sort_idx),
                              append_next_page);
}

typedef struct plugin_card_rects {
  ccord_rect card;
  ccord_rect btn_toggle;
  ccord_rect btn_log;
  ccord_rect btn_reload;
  ccord_rect btn_export;
  ccord_rect btn_update;
  ccord_rect btn_auto;
  ccord_rect btn_uninstall;
} plugin_card_rects;

static const char *plugin_tier_label(ccord_plugin_tier t)
{
  switch (t) {
    case CCORD_TIER_OFFICIAL:   return "[Official]";
    case CCORD_TIER_VERIFIED:   return "[Verified]";
    case CCORD_TIER_UNVERIFIED: return "[Unverified]";
    default:                    return "";
  }
}

static ccord_color plugin_tier_color(const ccord_ui *ui, ccord_plugin_tier t)
{
  switch (t) {
    case CCORD_TIER_OFFICIAL:   return COL(brand);
    case CCORD_TIER_VERIFIED:   return COL(status_online);
    default:                    return COL(text_muted);
  }
}

static const char *plugin_api_class_label(uint8_t api_class)
{
  if (api_class >= 3) return "[API Class 3]";
  if (api_class == 2) return "[API Class 2]";
  return "[API Class 1]";
}

static ccord_color plugin_api_class_color(const ccord_ui *ui, uint8_t api_class)
{
  if (api_class >= 3) return COL(danger);
  if (api_class == 2) return COL(status_idle);
  return COL(status_online);
}

static void plugin_subtab_rects(const sgeom *g, ccord_rect *tab_inst, ccord_rect *tab_disc,
                                ccord_rect *tab_upd, ccord_rect *btn_sync)
{
  float x = g->content.x, y = g->content.y + 84.0f, w = g->content.w;
  *tab_inst = (ccord_rect){x, y, 118.0f, UIS_H_BTN_SM};
  *tab_disc = (ccord_rect){x + 126.0f, y, 108.0f, UIS_H_BTN_SM};
  *tab_upd  = (ccord_rect){x + 242.0f, y, 116.0f, UIS_H_BTN_SM};
  *btn_sync = (ccord_rect){x + w - 128.0f, y, 128.0f, UIS_H_BTN_SM};
}

static float plugin_card_layout(const sgeom *g, size_t idx, const ccord_plugin_info *p, float y, plugin_card_rects *out)
{
  (void)idx;
  memset(out, 0, sizeof *out);
  bool has_err = p->error && p->error[0];
  bool has_warn = p->warning_sev != CCORD_WARN_NONE && p->warning_title && p->warning_title[0];
  bool has_upd = p->update_version && p->update_version[0];
  float h = 104.0f + (has_err ? 20.0f : 0.0f) + (has_warn ? 20.0f : 0.0f);
  float x = g->content.x, w = g->content.w;
  float by = y + h - 34.0f;
  out->card = (ccord_rect){x, y, w, h};
  out->btn_toggle = (ccord_rect){x + 14.0f, by, p->state == CCORD_PLUGIN_NEEDS_CONSENT ? 120.0f : 84.0f, UIS_H_BTN_SM};
  float bx = out->btn_toggle.x + out->btn_toggle.w + 6.0f;
  out->btn_log = (ccord_rect){bx, by, 108.0f, UIS_H_BTN_SM};
  bx += out->btn_log.w + 6.0f;
  out->btn_reload = (ccord_rect){bx, by, 78.0f, UIS_H_BTN_SM};
  bx += out->btn_reload.w + 6.0f;
  out->btn_export = (ccord_rect){bx, by, 106.0f, UIS_H_BTN_SM};
  bx += out->btn_export.w + 6.0f;
  if (has_upd && bx + 106.0f <= x + w - 14.0f) {
    out->btn_update = (ccord_rect){bx, by, 106.0f, UIS_H_BTN_SM};
    bx += out->btn_update.w + 6.0f;
  }
  if (!p->is_bundled) {
    float rx = x + w - 14.0f;
    if (rx - 92.0f >= bx) {
      rx -= 92.0f;
      out->btn_uninstall = (ccord_rect){rx, by, 92.0f, UIS_H_BTN_SM};
      rx -= 6.0f;
    }
    if (rx - 94.0f >= bx) {
      rx -= 94.0f;
      out->btn_auto = (ccord_rect){rx, by, 94.0f, UIS_H_BTN_SM};
    }
  }
  return h;
}

static bool handle_page_controls(ccord_ui *ui, const sgeom *g, float x, float y)
{
  switch (ui->settings_page) {
    case SP_PROFILE_EDIT:
      return ui_settings_profile_click(ui, g->content, g->panel, x, y);
    case SP_APPEARANCE:
      if (ui->account_focus_field == 6 && !ui_in_rect(x, y, quickcss_box_rect(g))) {
        ui->account_focus_field = 0;   // clicking away commits the snippet editor
        settings_save_flush(ui);
        ui->dirty = true;
      }
      if (ui_settings_m6_appearance_click(ui, g->content, x, y)) return true;   // theme chips + zoom
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 0, i, 2))) {
          ui->settings_display_mode = i;
          ui->dirty = true;
          return true;
        }
      }
      if (ui_in_rect(x, y, toggle_rect(g, 5))) {
        ui->settings_quickcss_enabled = !ui->settings_quickcss_enabled;
        ui_quickcss_changed(ui);   // live apply through the host + persist
        settings_save_flush(ui);
        return true;
      }
      if (ui_in_rect(x, y, quickcss_box_rect(g))) {
        ui->account_focus_field = 6;   // sentinel: routes key/text events to the snippet editor
        ccord_text_edit_set_caret(&ui->quickcss_edit, ui->quickcss_edit.len, false);
        ui->dirty = true;
        return false;   // focusing the box does not modify any persisted setting
      }
      if (ui_in_rect(x, y, toggle_rect(g, 7))) {
        ui->settings_reduced_motion = !ui->settings_reduced_motion;
        ui->anim.reduced_motion = ui->settings_reduced_motion;
        ui->dirty = true;
        return true;
      }
      break;
    case SP_VOICE: {
      // g arrives shifted by -s_voice_scroll. The fixed scrollbar lives in the right margin
      // (outside the content width), so answer it before clipping clicks to the content box.
      sgeom gu = *g;
      gu.content.y += s_voice_scroll;   // unshifted geometry for the fixed scrollbar
      if (voice_scroll_click(ui, &gu, x, y)) return false;   // scrolling does not change settings.ini
      if (!ui_in_rect(x, y, gu.content)) break;   // rows only answer inside the visible content box
      // Rows 0-8 keep the original Voice & Video layout; the M5 additions (camera, PTT key,
      // sensitivity, diagnostics) follow at rows 9-12 and are reached by scrolling.
      if (ui_in_rect(x, y, device_rect(g, 0))) {
        voice_devices_refresh(ui, true);
        voice_select_input(ui, (ui->settings_mic_device + 1) % voice_in_count());
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, device_rect(g, 1))) {
        voice_devices_refresh(ui, true);
        voice_select_output(ui, (ui->settings_speaker_device + 1) % voice_out_count());
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, vol_minus_rect(g, 2))) {
        ui->settings_input_volume = ui->settings_input_volume > 5 ? ui->settings_input_volume - 5 : 0;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, vol_plus_rect(g, 2))) {
        ui->settings_input_volume = ui->settings_input_volume < 95 ? ui->settings_input_volume + 5 : 100;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, vol_track_rect(g, 2))) {
        ccord_rect tr = vol_track_rect(g, 2);
        ui->settings_input_volume = clamp_volume(&tr, x);
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, vol_minus_rect(g, 3))) {
        ui->settings_output_volume = ui->settings_output_volume > 5 ? ui->settings_output_volume - 5 : 0;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, vol_plus_rect(g, 3))) {
        ui->settings_output_volume = ui->settings_output_volume < 95 ? ui->settings_output_volume + 5 : 100;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, vol_track_rect(g, 3))) {
        ccord_rect tr = vol_track_rect(g, 3);
        ui->settings_output_volume = clamp_volume(&tr, x);
        ui->dirty = true;
        return true;
      }
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 4, i, 2))) {
          ui->settings_voice_mode = i;
          voice_config_apply(ui);
          return true;
        }
      }
      bool dsp = ccord_dsp_available();
      if (dsp && ui_in_rect(x, y, toggle_rect(g, 5))) {
        ui->settings_noise_suppression = !ui->settings_noise_suppression;
        voice_config_apply(ui);
        return true;
      }
      if (dsp && ui_in_rect(x, y, toggle_rect(g, 6))) {
        ui->settings_echo_cancel = !ui->settings_echo_cancel;
        voice_config_apply(ui);
        return true;
      }
      if (dsp && ui_in_rect(x, y, toggle_rect(g, 7))) {
        ui->settings_auto_gain = !ui->settings_auto_gain;
        voice_config_apply(ui);
        return true;
      }
      if (ui_in_rect(x, y, toggle_rect(g, 8))) {
        ui->settings_hardware_accel = !ui->settings_hardware_accel;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, device_rect(g, 9))) {
        voice_devices_refresh(ui, true);
        if (s_dev.n_cam > 0) {
          voice_select_camera(ui, (ui->settings_camera_device + 1) % voice_cam_count());
          ui->dirty = true;
          return true;
        }
        return false;
      }
      if (ui_in_rect(x, y, device_rect(g, 10))) {   // Push to Talk key: arm live capture
        s_ptt_armed = true;
        ui->account_focus_field = 5;   // sentinel: routes key events to this modal (ui.c handle_key)
        ui->dirty = true;
        return false;
      }
      if (ui->settings_voice_mode == 0) {   // sensitivity only applies to voice activity
        if (ui_in_rect(x, y, vol_minus_rect(g, 11))) {
          ui->settings_vad_sensitivity = ui->settings_vad_sensitivity > 5 ? ui->settings_vad_sensitivity - 5 : 0;
          voice_config_apply(ui);
          return true;
        }
        if (ui_in_rect(x, y, vol_plus_rect(g, 11))) {
          ui->settings_vad_sensitivity = ui->settings_vad_sensitivity < 95 ? ui->settings_vad_sensitivity + 5 : 100;
          voice_config_apply(ui);
          return true;
        }
        if (ui_in_rect(x, y, vol_track_rect(g, 11))) {
          ccord_rect tr = vol_track_rect(g, 11);
          ui->settings_vad_sensitivity = clamp_volume(&tr, x);
          voice_config_apply(ui);
          return true;
        }
      }
      break;
    }
    case SP_KEYBINDS: {
      // The Push to Talk row jumps to Voice & Video and arms the same live capture.
      ccord_rect b = keybind_rect(g, 0);
      if (ui_in_rect(x, y, b)) {
        ui->settings_page = SP_VOICE;
        ui->settings_category = k_pages[SP_VOICE].category;
        s_ptt_armed = true;
        ui->account_focus_field = 5;
        ui->dirty = true;
        return true;
      }
      // Mute / Deafen / Quick Switcher rows arm an in-place capture (rows 1..3).
      for (int row = 1; row <= 3; row++) {
        ccord_rect kb = keybind_rect(g, row);
        if (ui_in_rect(x, y, kb)) {
          s_kb_armed = row - 1;
          ui->account_focus_field = 5;   // sentinel: routes key events to this modal (ui.c handle_key)
          ui->dirty = true;
          return true;
        }
      }
      break;
    }
    case SP_STREAMING: {
      ccord_rect auto_enc = auto_pill_rect(g, 0);
      ccord_rect auto_fw = auto_pill_rect(g, 1);
      if (ui_in_rect(x, y, auto_enc)) {
        ui->settings_encoder = (int)CCORD_STREAM_ENCODER_AUTO;
        ui->gl_encoder = (int)CCORD_STREAM_ENCODER_AUTO;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, auto_fw)) {
        ui->settings_framework = (int)CCORD_STREAM_FRAMEWORK_AUTO;
        ui->gl_framework = (int)CCORD_STREAM_FRAMEWORK_AUTO;
        ui->dirty = true;
        return true;
      }
      for (int i = 0; i < STREAM_ENC_COUNT; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 0, i, STREAM_ENC_COUNT))) {
          ui->settings_encoder = k_encoder_vals[i];
          ui->gl_encoder = k_encoder_vals[i];   // Go Live starts from these defaults
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < STREAM_FW_COUNT; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 1, i, STREAM_FW_COUNT))) {
          ui->settings_framework = k_framework_vals[i];
          ui->gl_framework = k_framework_vals[i];
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 4; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 2, i, 4))) {
          ui->settings_resolution = i;
          ui->gl_resolution = i;
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 3, i, 3))) {
          ui->settings_fps = i;
          ui->gl_fps = i;
          ui->dirty = true;
          return true;
        }
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 4, i, 3))) {
          ui->settings_codec = i;
          ui->gl_codec = i;
          ui->dirty = true;
          return true;
        }
      }
      break;
    }
    case SP_TEXT_IMAGES:
      if (ui_in_rect(x, y, toggle_rect(g, 0))) {
        ui->settings_embeds = !ui->settings_embeds;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(x, y, toggle_rect(g, 1))) {
        ui->settings_attachments = !ui->settings_attachments;
        ui->dirty = true;
        return true;
      }
      break;
    case SP_NOTIFICATIONS:
      // M6 rework: body, capability gating and the /tts PATCH live in settings_m6_prefs.c.
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_NOTIFICATIONS, g->content, g->panel, x, y);
    case SP_ACCESSIBILITY:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_ACCESSIBILITY, g->content, g->panel, x, y);
    case SP_STREAMER:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_STREAMER, g->content, g->panel, x, y);
    case SP_LANGUAGE:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_LANGUAGE, g->content, g->panel, x, y);
    case SP_ADVANCED:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_ADVANCED, g->content, g->panel, x, y);
    case SP_CCORD:
      return ui_settings_m6_prefs_click(ui, CCORD_M6_PREFS_CCORD, g->content, g->panel, x, y);
    case SP_PRIVACY:
    case SP_DEVICES:
    case SP_CONNECTIONS:
      // M6 account pages: bodies, scrolling and confirmations live in settings_m6_devices.c.
      return ui_settings_m6_click(ui, ui->settings_page - SP_PRIVACY, g->content, g->panel, x, y);
    case SP_PLUGINS: {
      if (ui_in_rect(x, y, extensions_toggle_rect(g))) {
        set_extensions(ui, !ui->extensions_on);
        return true;
      }
      ccord_rect r_inst, r_disc, r_upd, r_sync;
      plugin_subtab_rects(g, &r_inst, &r_disc, &r_upd, &r_sync);
      if (ui_in_rect(x, y, r_inst)) {
        s_plugin_subtab = 0; s_plugin_search_focused = false;
        s_plugin_log_id[0] = '\0'; s_plugin_consent_id[0] = '\0'; s_plugin_detail_id[0] = '\0';
        return true;
      }
      if (ui_in_rect(x, y, r_disc)) {
        s_plugin_subtab = 1; s_plugin_search_focused = false;
        s_plugin_log_id[0] = '\0'; s_plugin_consent_id[0] = '\0'; s_plugin_detail_id[0] = '\0';
        const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
        if (cst && !cst->has_loaded && !cst->syncing) trigger_plugin_discover_query(ui, false);
        return true;
      }
      if (ui_in_rect(x, y, r_upd)) {
        s_plugin_subtab = 2; s_plugin_search_focused = false;
        s_plugin_log_id[0] = '\0'; s_plugin_consent_id[0] = '\0'; s_plugin_detail_id[0] = '\0';
        ccord_contrib_catalog_check_updates(ui->d.contrib);
        return true;
      }
      if (ui_in_rect(x, y, r_sync)) {
        ccord_contrib_catalog_refresh(ui->d.contrib);
        return true;
      }

      float px = g->content.x, py = g->content.y + 120.0f, pw = g->content.w;
      if (s_plugin_log_id[0]) {
        if (ui_in_rect(x, y, (ccord_rect){px + pw - 246.0f, py - 4.0f, 74.0f, UIS_H_BTN_SM})) {
          ccord_contrib_plugin_reload(ui->d.contrib, s_plugin_log_id);
          return true;
        }
        if (ui_in_rect(x, y, (ccord_rect){px + pw - 164.0f, py - 4.0f, 74.0f, UIS_H_BTN_SM})) {
          ccord_contrib_clear_plugin_log(ui->d.contrib, s_plugin_log_id);
          return true;
        }
        if (ui_in_rect(x, y, (ccord_rect){px + pw - 82.0f, py - 4.0f, 82.0f, UIS_H_BTN_SM})) {
          s_plugin_log_id[0] = '\0';
          return true;
        }
        return false;
      }
      if (s_plugin_consent_id[0]) {
        ccord_rect box = {px, py, pw, 224.0f};
        if (ui_in_rect(x, y, (ccord_rect){box.x + 16.0f, box.y + box.h - 46.0f, 184.0f, UIS_H_BTN})) {
          ccord_contrib_plugin_consent_accept(ui->d.contrib, s_plugin_consent_id);
          s_plugin_consent_id[0] = '\0';
          return true;
        }
        if (ui_in_rect(x, y, (ccord_rect){box.x + 212.0f, box.y + box.h - 46.0f, 96.0f, UIS_H_BTN})) {
          s_plugin_consent_id[0] = '\0';
          return true;
        }
        return false;
      }
      if (s_plugin_detail_id[0]) {
        const ccord_catalog_entry *e = ccord_contrib_catalog_detail(ui->d.contrib);
        if (!e || strcmp(e->id, s_plugin_detail_id) != 0) {
          const ccord_catalog_entry *fb = ccord_contrib_catalog_find(ui->d.contrib, s_plugin_detail_id);
          e = (fb && fb->has_detail) ? fb : nullptr;
        }
        if (ui_in_rect(x, y, (ccord_rect){px, py, 86.0f, UIS_H_BTN_SM})) {
          s_plugin_detail_id[0] = '\0';
          return true;
        }
        if (!e) {
          ccord_rect box = {px, py + 34.0f, pw, 128.0f};
          if (ui_in_rect(x, y, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 120.0f, UIS_H_BTN})) {
            ccord_contrib_catalog_fetch_detail(ui->d.contrib, s_plugin_detail_id);
            return true;
          }
          return false;
        }
        ccord_rect box = {px, py + 34.0f, pw, 286.0f};
        if (!e->bundled && !e->installing && (!e->installed || e->update_available)) {
          if (ui_in_rect(x, y, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN})) {
            ccord_contrib_catalog_install(ui->d.contrib, e->id);
            return true;
          }
        }
        if (e->installed && !e->bundled) {
          float ux = box.x + ((!e->installed || e->update_available) ? 180.0f : 16.0f);
          if (ui_in_rect(x, y, (ccord_rect){ux, box.y + box.h - 44.0f, 96.0f, UIS_H_BTN})) {
            char eb[128] = "";
            if (ccord_contrib_plugin_uninstall(ui->d.contrib, e->id, eb, sizeof eb)) {
              snprintf(s_plugin_status_msg, sizeof s_plugin_status_msg, "Uninstalled %s", e->name);
              s_plugin_detail_id[0] = '\0';
            } else {
              snprintf(s_plugin_status_msg, sizeof s_plugin_status_msg, "Uninstall failed: %.150s", eb);
            }
            return true;
          }
        }
        return false;
      }

      if (s_plugin_subtab == 1) {
        // Row 1: Search input + Search button + Sort toggle
        ccord_rect r_q = {px, py, pw - 196.0f, UIS_H_BTN_SM};
        ccord_rect r_go = {px + pw - 188.0f, py, 74.0f, UIS_H_BTN_SM};
        ccord_rect r_sort = {px + pw - 106.0f, py, 106.0f, UIS_H_BTN_SM};
        if (ui_in_rect(x, y, r_q)) {
          s_plugin_search_focused = true;
          return true;
        }
        s_plugin_search_focused = false;
        if (ui_in_rect(x, y, r_go)) {
          trigger_plugin_discover_query(ui, false);
          return true;
        }
        if (ui_in_rect(x, y, r_sort)) {
          s_plugin_sort_idx = (s_plugin_sort_idx + 1) % 5;
          trigger_plugin_discover_query(ui, false);
          return true;
        }
        py += 34.0f;

        // Row 2: Tier filters + API Class 1 toggle (server-side query)
        float fx = px;
        static const float k_fw[4] = {54.0f, 76.0f, 76.0f, 88.0f};
        for (int t = 0; t < 4; t++) {
          if (ui_in_rect(x, y, (ccord_rect){fx, py, k_fw[t], UIS_H_BTN_SM})) {
            s_plugin_tier_filter = t;
            trigger_plugin_discover_query(ui, false);
            return true;
          }
          fx += k_fw[t] + 6.0f;
        }
        if (ui_in_rect(x, y, (ccord_rect){fx + 4.0f, py, 126.0f, UIS_H_BTN_SM})) {
          s_plugin_class1_only = !s_plugin_class1_only;
          trigger_plugin_discover_query(ui, false);
          return true;
        }
        py += 36.0f;

        const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
        float bottom_lim = g->panel.y + g->panel.h - (cst && cst->has_more ? 52.0f : 16.0f);
        size_t cn = ui->d.contrib ? ccord_contrib_catalog_count(ui->d.contrib) : 0;
        for (size_t i = 0; i < cn; i++) {
          const ccord_catalog_entry *e = ccord_contrib_catalog_at(ui->d.contrib, i);
          if (!e) continue;
          bool has_w = e->warning_sev != CCORD_WARN_NONE && e->warning_title && e->warning_title[0];
          float ch = 98.0f + (has_w ? 20.0f : 0.0f);
          if (py + ch > bottom_lim) break;
          ccord_rect btn_det = {px + pw - 200.0f, py + ch - 34.0f, 76.0f, UIS_H_BTN_SM};
          ccord_rect btn_act = {px + pw - 116.0f, py + ch - 34.0f, 102.0f, UIS_H_BTN_SM};
          if (ui_in_rect(x, y, btn_det)) {
            snprintf(s_plugin_detail_id, sizeof s_plugin_detail_id, "%s", e->id);
            ccord_contrib_catalog_fetch_detail(ui->d.contrib, e->id);
            return true;
          }
          if (ui_in_rect(x, y, btn_act) && !e->installing && !e->bundled) {
            if (!e->installed || e->update_available) {
              ccord_contrib_catalog_install(ui->d.contrib, e->id);
              return true;
            }
          }
          py += ch + 10.0f;
        }
        if (cst && cst->has_more && !cst->loading_more) {
          ccord_rect r_more = {px, g->panel.y + g->panel.h - 44.0f, 148.0f, UIS_H_BTN_SM};
          if (ui_in_rect(x, y, r_more)) {
            trigger_plugin_discover_query(ui, true);
            return true;
          }
        }
        return false;
      }

      if (s_plugin_subtab == 2) {
        // Updates & Settings tab clicks
        const ccord_catalog_prefs *pr_cur = ccord_contrib_catalog_prefs(ui->d.contrib);
        ccord_catalog_prefs pr = pr_cur ? *pr_cur : (ccord_catalog_prefs){.auto_update_enabled = false, .auto_update_min_tier = 2};
        ccord_rect box = {px, py, pw, 142.0f};
        ccord_rect sw_row = {box.x + 14.0f, box.y + 12.0f, box.w - 28.0f, 36.0f};
        ccord_rect sw_hit = {sw_row.x + sw_row.w - 48.0f, sw_row.y + 4.0f, 44.0f, 26.0f};
        if (ui_in_rect(x, y, sw_hit)) {
          pr.auto_update_enabled = !pr.auto_update_enabled;
          ccord_contrib_catalog_set_prefs(ui->d.contrib, &pr);
          return true;
        }
        float tx = box.x + 14.0f, ty = box.y + 68.0f;
        static const float k_tw[3] = {104.0f, 134.0f, 118.0f};
        for (int k = 0; k < 3; k++) {
          if (ui_in_rect(x, y, (ccord_rect){tx, ty, k_tw[k], UIS_H_BTN_SM})) {
            pr.auto_update_min_tier = (uint8_t)(k + 1);
            ccord_contrib_catalog_set_prefs(ui->d.contrib, &pr);
            return true;
          }
          tx += k_tw[k] + 8.0f;
        }
        if (ui_in_rect(x, y, (ccord_rect){box.x + box.w - 178.0f, box.y + box.h - 36.0f, 164.0f, UIS_H_BTN_SM})) {
          ccord_contrib_catalog_run_auto_updates(ui->d.contrib);
          return true;
        }
        py += box.h + 28.0f;
        size_t un = ui->d.contrib ? ccord_contrib_catalog_update_count(ui->d.contrib) : 0;
        for (size_t i = 0; i < un; i++) {
          const ccord_catalog_entry *e = ccord_contrib_catalog_update_at(ui->d.contrib, i);
          if (!e || !e->update_available) continue;
          float ch = 72.0f;
          if (py + ch > g->panel.y + g->panel.h - 16.0f) break;
          ccord_rect btn_u = {px + pw - 134.0f, py + 20.0f, 120.0f, UIS_H_BTN_SM};
          if (ui_in_rect(x, y, btn_u) && !e->installing) {
            if (e->update_needs_review) {
              snprintf(s_plugin_detail_id, sizeof s_plugin_detail_id, "%s", e->id);
              ccord_contrib_catalog_fetch_detail(ui->d.contrib, e->id);
            } else {
              ccord_contrib_catalog_install(ui->d.contrib, e->id);
            }
            return true;
          }
          py += ch + 8.0f;
        }
        return false;
      }

      // Tab 0: Installed
      py += 24.0f;
      size_t n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
      for (size_t i = 0; i < n; i++) {
        const ccord_plugin_info *p = ccord_contrib_plugin_at(ui->d.contrib, i);
        if (!p) continue;
        plugin_card_rects rc;
        float h = plugin_card_layout(g, i, p, py, &rc);
        if (py + h > g->panel.y + g->panel.h - 16.0f) break;
        if (ui_in_rect(x, y, rc.btn_toggle)) {
          if (p->state == CCORD_PLUGIN_NEEDS_CONSENT) {
            snprintf(s_plugin_consent_id, sizeof s_plugin_consent_id, "%s", p->id);
          } else {
            bool on = !(p->state == CCORD_PLUGIN_READY || p->state == CCORD_PLUGIN_STARTING);
            ccord_contrib_plugin_set_enabled(ui->d.contrib, p->id, on);
          }
          return true;
        }
        if (ui_in_rect(x, y, rc.btn_log)) {
          snprintf(s_plugin_log_id, sizeof s_plugin_log_id, "%s", p->id);
          return true;
        }
        if (ui_in_rect(x, y, rc.btn_reload)) {
          ccord_contrib_plugin_reload(ui->d.contrib, p->id);
          return true;
        }
        if (ui_in_rect(x, y, rc.btn_export)) {
          const char *home = getenv("HOME");
          char dest[512], ebuf[128] = "";
          snprintf(dest, sizeof dest, "%s/%s-%s.cce", home && home[0] ? home : ".", p->id, p->version);
          if (ccord_contrib_plugin_export_cce(ui->d.contrib, p->id, dest, ebuf, sizeof ebuf)) {
            snprintf(s_plugin_status_msg, sizeof s_plugin_status_msg, "Exported: %.175s", dest);
          } else {
            snprintf(s_plugin_status_msg, sizeof s_plugin_status_msg, "Export failed: %.170s", ebuf);
          }
          return true;
        }
        if (rc.btn_update.w > 0.0f && ui_in_rect(x, y, rc.btn_update)) {
          if (p->update_needs_review) {
            snprintf(s_plugin_detail_id, sizeof s_plugin_detail_id, "%s", p->id);
            ccord_contrib_catalog_fetch_detail(ui->d.contrib, p->id);
          } else {
            ccord_contrib_catalog_install(ui->d.contrib, p->id);
          }
          return true;
        }
        if (rc.btn_auto.w > 0.0f && ui_in_rect(x, y, rc.btn_auto)) {
          ccord_contrib_catalog_set_plugin_auto_update(ui->d.contrib, p->id, !p->auto_update);
          return true;
        }
        if (rc.btn_uninstall.w > 0.0f && ui_in_rect(x, y, rc.btn_uninstall)) {
          char ebuf[128] = "";
          if (ccord_contrib_plugin_uninstall(ui->d.contrib, p->id, ebuf, sizeof ebuf)) {
            snprintf(s_plugin_status_msg, sizeof s_plugin_status_msg, "Uninstalled %s", p->name);
          } else {
            snprintf(s_plugin_status_msg, sizeof s_plugin_status_msg, "Uninstall failed: %.150s", ebuf);
          }
          return true;
        }
        py += h + 10.0f;
      }
      break;
    }
    default:
      break;
  }
  return false;
}

// Shared keyboard editing for single-line login/account fields (supports Copy/Cut/Paste/Select All/arrows).
static bool handle_account_field_edit_key(ccord_ui *ui, ccord_text_edit *active, bool masked,
                                          const ccord_input_event *ev)
{
  if (!active || ev->type != CCORD_IN_KEY_DOWN) return false;
  bool mod = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  if (ev->key == CCORD_KEY_BACKSPACE) {
    if (mod) ccord_text_edit_backspace_word(active);
    else ccord_text_edit_backspace(active);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_DELETE) {
    if (mod) ccord_text_edit_delete_word(active);
    else ccord_text_edit_delete(active);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_LEFT) {
    ccord_text_edit_move(active, mod ? -3 : -1, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_RIGHT) {
    ccord_text_edit_move(active, mod ? 3 : 1, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_HOME) {
    ccord_text_edit_move(active, -2, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (ev->key == CCORD_KEY_END) {
    ccord_text_edit_move(active, 2, (ev->mods & CCORD_MOD_SHIFT) != 0);
    ui->dirty = true;
    return true;
  }
  if (mod && ev->key == CCORD_KEY_A) {
    ccord_text_edit_select_all(active);
    ui->dirty = true;
    return true;
  }
  if (mod && !masked && (ev->key == CCORD_KEY_C || ev->key == CCORD_KEY_X) && ui->d.host.clipboard_set) {
    char *sel = ccord_text_edit_copy_selection(active);
    if (sel) {
      ui->d.host.clipboard_set(ui->d.host.ud, sel);
      ccord_free(sel);
      if (ev->key == CCORD_KEY_X) ccord_text_edit_backspace(active);
    } else if (active->len > 0) {
      ui->d.host.clipboard_set(ui->d.host.ud, ccord_text_edit_cstr(active));
      if (ev->key == CCORD_KEY_X) ccord_text_edit_clear(active);
    }
    ui->dirty = true;
    return true;
  }
  if (mod && ev->key == CCORD_KEY_V && ui->d.host.clipboard_get) {
    char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
    if (clip) {
      ccord_error ierr;
      (void)ccord_text_edit_insert(active, clip, strlen(clip), &ierr);
      ccord_free(clip);
    }
    ui->dirty = true;
    return true;
  }
  return false;
}

static int64_t settings_hover_key(const ccord_ui *ui, const sgeom *g, float x, float y)
{
  if (g->back.w > 0.0f && ui_in_rect(x, y, g->back)) return 1;
  if (g->sb_up.w > 0.0f) {
    if (ui_in_rect(x, y, g->sb_up)) return 2;
    if (ui_in_rect(x, y, g->sb_down)) return 3;
  }
  if (ui->mobile_layout && !ui->mobile_settings_detail) return 0;

  switch (ui->settings_page) {
    case SP_ACCOUNT: {
      for (int i = 0; i < 5; i++) {
        if (ui_in_rect(x, y, account_web_btn_rect(g, i))) return 100 + i;
      }
      if (!ui->account_view.signed_in) {
        if (ui_in_rect(x, y, account_email_box_rect(g))) return 110;
        if (ui_in_rect(x, y, account_password_box_rect(g))) return 111;
        if (ui_in_rect(x, y, account_login_btn_rect(g))) return 112;
      }
      if (ui_in_rect(x, y, account_token_box_rect(g))) return 113;
      if (ui_in_rect(x, y, account_token_paste_rect(g))) return 114;
      if (ui_in_rect(x, y, account_token_save_rect(g))) return 115;
      if (ui_in_rect(x, y, account_token_clear_rect(g))) return 116;
      if (ui_in_rect(x, y, account_add_btn_rect(g))) return 117;
      for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
        if (ui_in_rect(x, y, saved_account_switch_rect(g, (int)i))) return 120 + (int64_t)i * 2;
        if (ui_in_rect(x, y, saved_account_remove_rect(g, (int)i))) return 120 + (int64_t)i * 2 + 1;
      }
      ccord_rect primary, secondary;
      account_buttons(ui, g, &primary, &secondary);
      if (ui_in_rect(x, y, primary)) return 140;
      if (ui_in_rect(x, y, secondary)) return 141;
      return 0;
    }
    case SP_APPEARANCE: {
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 0, i, 2))) return 200 + i;
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 1, i, 3))) return 210 + i;
      }
      if (ui_in_rect(x, y, vol_minus_rect(g, 2))) return 220;
      if (ui_in_rect(x, y, vol_plus_rect(g, 2))) return 221;
      if (ui_in_rect(x, y, vol_track_rect(g, 2))) return 222;
      ccord_rect reset_r = {g->content.x + 324.0f, row_y(g, 2) + 18.0f, 64.0f, UIS_H_BTN_SM};
      if (ui_in_rect(x, y, reset_r)) return 223;
      if (ui_in_rect(x, y, toggle_rect(g, 5))) return 225;
      if (ui_in_rect(x, y, quickcss_box_rect(g))) return 226;
      if (ui_in_rect(x, y, toggle_rect(g, 7))) return 227;
      return 0;
    }
    case SP_VOICE: {
      if (voice_max_scroll(g) > 0.0f) {
        if (ui_in_rect(x, y, voice_scroll_up_rect(g))) return 300;
        if (ui_in_rect(x, y, voice_scroll_down_rect(g))) return 301;
        if (ui_in_rect(x, y, voice_scroll_track(g))) return 302;
      }
      if (!ui_in_rect(x, y, g->content)) return 0;
      sgeom gv = *g;
      gv.content.y -= s_voice_scroll;
      if (ui_in_rect(x, y, device_rect(&gv, 0))) return 310;
      if (ui_in_rect(x, y, device_rect(&gv, 1))) return 311;
      if (ui_in_rect(x, y, vol_minus_rect(&gv, 2))) return 312;
      if (ui_in_rect(x, y, vol_plus_rect(&gv, 2))) return 313;
      if (ui_in_rect(x, y, vol_track_rect(&gv, 2))) return 314;
      if (ui_in_rect(x, y, vol_minus_rect(&gv, 3))) return 315;
      if (ui_in_rect(x, y, vol_plus_rect(&gv, 3))) return 316;
      if (ui_in_rect(x, y, vol_track_rect(&gv, 3))) return 317;
      for (int i = 0; i < 2; i++) {
        if (ui_in_rect(x, y, chips_rect(&gv, 4, i, 2))) return 320 + i;
      }
      for (int r = 5; r <= 8; r++) {
        if (ui_in_rect(x, y, toggle_rect(&gv, r))) return 320 + r;
      }
      if (ui_in_rect(x, y, device_rect(&gv, 9))) return 329;
      if (ui_in_rect(x, y, device_rect(&gv, 10))) return 330;
      if (ui_in_rect(x, y, vol_minus_rect(&gv, 11))) return 331;
      if (ui_in_rect(x, y, vol_plus_rect(&gv, 11))) return 332;
      if (ui_in_rect(x, y, vol_track_rect(&gv, 11))) return 333;
      return 0;
    }
    case SP_STREAMING: {
      if (ui_in_rect(x, y, auto_pill_rect(g, 0))) return 400;
      if (ui_in_rect(x, y, auto_pill_rect(g, 1))) return 401;
      for (int i = 0; i < STREAM_ENC_COUNT; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 0, i, STREAM_ENC_COUNT))) return 410 + i;
      }
      for (int i = 0; i < STREAM_FW_COUNT; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 1, i, STREAM_FW_COUNT))) return 420 + i;
      }
      for (int i = 0; i < 4; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 2, i, 4))) return 430 + i;
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 3, i, 3))) return 440 + i;
      }
      for (int i = 0; i < 3; i++) {
        if (ui_in_rect(x, y, chips_rect(g, 4, i, 3))) return 450 + i;
      }
      return 0;
    }
    case SP_TEXT_IMAGES: {
      if (ui_in_rect(x, y, toggle_rect(g, 0))) return 500;
      if (ui_in_rect(x, y, toggle_rect(g, 1))) return 501;
      return 0;
    }
    case SP_KEYBINDS: {
      for (int r = 0; r < 4; r++) {
        if (ui_in_rect(x, y, keybind_rect(g, r))) return 550 + r;
      }
      return 0;
    }
    case SP_PLUGINS: {
      if (ui_in_rect(x, y, extensions_toggle_rect(g))) return 600;
      ccord_rect r_inst, r_disc, r_upd, r_sync;
      plugin_subtab_rects(g, &r_inst, &r_disc, &r_upd, &r_sync);
      if (ui_in_rect(x, y, r_inst)) return 601;
      if (ui_in_rect(x, y, r_disc)) return 602;
      if (ui_in_rect(x, y, r_upd)) return 603;
      if (ui_in_rect(x, y, r_sync)) return 604;
      if (ui_in_rect(x, y, g->content) && y >= g->content.y + 116.0f) {
        int row_cell = (int)((y - (g->content.y + 116.0f)) / 24.0f);
        int col_cell = (int)((x - g->content.x) / 64.0f);
        return 650 + (int64_t)row_cell * 32 + (int64_t)col_cell;
      }
      return 0;
    }
    default: {
      // External M6 preference / device / profile pages: quantize hover cells inside content so
      // 1px pointer jitter in the same control or whitespace does not trigger redraws.
      if (!ui_in_rect(x, y, g->content)) return 0;
      int row_cell = (int)((y - g->content.y) / 26.0f);
      int col_cell = (int)((x - g->content.x) / 44.0f);
      return 1000 + (int64_t)ui->settings_page * 4096 + (int64_t)row_cell * 64 + (int64_t)col_cell;
    }
  }
}

static bool handle_mfa_popup(ccord_ui *ui, const ccord_input_event *ev)
{
  ccord_rect pop = mfa_popup_rect(ui);
  ccord_rect mbox = mfa_popup_input_rect(&pop);
  ccord_rect vbtn = mfa_popup_verify_rect(&pop);
  ccord_rect bbtn = mfa_popup_back_rect(&pop);
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    int64_t hk = ui_in_rect(ev->x, ev->y, vbtn) ? 901
               : ui_in_rect(ev->x, ev->y, bbtn) ? 902
               : ui_in_rect(ev->x, ev->y, mbox) ? 903
                                                : 900;
    if (hk != s_hover_key) {
      s_hover_key = hk;
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3) {
    if (ui_in_rect(ev->x, ev->y, mbox)) {
      ui->account_focus_field = 4;
      ui->text_area = mbox;
      ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_mfa_edit, false, false, false, false);
      return true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (ui_in_rect(ev->x, ev->y, mbox)) {
      ui->account_focus_field = 4;
      ui->touch_text_focused = true;
      ui->text_area = mbox;
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, vbtn)) {
      ui->touch_text_focused = false;
      if (ui->d.host.account_action) {
        ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
      }
      ui->dirty = true;
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, bbtn)) {
      ui_settings_dismiss_mfa(ui);
      return true;
    }
    ui->touch_text_focused = false;
    return true;
  }
  if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
    ccord_error ierr;
    (void)ccord_text_edit_insert(&ui->account_mfa_edit, ev->text, strlen(ev->text), &ierr);
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      ui_settings_dismiss_mfa(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      ui->touch_text_focused = false;
      if (ui->d.host.account_action) {
        ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
      }
      ui->dirty = true;
      return true;
    }
    if (handle_account_field_edit_key(ui, &ui->account_mfa_edit, false, ev)) return true;
  }
  return true;
}

bool ui_settings_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->settings_open) return false;
  settings_init_defaults(ui);
  settings_fix_page(ui);
  // A capture armed on the Voice page must not leak into other pages (e.g. after Esc closed the
  // modal and it reopened on My Account): disarm as soon as the Voice page is not current.
  if (s_ptt_armed && ui->settings_page != SP_VOICE) {
    s_ptt_armed = false;
    if (ui->account_focus_field == 5) ui->account_focus_field = 0;
    ui->dirty = true;
  }
  if (s_kb_armed >= 0 && ui->settings_page != SP_KEYBINDS) {
    s_kb_armed = -1;
    if (ui->account_focus_field == 5) ui->account_focus_field = 0;
    ui->dirty = true;
  }
  if (ui->account_mfa_required && (ui_is_login_screen(ui) || ui->settings_page == SP_ACCOUNT)) {
    return handle_mfa_popup(ui, ev);
  }
  if (ui_is_login_screen(ui)) {
    ccord_rect card = login_card_rect(ui);
    if (ev->type == CCORD_IN_MOUSE_MOVE) {
      int64_t hk = 800;
      if (ui_in_rect(ev->x, ev->y, login_tab_rect(&card, 0))) hk = 801;
      else if (ui_in_rect(ev->x, ev->y, login_tab_rect(&card, 1))) hk = 802;
      else if (ui->login_tab == 0) {
        if (ui_in_rect(ev->x, ev->y, login_token_box_rect(&card))) hk = 803;
        else if (ui_in_rect(ev->x, ev->y, login_token_paste_rect(&card))) hk = 804;
        else if (ui_in_rect(ev->x, ev->y, login_token_save_rect(&card))) hk = 805;
      } else {
        if (ui_in_rect(ev->x, ev->y, login_email_box_rect(&card))) hk = 806;
        else if (ui_in_rect(ev->x, ev->y, login_password_box_rect(&card))) hk = 807;
        else if (ui_in_rect(ev->x, ev->y, login_credentials_btn_rect(&card))) hk = 808;
      }
      bool changed = ui->hover_settings_page != -1 || ui->hover_settings_close || hk != s_hover_key;
      ui->hover_settings_page = -1;
      ui->hover_settings_close = false;
      s_hover_key = hk;
      if (changed) ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3) {
      if (ui->login_tab == 0) {
        ccord_rect tbox = login_token_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, tbox)) {
          ui->account_focus_field = 1;
          ui->text_area = tbox;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_token_edit, true, false, false, false);
          return true;
        }
      } else {
        ccord_rect em_box = login_email_box_rect(&card);
        ccord_rect pw_box = login_password_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->text_area = em_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_email_edit, false, false, false, false);
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->text_area = pw_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_password_edit, true, false, false, false);
          return true;
        }
      }
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (!ui_in_rect(ev->x, ev->y, card)) {
        // Unauthenticated Login Screen is non-dismissable by outside clicks.
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, login_tab_rect(&card, 0))) {
        ui->login_tab = 0;
        ui->account_focus_field = 1;
        ui->touch_text_focused = false;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, login_tab_rect(&card, 1))) {
        ui->login_tab = 1;
        ui->account_focus_field = 2;
        ui->touch_text_focused = false;
        ui->dirty = true;
        return true;
      }
      if (ui->login_tab == 0) {
        ccord_rect tbox = login_token_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, tbox)) {
          ui->account_focus_field = 1;
          ui->touch_text_focused = true;
          ui->text_area = tbox;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, login_token_paste_rect(&card))) {
          ui->account_focus_field = 1;
          ui->touch_text_focused = false;
          if (ui->d.host.clipboard_get) {
            char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
            if (clip) {
              ccord_error ierr;
              ccord_text_edit_clear(&ui->account_token_edit);
              (void)ccord_text_edit_insert(&ui->account_token_edit, clip, strlen(clip), &ierr);
              ccord_free(clip);
            }
          }
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, login_token_save_rect(&card))) {
          ui->touch_text_focused = false;
          if (ui->d.host.account_action) {
            ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN);
          }
          ui->dirty = true;
          return true;
        }
      } else {
        ccord_rect em_box = login_email_box_rect(&card);
        ccord_rect pw_box = login_password_box_rect(&card);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->touch_text_focused = true;
          ui->text_area = em_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->touch_text_focused = true;
          ui->text_area = pw_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, login_credentials_btn_rect(&card))) {
          ui->touch_text_focused = false;
          if (ui->d.host.account_action) {
            ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
          }
          ui->dirty = true;
          return true;
        }
      }
      ui->touch_text_focused = false;
      return true;
    }
    if (ui->account_focus_field == 0) {
      ui->account_focus_field = (ui->login_tab == 0) ? 1 : 2;
    }
    ccord_text_edit *active = nullptr;
    bool masked = false;
    if (ui->account_focus_field == 1) { active = &ui->account_token_edit; masked = true; }
    else if (ui->account_focus_field == 2) { active = &ui->account_email_edit; }
    else if (ui->account_focus_field == 3) { active = &ui->account_password_edit; masked = true; }
    if (active) {
      if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
        ccord_error ierr;
        (void)ccord_text_edit_insert(active, ev->text, strlen(ev->text), &ierr);
        ui->dirty = true;
        return true;
      }
      if (ev->type == CCORD_IN_KEY_DOWN) {
        if (ev->key == CCORD_KEY_TAB) {
          if (ui->login_tab == 0) {
            ui->account_focus_field = 1;
          } else {
            ui->account_focus_field = (ui->account_focus_field == 2) ? 3 : 2;
          }
          ui->dirty = true;
          return true;
        }
        if (ev->key == CCORD_KEY_RETURN) {
          if (ui->d.host.account_action) {
            ccord_account_action act = (ui->login_tab == 0 || ui->account_focus_field == 1)
                                         ? CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN
                                         : CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN;
            ui->d.host.account_action(ui->d.host.ud, act);
          }
          ui->dirty = true;
          return true;
        }
        if (handle_account_field_edit_key(ui, active, masked, ev)) return true;
      }
    }
    return true;
  }
  sgeom g = geometry(ui);
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    int page = -1;
    for (int i = 0; i < SP_COUNT; i++) {
      if (ui_in_rect(ev->x, ev->y, g.page[i]) && ui_in_rect(ev->x, ev->y, g.list)) page = i;
    }
    bool close = ui_in_rect(ev->x, ev->y, g.close);
    int64_t hk = ui_in_rect(ev->x, ev->y, g.panel) ? settings_hover_key(ui, &g, ev->x, ev->y) : -1;
    bool changed = page != ui->hover_settings_page || close != ui->hover_settings_close || hk != s_hover_key;
    ui->hover_settings_page = page;
    ui->hover_settings_close = close;
    s_hover_key = hk;
    if (changed) ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 3) {
    if (ui->settings_page == SP_ACCOUNT && (!ui->mobile_layout || ui->mobile_settings_detail)) {
      if (!ui->account_view.signed_in) {
        ccord_rect em_box = account_email_box_rect(&g);
        ccord_rect pw_box = account_password_box_rect(&g);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->text_area = em_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_email_edit, false, false, false, false);
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->text_area = pw_box;
          ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_password_edit, true, false, false, false);
          return true;
        }
      }
      ccord_rect tbox = account_token_box_rect(&g);
      if (ui_in_rect(ev->x, ev->y, tbox)) {
        ui->account_focus_field = 1;
        ui->text_area = tbox;
        ui_menu_open_text_field(ui, ev->x, ev->y, &ui->account_token_edit, true, false, false, false);
        return true;
      }
    } else if (ui->settings_page == SP_APPEARANCE && (!ui->mobile_layout || ui->mobile_settings_detail)) {
      ccord_rect qbox = quickcss_box_rect(&g);
      if (ui_in_rect(ev->x, ev->y, qbox)) {
        ui->account_focus_field = 6;
        ui->text_area = qbox;
        ui_menu_open_text_field(ui, ev->x, ev->y, &ui->quickcss_edit, false, false, true, false);
        return true;
      }
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    s_press_x = ev->x;
    s_press_y = ev->y;
    s_press_until_ms = ui->now_ms + SETTINGS_PRESS_MS;
    ui->dirty = true;
    if (ui_in_rect(ev->x, ev->y, g.close) || !ui_in_rect(ev->x, ev->y, g.panel)) {
      settings_save_flush(ui);
      ui_open_settings(ui, false);
      return true;
    }
    if (g.back.w > 0.0f && ui_in_rect(ev->x, ev->y, g.back)) {
      ui->mobile_settings_detail = false;
      ui->account_focus_field = 0;
      ui->dirty = true;
      return true;
    }
    if (s_ptt_armed || s_kb_armed >= 0) {
      // Any click cancels an armed keybind capture; clicking the row again re-arms it.
      s_ptt_armed = false;
      s_kb_armed = -1;
      ui->account_focus_field = 0;
      ui->dirty = true;
      return true;
    }
    if (g.sb_up.w > 0.0f) {
      if (ui_in_rect(ev->x, ev->y, g.sb_up)) {
        s_sidebar_scroll -= g.step;   // geometry() clamps the offset to the list
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, g.sb_down)) {
        s_sidebar_scroll += g.step;
        ui->dirty = true;
        return true;
      }
    }
    for (int i = 0; i < SP_COUNT; i++) {
      if (ui_in_rect(ev->x, ev->y, g.page[i]) && ui_in_rect(ev->x, ev->y, g.list)) {
        if (i == SP_VOICE && ui->settings_page != SP_VOICE) s_dev.valid = false;
        ui->settings_page = i;
        ui->settings_category = k_pages[i].category;
        s_hover_key = -1;
        if (ui->mobile_layout) ui->mobile_settings_detail = true;
        ui->dirty = true;
        return true;
      }
    }
    if (ui->mobile_layout && !ui->mobile_settings_detail) {
      return true;   // Level 1 categories screen: page content is not visible
    }
    if (ui->settings_page == SP_ACCOUNT) {
      for (int i = 0; i < 5; i++) {
        if (ui_in_rect(ev->x, ev->y, account_web_btn_rect(&g, i))) {
          ui_open_url(ui, k_web_urls[i], k_web_labels[i]);
          ui->dirty = true;
          return true;
        }
      }
      if (!ui->account_view.signed_in) {
        ccord_rect em_box = account_email_box_rect(&g);
        ccord_rect pw_box = account_password_box_rect(&g);
        if (ui_in_rect(ev->x, ev->y, em_box)) {
          ui->account_focus_field = 2;
          ui->touch_text_focused = true;
          ui->text_area = em_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, pw_box)) {
          ui->account_focus_field = 3;
          ui->touch_text_focused = true;
          ui->text_area = pw_box;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, account_login_btn_rect(&g))) {
          ui->account_focus_field = 0;
          ui->touch_text_focused = false;
          if (ui->d.host.account_action) {
            ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN);
          }
          ui->dirty = true;
          return true;
        }
      }
      {
        ccord_rect tbox = account_token_box_rect(&g);
        if (ui_in_rect(ev->x, ev->y, tbox)) {
          ui->account_focus_field = 1;
          ui->touch_text_focused = true;
          ui->text_area = tbox;
          ui->dirty = true;
          return true;
        }
      }
      ui->account_focus_field = 0;
      ui->touch_text_focused = false;
      if (ui_in_rect(ev->x, ev->y, account_token_paste_rect(&g))) {
        if (ui->d.host.clipboard_get) {
          char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
          if (clip) {
            ccord_error ierr;
            ccord_text_edit_clear(&ui->account_token_edit);
            (void)ccord_text_edit_insert(&ui->account_token_edit, clip, strlen(clip), &ierr);
            ccord_free(clip);
          }
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, account_token_save_rect(&g))) {
        if (ui->d.host.account_action) {
          ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN);
        }
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, account_token_clear_rect(&g))) {
        ccord_text_edit_clear(&ui->account_token_edit);
        if (ui->d.host.account_action) {
          ui->d.host.account_action(ui->d.host.ud, CCORD_ACCOUNT_ACTION_TOKEN_SIGN_OUT);
        }
        ui->dirty = true;
        return true;
      }
      // Multi-account switcher: Add focuses the token box for a new sign-in; Switch/Remove go to
      // the app (keyring + session/store), which pushes the updated roster back.
      if (ui_in_rect(ev->x, ev->y, account_add_btn_rect(&g))) {
        ccord_text_edit_clear(&ui->account_token_edit);
        ui->account_focus_field = 1;
        ui->dirty = true;
        return true;
      }
      for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
        if (!ui->saved_accounts[i].active && ui_in_rect(ev->x, ev->y, saved_account_switch_rect(&g, (int)i))) {
          if (ui->d.host.account_switch) ui->d.host.account_switch(ui->d.host.ud, ui->saved_accounts[i].user_id);
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, saved_account_remove_rect(&g, (int)i))) {
          if (ui->d.host.account_remove) ui->d.host.account_remove(ui->d.host.ud, ui->saved_accounts[i].user_id);
          ui->dirty = true;
          return true;
        }
      }
      ccord_rect primary, secondary;
      account_buttons(ui, &g, &primary, &secondary);
      const ccord_account_view *v = &ui->account_view;
      ccord_account_action action;
      bool have = false;
      if (v->signed_in && ui_in_rect(ev->x, ev->y, primary)) {
        action = CCORD_ACCOUNT_ACTION_REFRESH;
        have = true;
      } else if (v->signed_in && ui_in_rect(ev->x, ev->y, secondary)) {
        action = CCORD_ACCOUNT_ACTION_SIGN_OUT;
        have = true;
      } else if (v->can_sign_in && ui_in_rect(ev->x, ev->y, primary)) {
        action = CCORD_ACCOUNT_ACTION_SIGN_IN;
        have = true;
      }
      if (have) {
        if (ui->d.host.account_action) ui->d.host.account_action(ui->d.host.ud, action);
        ui->dirty = true;
        return true;
      }
      return true;
    }
    bool handled = false;
    if (ui->settings_page == SP_VOICE) {
      sgeom gv = g;
      gv.content.y -= s_voice_scroll;   // rows scroll; hit-testing matches the drawn positions
      handled = handle_page_controls(ui, &gv, ev->x, ev->y);
    } else {
      handled = handle_page_controls(ui, &g, ev->x, ev->y);
    }
    s_hover_key = -1;
    if (handled &&
        (ui->settings_page == SP_APPEARANCE || ui->settings_page == SP_VOICE ||
         ui->settings_page == SP_STREAMING || ui->settings_page == SP_TEXT_IMAGES ||
         ui->settings_page == SP_NOTIFICATIONS || ui->settings_page == SP_ACCESSIBILITY ||
         ui->settings_page == SP_STREAMER || ui->settings_page == SP_LANGUAGE ||
         ui->settings_page == SP_ADVANCED || ui->settings_page == SP_CCORD)) {
      settings_save(ui);
    }
    return true;
  }
  if (ui->settings_page == SP_PROFILE_EDIT && ui_settings_profile_key(ui, ev)) return true;
  if (ui->settings_page == SP_PLUGINS && s_plugin_subtab == 1 && !s_plugin_detail_id[0] && s_plugin_search_focused) {
    if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
      size_t cur = strlen(s_plugin_search);
      size_t add = strlen(ev->text);
      if (cur + add < sizeof s_plugin_search) {
        memcpy(s_plugin_search + cur, ev->text, add + 1);
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_BACKSPACE) {
        size_t cur = strlen(s_plugin_search);
        while (cur > 0 && ((unsigned char)s_plugin_search[cur - 1] & 0xC0u) == 0x80u) cur--;
        if (cur > 0) s_plugin_search[cur - 1] = '\0';
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        trigger_plugin_discover_query(ui, false);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_ESCAPE) {
        s_plugin_search_focused = false;
        ui->dirty = true;
        return true;
      }
    }
  }
  if (ui->settings_page == SP_APPEARANCE && ui->account_focus_field == 6) {
    // QuickCSS snippet editor: every edit applies live through the host and persists.
    ccord_text_edit *active = &ui->quickcss_edit;
    if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
      ccord_error ierr;
      if (ccord_text_edit_insert(active, ev->text, strlen(ev->text), &ierr) == CCORD_OK) ui_quickcss_changed(ui);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_RETURN || ev->key == CCORD_KEY_TAB) {
        ui->account_focus_field = 0;   // commit and leave the editor
        settings_save_flush(ui);
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_BACKSPACE) {
        if (ev->mods & CCORD_MOD_CTRL) ccord_text_edit_backspace_word(active);
        else ccord_text_edit_backspace(active);
        ui_quickcss_changed(ui);
        return true;
      }
      if (ev->key == CCORD_KEY_DELETE) {
        if (ev->mods & CCORD_MOD_CTRL) ccord_text_edit_delete_word(active);
        else ccord_text_edit_delete(active);
        ui_quickcss_changed(ui);
        return true;
      }
      if (ev->key == CCORD_KEY_LEFT) { ccord_text_edit_move(active, -1, (ev->mods & CCORD_MOD_SHIFT) != 0); ui->dirty = true; return true; }
      if (ev->key == CCORD_KEY_RIGHT) { ccord_text_edit_move(active, 1, (ev->mods & CCORD_MOD_SHIFT) != 0); ui->dirty = true; return true; }
      if (ev->key == CCORD_KEY_HOME) { ccord_text_edit_move(active, -4, false); ui->dirty = true; return true; }
      if (ev->key == CCORD_KEY_END) { ccord_text_edit_move(active, 4, false); ui->dirty = true; return true; }
      if ((ev->mods & CCORD_MOD_CTRL) && ev->key == CCORD_KEY_A) {
        ccord_text_edit_select_all(active);
        ui->dirty = true;
        return true;
      }
      if ((ev->mods & CCORD_MOD_CTRL) && ev->key == CCORD_KEY_V && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          ccord_error ierr;
          if (ccord_text_edit_insert(active, clip, strlen(clip), &ierr) == CCORD_OK) ui_quickcss_changed(ui);
          ccord_free(clip);
        }
        ui->dirty = true;
        return true;
      }
    }
    return true;   // modal editor: swallow everything else
  }
  if (ui->settings_page == SP_ACCOUNT && ui->account_focus_field != 0) {
    ccord_text_edit *active = nullptr;
    bool masked = false;
    if (ui->account_focus_field == 1) { active = &ui->account_token_edit; masked = true; }
    else if (ui->account_focus_field == 2) { active = &ui->account_email_edit; }
    else if (ui->account_focus_field == 3) { active = &ui->account_password_edit; masked = true; }
    if (active) {
      if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
        ccord_error ierr;
        (void)ccord_text_edit_insert(active, ev->text, strlen(ev->text), &ierr);
        ui->dirty = true;
        return true;
      }
      if (ev->type == CCORD_IN_KEY_DOWN) {
        if (ev->key == CCORD_KEY_TAB && !ui->account_view.signed_in) {
          if (ui->account_focus_field == 2) ui->account_focus_field = 3;
          else if (ui->account_focus_field == 3) ui->account_focus_field = 1;
          else ui->account_focus_field = 2;
          ui->dirty = true;
          return true;
        }
        if (ev->key == CCORD_KEY_RETURN) {
          if (ui->d.host.account_action) {
            ccord_account_action act = (ui->account_focus_field == 1)
                                         ? CCORD_ACCOUNT_ACTION_TOKEN_SIGN_IN
                                         : CCORD_ACCOUNT_ACTION_CREDENTIALS_LOGIN;
            ui->d.host.account_action(ui->d.host.ud, act);
          }
          ui->dirty = true;
          return true;
        }
        if (handle_account_field_edit_key(ui, active, masked, ev)) return true;
      }
    }
  }
  if (s_ptt_armed && ui->settings_page == SP_VOICE) {
    // Live keybind capture: the next key press binds Push to Talk. Escape never arrives here
    // (ui.c closes the modal with it); the stale-armed cleanup above handles that path.
    if (ev->type == CCORD_IN_KEY_DOWN) {
      SDL_Scancode sc = key_to_scancode(ev->key);
      if (sc != SDL_SCANCODE_UNKNOWN) {
        ui->settings_ptt_scancode = (int)sc;
        s_ptt_armed = false;
        ui->account_focus_field = 0;
        settings_save(ui);
      }
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_TEXT) {
      ui->dirty = true;
      return true;   // swallow text while waiting for a key
    }
  }
  if (s_kb_armed >= 0 && ui->settings_page == SP_KEYBINDS) {
    // Live capture for Mute / Deafen / Quick Switcher: the next key press binds scancode + mods.
    if (ev->type == CCORD_IN_KEY_DOWN) {
      SDL_Scancode sc = key_to_scancode(ev->key);
      if (sc != SDL_SCANCODE_UNKNOWN) {
        ui->settings_keybinds[s_kb_armed].scancode = (int)sc;
        ui->settings_keybinds[s_kb_armed].mods =
            ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT | CCORD_MOD_SUPER);
        s_kb_armed = -1;
        ui->account_focus_field = 0;
        settings_save(ui);
        if (ui->d.host.keybinds_changed) ui->d.host.keybinds_changed(ui->d.host.ud);
      }
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_TEXT) {
      ui->dirty = true;
      return true;   // swallow text while waiting for a key
    }
  }
  if (ev->type == CCORD_IN_KEY_DOWN && !(ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SHIFT | CCORD_MOD_ALT | CCORD_MOD_SUPER)) &&
      ui->account_focus_field == 0 && (ev->key == CCORD_KEY_UP || ev->key == CCORD_KEY_DOWN)) {
    return sidebar_key_step(ui, ev->key == CCORD_KEY_UP ? -1 : 1);
  }
  return true;   // modal: swallow everything else
}

// ---- drawing helpers ----

static void label_value(ccord_ui *ui, ccord_renderer *r, float x, float *y, float w, const char *label, const char *value)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_style vs = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_body, COL(text_normal));
  ccord_text_draw(r, label, strlen(label), x, *y, &ls, 0.0f);
  *y += ui_line_h(ui, ls.size) + 2.0f;
  ccord_text_draw_ellipsis(r, value, strlen(value), x, *y, w, &vs);
  *y += ui_line_h(ui, vs.size) + 14.0f;
}

static void draw_row_label(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_draw(r, label, strlen(label), g->content.x, row_y(g, row), &ls, 0.0f);
}

static void draw_chip_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, int count,
                          const char *const *labels, int selected)
{
  for (int i = 0; i < count; i++) {
    ccord_rect o = chips_rect(g, row, i, count);
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, o), .selected = i == selected};
    uint64_t cid = (uint64_t)ui->settings_page * 4096u + (uint64_t)row * 64u + (uint64_t)i;
    uis_chip(ui, r, o, labels[i], st, "chip", cid);
  }
}

static void draw_toggle_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label,
                            const char *note, bool on, bool dim)
{
  draw_row_label(ui, r, g, row, label);
  if (note && note[0]) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
    ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, row_y(g, row) + 18.0f,
                             g->content.w - 60.0f, &ns);
  }
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, toggle_rect(g, row)), .disabled = dim};
  uis_toggle(ui, r, row_rect(g, row), on, st, "swset", (uint64_t)ui->settings_page * 64u + (uint64_t)row);
}

static void draw_device_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label,
                            const char *device)
{
  draw_row_label(ui, r, g, row, label);
  ccord_rect d = device_rect(g, row);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, d)};
  uis_select(ui, r, d, device, st, "sdev", (uint64_t)ui->settings_page * 64u + (uint64_t)row);
}

static void draw_volume_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label, int value,
                            const char *suffix, bool dim)
{
  draw_row_label(ui, r, g, row, label);
  ccord_rect minus = vol_minus_rect(g, row), plus = vol_plus_rect(g, row), track = vol_track_rect(g, row);
  uint64_t vid = (uint64_t)ui->settings_page * 64u + (uint64_t)row;
  uis_state bst = {.hover = ui_in_rect(ui->mx, ui->my, minus), .disabled = dim};
  uis_button(ui, r, minus, "-", UIS_BTN_SECONDARY, UIS_BTN_S_SM, bst, "volm", vid);
  bst.hover = ui_in_rect(ui->mx, ui->my, plus);
  uis_button(ui, r, plus, "+", UIS_BTN_SECONDARY, UIS_BTN_S_SM, bst, "volp", vid);
  uis_state sst = {.disabled = dim};
  uis_slider(ui, r, track, (float)value / 100.0f, sst);
  char vs[24];
  snprintf(vs, sizeof vs, "%d%s", value, suffix);
  ccord_text_style vsl = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  ccord_text_draw(r, vs, strlen(vs), plus.x + plus.w + 12.0f, row_y(g, row) + 26.0f, &vsl, 0.0f);
}

// ---- pages ----

static const char *state_name(ccord_plugin_state s)
{
  switch (s) {
    case CCORD_PLUGIN_STARTING: return "Starting";
    case CCORD_PLUGIN_READY: return "Ready";
    case CCORD_PLUGIN_FAILED: return "Failed";
    case CCORD_PLUGIN_STOPPED: return "Stopped";
    case CCORD_PLUGIN_NEEDS_CONSENT: return "Needs Consent";
  }
  return "?";
}

static void draw_appearance(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_row_label(ui, r, g, 0, "MESSAGE DISPLAY");
  draw_chip_row(ui, r, g, 0, 2, k_display_mode, ui->settings_display_mode);

  // M6 theme chips + UI zoom slider + compact theme summary/swatches (rows 1-3).
  ui_settings_m6_appearance_draw(ui, r, g->content);

  draw_toggle_row(ui, r, g, 5, "CUSTOM THEME OVERRIDES", "Apply custom style overrides from settings.ini.",
                  ui->settings_quickcss_enabled, false);
  draw_toggle_row(ui, r, g, 7, "REDUCED MOTION", "Turn off fades, slides and other animations.",
                  ui->settings_reduced_motion, false);
  draw_row_label(ui, r, g, 6, "STYLE OVERRIDE SNIPPET");
  ccord_rect qbox = quickcss_box_rect(g);
  uis_state qst = {.focused = ui->account_focus_field == 6};
  uis_input(ui, r, qbox, &ui->quickcss_edit, "", qst, false, "squick", 6);
}

static const char *voice_conn_state_name(int state)
{
  switch (state) {
    case CCORD_VOICE_IDLE: return "Idle";
    case CCORD_VOICE_CONNECTING: return "Connecting";
    case CCORD_VOICE_HELLO: return "Hello";
    case CCORD_VOICE_READY: return "Ready";
    case CCORD_VOICE_ESTABLISHED: return "Established";
    case CCORD_VOICE_RECONNECTING: return "Reconnecting";
    case CCORD_VOICE_FAILED: return "Failed";
    default: return "Unknown";
  }
}

// Live RTC diagnostics + DAVE E2EE summary, read from the session (which mirrors the voice
// connection). Nothing is fabricated: outside voice the panel says so, and the "Verified" badge
// only appears when the DAVE backend actually reports a verified MLS handshake.
static void draw_voice_diagnostics(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  ccord_rect card = {g->content.x, row_y(g, VOICE_ROWS), g->content.w, VOICE_DIAG_H};
  uis_card(ui, r, card);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_muted));
  ccord_text_draw(r, "CONNECTION DIAGNOSTICS", 21, card.x + 12.0f, card.y + 10.0f, &hs, 0.0f);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  bool in_voice = ui->d.store && ccord_store_active_voice_channel(ui->d.store) != CCORD_ID_NONE;
  if (!in_voice) {
    const char *note = "Join a voice channel to see live connection diagnostics.";
    ccord_text_draw_ellipsis(r, note, strlen(note), card.x + 12.0f, card.y + 34.0f, card.w - 24.0f, &ns);
    return;
  }
  ccord_rtc_stats st;
  ccord_dave_status dv;
  ccord_session_voice_stats(ui->d.session, &st);
  ccord_session_dave_status(ui->d.session, &dv);
  char line[192];
  snprintf(line, sizeof line, "State: %s    Ping: %d ms    Bitrate: %u kbps", voice_conn_state_name(st.state),
           st.ping_ms, (unsigned)st.bitrate_kbps);
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 32.0f, card.w - 24.0f, &ns);
  snprintf(line, sizeof line, "Jitter: %zu    Lost packets: %llu    SSRC: %u", st.jitter_depth,
           (unsigned long long)st.packets_lost, (unsigned)st.ssrc);
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 52.0f, card.w - 24.0f, &ns);
  const char *enc = dv.active ? "End-to-end encrypted (DAVE)" : "Encryption: not active";
  ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, dv.active ? COL(status_online) : COL(text_muted));
  ccord_text_draw_ellipsis(r, enc, strlen(enc), card.x + 12.0f, card.y + 74.0f, card.w - 24.0f, &es);
  snprintf(line, sizeof line, "Protocol v%u    Epoch: %u%s", (unsigned)dv.protocol_version, (unsigned)st.dave_epoch,
           dv.verified ? "    Verified" : "");
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 94.0f, card.w - 24.0f, &ns);
}

// Fixed scrollbar in the right margin (the page scrolls; the scrollbar does not).
static void draw_voice_scrollbar(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  float max = voice_max_scroll(g);
  if (max <= 0.0f) return;
  ccord_rect tr = voice_scroll_track(g), up = voice_scroll_up_rect(g), down = voice_scroll_down_rect(g);
  float th = voice_scroll_thumb_h(g);
  float ty = tr.y + (tr.h - th) * (s_voice_scroll / max);
  uis_scrollbar(ui, r, tr, (ccord_rect){tr.x, ty, tr.w, th});
  uis_state us = {.hover = ui_in_rect(ui->mx, ui->my, up)}, ds = {.hover = ui_in_rect(ui->mx, ui->my, down)};
  uis_scroll_arrow(ui, r, up, true, us, "vscroll", 0);
  uis_scroll_arrow(ui, r, down, false, ds, "vscroll", 1);
}

static void draw_voice(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  if (s_last_drawn_page != SP_VOICE) s_dev.valid = false;
  voice_devices_refresh(ui, false);
  draw_voice_scrollbar(ui, r, g);   // fixed, drawn against the unshifted geometry
  sgeom gv = *g;
  gv.content.y -= s_voice_scroll;
  ccord_push_clip(r, g->content);   // scrolled rows must not paint over the title

  // Rows 0-8 keep the original layout; M5 additions (camera, PTT key, sensitivity, diagnostics)
  // follow at rows 9-12, reached by scrolling.
  draw_device_row(ui, r, &gv, 0, "INPUT DEVICE", voice_input_name(ui));
  draw_device_row(ui, r, &gv, 1, "OUTPUT DEVICE", voice_output_name(ui));
  draw_volume_row(ui, r, &gv, 2, "INPUT VOLUME", ui->settings_input_volume, "%", false);
  draw_volume_row(ui, r, &gv, 3, "OUTPUT VOLUME", ui->settings_output_volume, "%", false);
  draw_row_label(ui, r, &gv, 4, "INPUT MODE");
  draw_chip_row(ui, r, &gv, 4, 2, k_voice_mode, ui->settings_voice_mode);
  bool dsp = ccord_dsp_available();
  const char *dsp_note = dsp ? nullptr : "Unavailable: this build has no speexdsp backend.";
  draw_toggle_row(ui, r, &gv, 5, "NOISE SUPPRESSION",
                  dsp ? "Reduces background noise picked up by your microphone." : dsp_note,
                  ui->settings_noise_suppression && dsp, !dsp);
  draw_toggle_row(ui, r, &gv, 6, "ECHO CANCELLATION",
                  dsp ? "Removes speaker echo picked up by the microphone." : dsp_note,
                  ui->settings_echo_cancel && dsp, !dsp);
  draw_toggle_row(ui, r, &gv, 7, "AUTOMATIC GAIN CONTROL",
                  dsp ? "Keeps your voice at a consistent level." : dsp_note,
                  ui->settings_auto_gain && dsp, !dsp);
  draw_toggle_row(ui, r, &gv, 8, "HARDWARE ACCELERATION", "Use the GPU for video encoding and rendering.",
                  ui->settings_hardware_accel, false);

  draw_device_row(ui, r, &gv, 9, "CAMERA", voice_camera_name(ui));
  // Push to Talk key: live capture. While armed the button prompts for a key press.
  char binding[64];
  if (s_ptt_armed) snprintf(binding, sizeof binding, "Press a key...");
  else ptt_binding_name(ui, binding, sizeof binding);
  draw_device_row(ui, r, &gv, 10, "PUSH TO TALK KEY", binding);
  bool vad = ui->settings_voice_mode == 0;
  draw_volume_row(ui, r, &gv, 11, "INPUT SENSITIVITY", ui->settings_vad_sensitivity, "", !vad);
  if (!vad) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
    const char *note = "Push to Talk is active; the mic opens while the bound key is held.";
    ccord_text_draw_ellipsis(r, note, strlen(note), gv.content.x, row_y(&gv, 11) + 40.0f, gv.content.w - 60.0f, &ns);
  }

  draw_voice_diagnostics(ui, r, &gv);
  ccord_pop_clip(r);
}

// The "Automatic" pill at the end of a streaming chip row: a chip, selected while the choice is Automatic.
static void draw_auto_pill(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, bool sel)
{
  ccord_rect ar = auto_pill_rect(g, row);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, ar), .selected = sel};
  uis_chip(ui, r, ar, "Automatic", st, "sauto", (uint64_t)row);
}

static void draw_streaming(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_row_label(ui, r, g, 0, "GPU / DISCORD ENCODER");
  draw_auto_pill(ui, r, g, 0, ui->settings_encoder == (int)CCORD_STREAM_ENCODER_AUTO);
  draw_chip_row(ui, r, g, 0, STREAM_ENC_COUNT, k_encoders, find_val_index(k_encoder_vals, STREAM_ENC_COUNT, ui->settings_encoder));
  draw_row_label(ui, r, g, 1, "CAPTURE FRAMEWORK");
  draw_auto_pill(ui, r, g, 1, ui->settings_framework == (int)CCORD_STREAM_FRAMEWORK_AUTO);
  draw_chip_row(ui, r, g, 1, STREAM_FW_COUNT, k_frameworks, find_val_index(k_framework_vals, STREAM_FW_COUNT, ui->settings_framework));
  draw_row_label(ui, r, g, 2, "RESOLUTION");
  draw_chip_row(ui, r, g, 2, 4, k_resolutions, ui->settings_resolution);
  draw_row_label(ui, r, g, 3, "FRAMERATE");
  draw_chip_row(ui, r, g, 3, 3, k_fps, ui->settings_fps);
  draw_row_label(ui, r, g, 4, "CODEC");
  draw_chip_row(ui, r, g, 4, 3, k_codecs, ui->settings_codec);
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  char note[192];
  snprintf(note, sizeof note, "%s",
           st && st->active ? "A stream is live; changes apply to the next Go Live session."
                            : "Encoder and capture backend default to Automatic and are configured here.");
  ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, row_y(g, 5) + 4.0f, g->content.w, &ns);
}

static void draw_text_images(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_toggle_row(ui, r, g, 0, "INLINE EMBEDS", "Show link previews and embed cards in chat.", ui->settings_embeds, false);
  draw_toggle_row(ui, r, g, 1, "ATTACHMENT PREVIEWS", "Render image and file attachment cards.", ui->settings_attachments, false);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  const char *note = "Turning a card off hides it locally; the message and its data stay in the channel.";
  ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, row_y(g, 2) + 8.0f, g->content.w, &ns);
}

// True while a left press that started on `rc` is recent and the pointer is still on it.
static bool settings_pressed(const ccord_ui *ui, ccord_rect rc)
{
  return ui->now_ms < s_press_until_ms && ui_in_rect(s_press_x, s_press_y, rc) && ui_in_rect(ui->mx, ui->my, rc);
}

static void draw_keybind_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label, const char *key)
{
  draw_row_label(ui, r, g, row, label);
  ccord_rect b = keybind_rect(g, row);
  bool armed = row == 0 ? s_ptt_armed : (row >= 1 && row <= 3 && s_kb_armed == row - 1);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, b), .pressed = settings_pressed(ui, b), .focused = armed};
  uis_button(ui, r, b, key, UIS_BTN_SECONDARY, UIS_BTN_S_SM, st, "skey", (uint64_t)row);
}

static void draw_keybinds(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  char ptt[64], mute[64], deafen[64], qs[64];
  ptt_binding_name(ui, ptt, sizeof ptt);
  keybind_name(ui->settings_keybinds[0].scancode, ui->settings_keybinds[0].mods, mute, sizeof mute);
  keybind_name(ui->settings_keybinds[1].scancode, ui->settings_keybinds[1].mods, deafen, sizeof deafen);
  keybind_name(ui->settings_keybinds[2].scancode, ui->settings_keybinds[2].mods, qs, sizeof qs);
  if (s_ptt_armed) snprintf(ptt, sizeof ptt, "Press a key...");
  if (s_kb_armed == 0) snprintf(mute, sizeof mute, "Press a key...");
  if (s_kb_armed == 1) snprintf(deafen, sizeof deafen, "Press a key...");
  if (s_kb_armed == 2) snprintf(qs, sizeof qs, "Press a key...");
  draw_keybind_row(ui, r, g, 0, "PUSH TO TALK", ptt);
  draw_keybind_row(ui, r, g, 1, "TOGGLE MUTE", mute);
  draw_keybind_row(ui, r, g, 2, "TOGGLE DEAFEN", deafen);
  draw_keybind_row(ui, r, g, 3, "OPEN QUICK SWITCHER", qs);
  draw_keybind_row(ui, r, g, 4, "START / STOP STREAM", "Ctrl + Shift + L");
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  const char *note = "Click a key row, then press the new chord (modifiers included). Push to Talk is "
                     "captured on Voice & Video; changes persist in settings.ini.";
  ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, row_y(g, 5) + 8.0f, g->content.w, &ns);
}

static void draw_extensions_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_row_label(ui, r, g, 0, "EXTENSIONS");
  float x = g->content.x, w = g->content.w;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  const char *note = ui->extensions_on ? "Turning off stops every extension and removes its menu items."
                                       : "Turning on starts the extensions again.";
  ccord_text_draw_ellipsis(r, note, strlen(note), x, row_y(g, 0) + 20.0f, w - 60.0f, &ns);
  ccord_rect t = extensions_toggle_rect(g);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, t)};
  uis_toggle(ui, r, row_rect(g, 0), ui->extensions_on, st, "swext", 0);
}

static void draw_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool primary);

static const ccord_plugin_info *find_plugin_info(const ccord_contrib *c, const char *id)
{
  if (!c || !id || !id[0]) return nullptr;
  size_t n = ccord_contrib_plugin_count(c);
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(c, i);
    if (p && strcmp(p->id, id) == 0) return p;
  }
  return nullptr;
}

static void draw_plugins(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  ccord_rect r_inst, r_disc, r_upd, r_sync;
  plugin_subtab_rects(g, &r_inst, &r_disc, &r_upd, &r_sync);
  size_t inst_n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
  const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
  size_t pend_upd = cst ? cst->pending_updates : 0;

  char l_inst[32], l_upd[32];
  snprintf(l_inst, sizeof l_inst, "Installed (%zu)", inst_n);
  if (pend_upd > 0) snprintf(l_upd, sizeof l_upd, "Updates (%zu)", pend_upd);
  else snprintf(l_upd, sizeof l_upd, "Updates");

  draw_button(ui, r, r_inst, l_inst, s_plugin_subtab == 0 && !s_plugin_detail_id[0]);
  draw_button(ui, r, r_disc, "Discover", s_plugin_subtab == 1 && !s_plugin_detail_id[0]);
  draw_button(ui, r, r_upd, l_upd, s_plugin_subtab == 2 && !s_plugin_detail_id[0]);
  draw_button(ui, r, r_sync, (cst && cst->syncing) ? "Syncing..." : "Sync Catalog", false);

  float x = g->content.x, y = g->content.y + 120.0f, w = g->content.w;
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_body, COL(text_header));
  ccord_text_style vs = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));

  // Debug Log modal view
  if (s_plugin_log_id[0]) {
    const ccord_plugin_info *p = find_plugin_info(ui->d.contrib, s_plugin_log_id);
    if (!p) { s_plugin_log_id[0] = '\0'; }
    else {
      char hdr[128];
      snprintf(hdr, sizeof hdr, "Debug Log: %s (%s)", p->name, p->id);
      ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x, y, w - 260.0f, &ns);
      draw_button(ui, r, (ccord_rect){x + w - 246.0f, y - 4.0f, 74.0f, UIS_H_BTN_SM}, "Reload", false);
      draw_button(ui, r, (ccord_rect){x + w - 164.0f, y - 4.0f, 74.0f, UIS_H_BTN_SM}, "Clear", false);
      draw_button(ui, r, (ccord_rect){x + w - 82.0f, y - 4.0f, 82.0f, UIS_H_BTN_SM}, "Back", true);
      y += 32.0f;
      ccord_rect box = {x, y, w, g->panel.y + g->panel.h - y - 16.0f};
      uis_card(ui, r, box);
      size_t lc = ccord_contrib_plugin_log_count(ui->d.contrib, p->id);
      if (lc == 0) {
        const char *empty = "No debug log entries recorded for this plugin yet.";
        ccord_text_draw(r, empty, strlen(empty), box.x + 14.0f, box.y + 14.0f, &ms, box.w - 28.0f);
      } else {
        float ly = box.y + 12.0f;
        size_t max_lines = (size_t)((box.h - 24.0f) / 18.0f);
        size_t start = lc > max_lines ? lc - max_lines : 0;
        for (size_t i = start; i < lc; i++) {
          const ccord_plugin_log_entry *e = ccord_contrib_plugin_log_at(ui->d.contrib, p->id, i);
          if (!e) continue;
          const char *tag = e->level == 3 ? "[ERR] " : e->level == 2 ? "[WARN] " : e->level == 0 ? "[DBG] " : "[INFO] ";
          ccord_color col = e->level == 3 ? COL(danger) : e->level == 2 ? COL(status_idle) : e->level == 0 ? COL(text_muted) : COL(text_normal);
          ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, col);
          char line[512];
          snprintf(line, sizeof line, "%s%s", tag, e->message ? e->message : "");
          ccord_text_draw_ellipsis(r, line, strlen(line), box.x + 12.0f, ly, box.w - 24.0f, &ls);
          ly += 18.0f;
        }
      }
      return;
    }
  }

  // API Class Permission Review / Consent view
  if (s_plugin_consent_id[0]) {
    const ccord_plugin_info *p = find_plugin_info(ui->d.contrib, s_plugin_consent_id);
    if (!p) { s_plugin_consent_id[0] = '\0'; }
    else {
      ccord_rect box = {x, y, w, 224.0f};
      uis_card(ui, r, box);
      uint8_t api_cls = (uint8_t)(p->risk_class + 1);
      ccord_text_style ws = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_body, plugin_api_class_color(ui, api_cls));
      const char *title = api_cls >= 3 ? "API CLASS 3: Host & Network Permissions Requested"
                                       : "API CLASS 2: C-Cord Internal Permissions Requested";
      ccord_text_draw_ellipsis(r, title, strlen(title), box.x + 16.0f, box.y + 14.0f, box.w - 32.0f, &ws);
      char sub[256];
      snprintf(sub, sizeof sub, "Plugin: %s v%s (%s)", p->name, p->version, p->id);
      ccord_text_draw_ellipsis(r, sub, strlen(sub), box.x + 16.0f, box.y + 42.0f, box.w - 32.0f, &ns);
      char pmsg[512];
      snprintf(pmsg, sizeof pmsg, "Declared permissions: %s", p->permissions && p->permissions[0] ? p->permissions : "none");
      ccord_text_draw(r, pmsg, strlen(pmsg), box.x + 16.0f, box.y + 72.0f, &ms, box.w - 32.0f);
      const char *warn = "Review the API permissions this plugin requests before enabling. Undeclared Class 2/3 APIs terminate the host.";
      ccord_text_draw(r, warn, strlen(warn), box.x + 16.0f, box.y + 118.0f, &vs, box.w - 32.0f);
      draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 46.0f, 184.0f, UIS_H_BTN}, "Approve & Enable", true);
      draw_button(ui, r, (ccord_rect){box.x + 212.0f, box.y + box.h - 46.0f, 96.0f, UIS_H_BTN}, "Cancel", false);
      return;
    }
  }

  // Inline Catalog Plugin Detail view (also used by ccord://plugins/install/<id> deep links)
  if (s_plugin_detail_id[0]) {
    const ccord_catalog_entry *e = ccord_contrib_catalog_detail(ui->d.contrib);
    if (!e || strcmp(e->id, s_plugin_detail_id) != 0) {
      const ccord_catalog_entry *fb = ccord_contrib_catalog_find(ui->d.contrib, s_plugin_detail_id);
      e = (fb && fb->has_detail) ? fb : nullptr;
    }
    if (!e) {
      draw_button(ui, r, (ccord_rect){x, y, 86.0f, UIS_H_BTN_SM}, "Back", false);
      ccord_rect box = {x, y + 34.0f, w, 128.0f};
      uis_card(ui, r, box);
      char hdr[128];
      bool busy = cst && cst->detail_loading;
      if (busy) {
        snprintf(hdr, sizeof hdr, "Loading %s from catalog...", s_plugin_detail_id);
      } else {
        snprintf(hdr, sizeof hdr, "Plugin not found in catalog: %s", s_plugin_detail_id);
      }
      ccord_text_draw_ellipsis(r, hdr, strlen(hdr), box.x + 16.0f, box.y + 18.0f, box.w - 32.0f, &ns);
      const char *sub = busy
                            ? "Fetching verified metadata and SHA-256 digest before prompting for install."
                            : (cst && cst->detail_error[0])
                                  ? cst->detail_error
                                  : "Verify the plugin ID on plugins.c-cord.org or click Retry.";
      ccord_text_draw_ellipsis(r, sub, strlen(sub), box.x + 16.0f, box.y + 46.0f, box.w - 32.0f, &ms);
      draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 120.0f, UIS_H_BTN}, "Retry", true);
      return;
    } else {
      draw_button(ui, r, (ccord_rect){x, y, 86.0f, UIS_H_BTN_SM}, "Back", false);
      ccord_rect box = {x, y + 34.0f, w, 286.0f};
      uis_card(ui, r, box);
      float nw = ccord_text_draw_ellipsis(r, e->name, strlen(e->name), box.x + 16.0f, box.y + 14.0f, w - 300.0f, &ns);
      float bx = box.x + 16.0f + nw + 8.0f;
      bx += ccord_text_draw_ellipsis(r, e->version, strlen(e->version), bx, box.y + 16.0f, 68.0f, &vs) + 8.0f;
      const char *tl = plugin_tier_label(e->tier);
      ccord_text_style tsty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_tier_color(ui, e->tier));
      if (tl[0]) {
        ccord_text_draw(r, tl, strlen(tl), bx, box.y + 16.0f, &tsty, 0.0f);
        bx += ccord_text_measure(r, tl, strlen(tl), &tsty, 0.0f).x + 8.0f;
      }
      const char *al = plugin_api_class_label(e->api_class);
      ccord_text_style asty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_api_class_color(ui, e->api_class));
      ccord_text_draw(r, al, strlen(al), bx, box.y + 16.0f, &asty, 0.0f);

      char meta[320];
      snprintf(meta, sizeof meta, "ID: %s  |  Author: @%s  |  Repo: %s  |  License: %s",
               e->id, e->author, e->repo, e->license && e->license[0] ? e->license : "Unspecified");
      ccord_text_draw_ellipsis(r, meta, strlen(meta), box.x + 16.0f, box.y + 42.0f, box.w - 32.0f, &ms);
      ccord_text_draw(r, e->description && e->description[0] ? e->description : "No description provided.",
                      strlen(e->description && e->description[0] ? e->description : "No description provided."),
                      box.x + 16.0f, box.y + 66.0f, &ms, box.w - 32.0f);
      char perms[512];
      snprintf(perms, sizeof perms, "Declared permissions: %s", e->permissions && e->permissions[0] ? e->permissions : "none (API Class 1 only)");
      ccord_text_draw_ellipsis(r, perms, strlen(perms), box.x + 16.0f, box.y + 114.0f, box.w - 32.0f, &vs);
      char sha[160];
      snprintf(sha, sizeof sha, "SHA-256: %s", e->sha256);
      ccord_text_draw_ellipsis(r, sha, strlen(sha), box.x + 16.0f, box.y + 136.0f, box.w - 32.0f, &vs);
      if (e->warning_sev != CCORD_WARN_NONE && e->warning_title && e->warning_title[0]) {
        ccord_color wc = e->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
        ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, wc);
        char wline[320];
        snprintf(wline, sizeof wline, "Moderator Warning: %s - %s", e->warning_title, e->warning_message ? e->warning_message : "");
        ccord_text_draw_ellipsis(r, wline, strlen(wline), box.x + 16.0f, box.y + 162.0f, box.w - 32.0f, &wst);
      } else if (e->changelog && e->changelog[0]) {
        char cl[320];
        snprintf(cl, sizeof cl, "Release notes: %s", e->changelog);
        ccord_text_draw_ellipsis(r, cl, strlen(cl), box.x + 16.0f, box.y + 162.0f, box.w - 32.0f, &ms);
      }
      if (e->update_needs_review) {
        ccord_text_style rws = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(status_idle));
        const char *rm = "Auto-update paused: this update adds new Class 2/3 API permissions or carries a moderator warning.";
        ccord_text_draw_ellipsis(r, rm, strlen(rm), box.x + 16.0f, box.y + 190.0f, box.w - 32.0f, &rws);
      }
      if (!e->bundled) {
        if (e->installing) {
          draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN}, "Verifying SHA-256...", true);
        } else if (!e->installed) {
          draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN}, "Install .cce", true);
        } else if (e->update_available) {
          draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN},
                      e->update_needs_review ? "Approve & Update" : "Update Now", true);
        }
        if (e->installed) {
          float ux = box.x + ((!e->installed || e->update_available) ? 180.0f : 16.0f);
          draw_button(ui, r, (ccord_rect){ux, box.y + box.h - 44.0f, 96.0f, UIS_H_BTN}, "Uninstall", false);
        }
      }
      return;
    }
  }

  // Subtab 1: Discover Directory (server-side FTS5 search + cursor pagination)
  if (s_plugin_subtab == 1) {
    // Row 1: Search bar + Search button + Sort toggle
    ccord_rect r_q = {x, y, w - 196.0f, UIS_H_BTN_SM};
    ccord_rect r_go = {x + w - 188.0f, y, 74.0f, UIS_H_BTN_SM};
    ccord_rect r_sort = {x + w - 106.0f, y, 106.0f, UIS_H_BTN_SM};
    uis_plate(ui, r, r_q, uis_tokens_of(ui).r_control, COL(bg_input));
    if (s_plugin_search_focused) {
      ccord_draw_rect(r, (ccord_rect){r_q.x, r_q.y + r_q.h - 2.0f, r_q.w, 2.0f}, COL(brand));
    }
    const char *qdisp = s_plugin_search[0] ? s_plugin_search : "Search plugins (press Enter)...";
    ccord_text_style qst = s_plugin_search[0] ? ns : ms;
    ccord_text_draw_ellipsis(r, qdisp, strlen(qdisp), r_q.x + 10.0f, r_q.y + 6.0f, r_q.w - 20.0f, &qst);
    draw_button(ui, r, r_go, "Search", true);
    char slbl[32];
    snprintf(slbl, sizeof slbl, "Sort: %s", plugin_sort_label(s_plugin_sort_idx));
    draw_button(ui, r, r_sort, slbl, false);
    y += 34.0f;

    // Row 2: Tier chips + API Class 1 filter
    static const char *const k_flbl[4] = {"All", "Official", "Verified", "Unverified"};
    static const float k_fw[4] = {54.0f, 76.0f, 76.0f, 88.0f};
    float fx = x;
    for (int t = 0; t < 4; t++) {
      draw_button(ui, r, (ccord_rect){fx, y, k_fw[t], UIS_H_BTN_SM}, k_flbl[t], s_plugin_tier_filter == t);
      fx += k_fw[t] + 6.0f;
    }
    draw_button(ui, r, (ccord_rect){fx + 4.0f, y, 126.0f, UIS_H_BTN_SM},
                s_plugin_class1_only ? "Class 1 Only: ON" : "API Class 1 Only", s_plugin_class1_only);
    y += 36.0f;

    size_t cn = ui->d.contrib ? ccord_contrib_catalog_count(ui->d.contrib) : 0;
    if (cn == 0) {
      const char *msg = (cst && cst->status_msg[0]) ? cst->status_msg
                      : "No plugins loaded yet. Click Sync Catalog or Search.";
      ccord_text_draw(r, msg, strlen(msg), x, y, &ms, w);
      return;
    }
    float bottom_lim = g->panel.y + g->panel.h - (cst && cst->has_more ? 52.0f : 16.0f);
    for (size_t i = 0; i < cn; i++) {
      const ccord_catalog_entry *e = ccord_contrib_catalog_at(ui->d.contrib, i);
      if (!e) continue;
      bool has_w = e->warning_sev != CCORD_WARN_NONE && e->warning_title && e->warning_title[0];
      float ch = 98.0f + (has_w ? 20.0f : 0.0f);
      if (y + ch > bottom_lim) break;
      ccord_rect card = {x, y, w, ch};
      uis_card(ui, r, card);
      float nw = ccord_text_draw_ellipsis(r, e->name, strlen(e->name), card.x + 14.0f, card.y + 10.0f, w - 340.0f, &ns);
      float bx = card.x + 14.0f + nw + 8.0f;
      bx += ccord_text_draw_ellipsis(r, e->version, strlen(e->version), bx, card.y + 12.0f, 64.0f, &vs) + 8.0f;
      const char *tl = plugin_tier_label(e->tier);
      ccord_text_style tsty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_tier_color(ui, e->tier));
      if (tl[0]) {
        ccord_text_draw(r, tl, strlen(tl), bx, card.y + 12.0f, &tsty, 0.0f);
        bx += ccord_text_measure(r, tl, strlen(tl), &tsty, 0.0f).x + 8.0f;
      }
      const char *al = plugin_api_class_label(e->api_class);
      ccord_text_style asty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_api_class_color(ui, e->api_class));
      ccord_text_draw(r, al, strlen(al), bx, card.y + 12.0f, &asty, 0.0f);

      ccord_text_draw_ellipsis(r, e->description && e->description[0] ? e->description : "No description",
                               strlen(e->description && e->description[0] ? e->description : "No description"),
                               card.x + 14.0f, card.y + 34.0f, w - 28.0f, &ms);
      char sub[224];
      snprintf(sub, sizeof sub, "by @%s  |  %s  |  %llu downloads",
               e->author, e->id, (unsigned long long)e->downloads);
      ccord_text_draw_ellipsis(r, sub, strlen(sub), card.x + 14.0f, card.y + ch - 28.0f, w - 224.0f, &vs);
      if (has_w) {
        ccord_color wc = e->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
        ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, wc);
        char wl[220];
        snprintf(wl, sizeof wl, "Warning: %s", e->warning_title);
        ccord_text_draw_ellipsis(r, wl, strlen(wl), card.x + 14.0f, card.y + 54.0f, w - 28.0f, &wst);
      }
      draw_button(ui, r, (ccord_rect){card.x + card.w - 200.0f, card.y + card.h - 34.0f, 76.0f, UIS_H_BTN_SM}, "Details", false);
      const char *albl = e->installing ? "Installing..."
                       : e->bundled ? "Bundled"
                       : !e->installed ? "Install"
                       : e->update_available ? (e->update_needs_review ? "Review Upd" : "Update")
                       : "Installed";
      draw_button(ui, r, (ccord_rect){card.x + card.w - 116.0f, card.y + card.h - 34.0f, 102.0f, UIS_H_BTN_SM},
                  albl, !e->installed || e->update_available);
      y += ch + 10.0f;
    }
    if (cst && cst->has_more) {
      ccord_rect r_more = {x, g->panel.y + g->panel.h - 44.0f, 148.0f, UIS_H_BTN_SM};
      draw_button(ui, r, r_more, cst->loading_more ? "Loading..." : "Load More", false);
      char pinfo[96];
      snprintf(pinfo, sizeof pinfo, "Loaded %zu of %zu plugins", cn, cst->total_count);
      ccord_text_draw_ellipsis(r, pinfo, strlen(pinfo), x + 160.0f, r_more.y + 6.0f, w - 168.0f, &vs);
    }
    return;
  }

  // Subtab 2: Updates & Settings
  if (s_plugin_subtab == 2) {
    const ccord_catalog_prefs *pr_cur = ccord_contrib_catalog_prefs(ui->d.contrib);
    ccord_catalog_prefs pr = pr_cur ? *pr_cur : (ccord_catalog_prefs){.auto_update_enabled = false, .auto_update_min_tier = 2};
    ccord_rect box = {x, y, w, 142.0f};
    uis_card(ui, r, box);
    ccord_text_draw_ellipsis(r, "Automatic Plugin Updates", 24, box.x + 14.0f, box.y + 12.0f, w - 80.0f, &ns);
    const char *anote = "Verifies SHA-256 and pauses if an update adds new Class 2/3 API permissions.";
    ccord_text_draw_ellipsis(r, anote, strlen(anote), box.x + 14.0f, box.y + 34.0f, w - 80.0f, &ms);
    ccord_rect sw_row = {box.x + 14.0f, box.y + 12.0f, box.w - 28.0f, 36.0f};
    uis_state swst = {.hover = ui_in_rect(ui->mx, ui->my, sw_row)};
    uis_toggle(ui, r, sw_row, pr.auto_update_enabled, swst, "swautoupd", 0);

    float tx = box.x + 14.0f, ty = box.y + 68.0f;
    static const char *const k_tlbl[3] = {"Official Only", "Official & Verified", "All Catalog"};
    static const float k_tw[3] = {104.0f, 134.0f, 118.0f};
    for (int k = 0; k < 3; k++) {
      draw_button(ui, r, (ccord_rect){tx, ty, k_tw[k], UIS_H_BTN_SM}, k_tlbl[k], pr.auto_update_min_tier == (uint8_t)(k + 1));
      tx += k_tw[k] + 8.0f;
    }
    const char *sline = (cst && cst->status_msg[0]) ? cst->status_msg : "Catalog ready";
    ccord_text_draw_ellipsis(r, sline, strlen(sline), box.x + 14.0f, box.y + box.h - 28.0f, w - 200.0f, &vs);
    draw_button(ui, r, (ccord_rect){box.x + box.w - 178.0f, box.y + box.h - 36.0f, 164.0f, UIS_H_BTN_SM}, "Run Safe Auto-Updates", true);

    y += box.h + 14.0f;
    char uhdr[64];
    snprintf(uhdr, sizeof uhdr, "PENDING UPDATES (%zu)", pend_upd);
    ccord_text_draw(r, uhdr, strlen(uhdr), x, y, &vs, 0.0f);
    y += 18.0f;

    size_t un = ui->d.contrib ? ccord_contrib_catalog_update_count(ui->d.contrib) : 0;
    size_t listed = 0;
    for (size_t i = 0; i < un; i++) {
      const ccord_catalog_entry *e = ccord_contrib_catalog_update_at(ui->d.contrib, i);
      if (!e || !e->update_available) continue;
      float ch = 72.0f;
      if (y + ch > g->panel.y + g->panel.h - 16.0f) break;
      listed++;
      ccord_rect uc = {x, y, w, ch};
      uis_card(ui, r, uc);
      char ut[192];
      snprintf(ut, sizeof ut, "%s  v%s -> v%s", e->name, e->installed_version, e->version);
      ccord_text_draw_ellipsis(r, ut, strlen(ut), uc.x + 14.0f, uc.y + 12.0f, w - 164.0f, &ns);
      ccord_text_style st_s = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control,
                                       e->update_needs_review ? COL(status_idle) : COL(status_online));
      const char *um = e->update_needs_review ? "Paused: New API Class permissions or warning - review required"
                                              : "Safe to auto-update (same API permissions)";
      ccord_text_draw_ellipsis(r, um, strlen(um), uc.x + 14.0f, uc.y + 38.0f, w - 164.0f, &st_s);
      draw_button(ui, r, (ccord_rect){uc.x + uc.w - 134.0f, uc.y + 20.0f, 120.0f, UIS_H_BTN_SM},
                  e->installing ? "Updating..." : (e->update_needs_review ? "Review & Update" : "Update Now"), true);
      y += ch + 8.0f;
    }
    if (listed == 0) {
      const char *okm = "All installed catalog plugins are up to date.";
      ccord_text_draw(r, okm, strlen(okm), x, y + 6.0f, &ms, w);
    }
    return;
  }

  // Subtab 0: Installed
  const char *drop_hint = s_plugin_status_msg[0] ? s_plugin_status_msg
                        : (cst && cst->status_msg[0]) ? cst->status_msg
                        : "Drag & drop a .cce plugin package into C-Cord to install, or browse Discover.";
  ccord_text_draw_ellipsis(r, drop_hint, strlen(drop_hint), x, y, w, &ms);
  y += 24.0f;

  size_t n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
  if (n == 0) {
    const char *none = "No extensions are loaded. Drop a .cce package or install one from Discover.";
    ccord_text_draw(r, none, strlen(none), x, y, &ms, w);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(ui->d.contrib, i);
    if (!p) continue;
    plugin_card_rects rc;
    float h = plugin_card_layout(g, i, p, y, &rc);
    if (y + h > g->panel.y + g->panel.h - 16.0f) break;
    uis_card(ui, r, rc.card);
    float nw = ccord_text_draw_ellipsis(r, p->name, strlen(p->name), rc.card.x + 14.0f, rc.card.y + 10.0f, w - 340.0f, &ns);
    float bx = rc.card.x + 14.0f + nw + 8.0f;
    bx += ccord_text_draw_ellipsis(r, p->version, strlen(p->version), bx, rc.card.y + 12.0f, 64.0f, &vs) + 8.0f;

    const char *tl = plugin_tier_label(p->tier);
    if (tl[0]) {
      ccord_text_style tsty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_tier_color(ui, p->tier));
      ccord_text_draw(r, tl, strlen(tl), bx, rc.card.y + 12.0f, &tsty, 0.0f);
      bx += ccord_text_measure(r, tl, strlen(tl), &tsty, 0.0f).x + 8.0f;
    }

    uint8_t api_cls = (uint8_t)(p->risk_class + 1);
    const char *al = plugin_api_class_label(api_cls);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_api_class_color(ui, api_cls));
    ccord_text_draw(r, al, strlen(al), bx, rc.card.y + 12.0f, &ts, 0.0f);

    ccord_color sc = p->state == CCORD_PLUGIN_READY ? COL(status_online)
                   : p->state == CCORD_PLUGIN_FAILED ? COL(danger)
                   : (p->state == CCORD_PLUGIN_STARTING || p->state == CCORD_PLUGIN_NEEDS_CONSENT) ? COL(status_idle)
                   : COL(status_offline);
    const char *sn = state_name(p->state);
    ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, sc);
    float sw_ = ccord_text_measure(r, sn, strlen(sn), &ss, 0.0f).x;
    ccord_draw_circle(r, rc.card.x + rc.card.w - 20.0f - sw_ - 10.0f, rc.card.y + 10.0f + ui_line_h(ui, ns.size) * 0.5f, 4.0f, sc);
    ccord_text_draw(r, sn, strlen(sn), rc.card.x + rc.card.w - 14.0f - sw_, rc.card.y + 12.0f, &ss, 0.0f);

    float info_y = rc.card.y + 36.0f;
    char perms[280];
    snprintf(perms, sizeof perms, "Permissions: %s", p->permissions && p->permissions[0] ? p->permissions : "none (API Class 1)");
    ccord_text_draw_ellipsis(r, perms, strlen(perms), rc.card.x + 14.0f, info_y, w - 28.0f, &vs);
    info_y += 18.0f;
    if (p->warning_sev != CCORD_WARN_NONE && p->warning_title && p->warning_title[0]) {
      ccord_color wc = p->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
      ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, wc);
      char wl[220];
      snprintf(wl, sizeof wl, "Warning: %s", p->warning_title);
      ccord_text_draw_ellipsis(r, wl, strlen(wl), rc.card.x + 14.0f, info_y, w - 28.0f, &wst);
      info_y += 18.0f;
    }
    if (p->error && p->error[0]) {
      ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(danger));
      ccord_text_draw_ellipsis(r, p->error, strlen(p->error), rc.card.x + 14.0f, info_y, w - 28.0f, &es);
    }

    const char *tlabel = p->state == CCORD_PLUGIN_NEEDS_CONSENT ? "Review Perms"
                       : (p->state == CCORD_PLUGIN_READY || p->state == CCORD_PLUGIN_STARTING) ? "Disable" : "Enable";
    draw_button(ui, r, rc.btn_toggle, tlabel, p->state == CCORD_PLUGIN_NEEDS_CONSENT);
    size_t ecnt = ccord_contrib_plugin_error_count(ui->d.contrib, p->id);
    char llabel[48];
    if (ecnt > 0) snprintf(llabel, sizeof llabel, "Log (%zu)", ecnt);
    else snprintf(llabel, sizeof llabel, "Debug Log");
    draw_button(ui, r, rc.btn_log, llabel, false);
    draw_button(ui, r, rc.btn_reload, "Reload", false);
    draw_button(ui, r, rc.btn_export, "Export .cce", false);
    if (rc.btn_update.w > 0.0f) {
      char ul[48];
      if (p->update_needs_review) snprintf(ul, sizeof ul, "Review Upd");
      else snprintf(ul, sizeof ul, "Update %s", p->update_version);
      draw_button(ui, r, rc.btn_update, ul, true);
    }
    if (rc.btn_auto.w > 0.0f) {
      draw_button(ui, r, rc.btn_auto, p->auto_update ? "Auto: On" : "Auto: Off", false);
    }
    if (rc.btn_uninstall.w > 0.0f) {
      draw_button(ui, r, rc.btn_uninstall, "Uninstall", false);
    }
    y += h + 10.0f;
  }
}

static void account_buttons(const ccord_ui *ui, const sgeom *g, ccord_rect *primary, ccord_rect *secondary)
{
  (void)ui;
  float by = g->content.y + g->content.h - 42.0f;
  *primary = (ccord_rect){g->content.x, by, 156.0f, UIS_H_BTN};
  *secondary = (ccord_rect){g->content.x + 168.0f, by, 132.0f, UIS_H_BTN};
}

static void draw_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, bool primary)
{
  if (rc.w <= 0.0f) return;
  uis_btn_size size = rc.h <= UIS_H_BTN_SM ? UIS_BTN_S_SM : rc.h >= UIS_H_BTN_LG ? UIS_BTN_S_LG : UIS_BTN_S_MD;
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, rc), .pressed = settings_pressed(ui, rc)};
  uint64_t id = (uint64_t)(rc.y * 4096.0f + rc.x);
  uis_button(ui, r, rc, label, primary ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, size, st, "sbtn", id);
}

static void draw_input_field(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const ccord_text_edit *ed,
                             bool focused, bool masked, const char *placeholder)
{
  uis_state st = {.focused = focused};
  uis_input(ui, r, box, ed, placeholder, st, masked, "sfield", (uint64_t)(box.y * 4096.0f + box.x));
}

static void draw_account(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  const ccord_account_view *v = &ui->account_view;
  float x = g->content.x, w = g->content.w;
  float y = g->content.y + 40.0f;
  ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
  ccord_text_draw_ellipsis(r, v->status, strlen(v->status), x, y, w, &ss);
  y += ui_line_h(ui, ss.size) + 4.0f;
  const char *detail_str = (ui->account_login_status[0] != '\0') ? ui->account_login_status : v->detail;
  if (detail_str && detail_str[0]) {
    ccord_color dcol = (ui->account_login_status[0] != '\0' && ui->account_login_is_error) ? COL(danger) : COL(text_secondary);
    ccord_text_style dstyle = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, dcol);
    ccord_vec2 d = ccord_text_measure(r, detail_str, strlen(detail_str), &dstyle, w);
    ccord_text_draw(r, detail_str, strlen(detail_str), x, y, &dstyle, w);
    y += d.y + 8.0f;
  }
  if (v->busy && !v->signed_in) {
    ccord_draw_rrect(r, (ccord_rect){x, y, w, 6.0f}, 3.0f, COL(bg_tertiary));
    ccord_draw_rrect(r, (ccord_rect){x, y, w * 0.4f, 6.0f}, 3.0f, COL(brand));
    y += 18.0f;
  }

  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  if (v->signed_in) {
    // Streamer Mode masks the account identity strings (local preference, design §2 Streamer Mode).
    char mname[128], muser[128], mid[32];
    const char *mdn = ui_settings_m6_mask(ui, v->display_name ? v->display_name : "", mname, sizeof mname);
    const char *mun = ui_settings_m6_mask(ui, v->username ? v->username : "", muser, sizeof muser);
    const char *mid_s = ui_settings_m6_mask(ui, v->user_id_text, mid, sizeof mid);
    char handle[CCORD_IDENTITY_NAME_BYTES + 2];
    snprintf(handle, sizeof handle, "@%s", mun);
    float colw = (w - 16.0f) * 0.5f;
    float y_left = y, y_right = y;
    label_value(ui, r, x, &y_left, colw, "DISPLAY NAME", mdn);
    label_value(ui, r, x + colw + 16.0f, &y_right, colw, "USERNAME", handle);
    y = y_left > y_right ? y_left : y_right;
    y_left = y;
    y_right = y;
    label_value(ui, r, x, &y_left, colw, "USER ID", mid_s);
    char guilds[256];
    int gn = snprintf(guilds, sizeof guilds, "%zu guild%s", v->guild_count, v->guild_count == 1 ? "" : "s");
    if (v->identity && v->guild_count > 0 && v->identity->guilds[0].name[0] && gn > 0 && (size_t)gn + 3 < sizeof guilds) {
      gn += snprintf(guilds + gn, sizeof guilds - (size_t)gn, ": %.60s", v->identity->guilds[0].name);
      if (v->guild_count > 1 && v->identity->guilds[1].name[0] && gn > 0 && (size_t)gn + 3 < sizeof guilds) {
        gn += snprintf(guilds + gn, sizeof guilds - (size_t)gn, ", %.60s", v->identity->guilds[1].name);
      }
      if (v->guild_count > 2 && gn > 0 && (size_t)gn + 5 < sizeof guilds) {
        snprintf(guilds + gn, sizeof guilds - (size_t)gn, ", ...");
      }
    }
    label_value(ui, r, x + colw + 16.0f, &y_right, colw, "GUILDS", guilds);
    y = y_left > y_right ? y_left : y_right;
  } else {
    ccord_rect em_box = account_email_box_rect(g);
    ccord_rect pw_box = account_password_box_rect(g);
    ccord_rect login_btn = account_login_btn_rect(g);
    ccord_text_draw(r, "EMAIL OR PHONE", 14, em_box.x, em_box.y - 15.0f, &ls, 0.0f);
    ccord_text_draw(r, "PASSWORD", 8, pw_box.x, pw_box.y - 15.0f, &ls, 0.0f);
    draw_input_field(ui, r, em_box, &ui->account_email_edit, ui->account_focus_field == 2, false, "you@example.com");
    draw_input_field(ui, r, pw_box, &ui->account_password_edit, ui->account_focus_field == 3, true, "Password");
    draw_button(ui, r, login_btn, "Log In to Discord", true);
  }

  ccord_rect tbox = account_token_box_rect(g);
#if defined(_WIN32)
  const char *tok_hdr = "STANDALONE DISCORD TOKEN (%APPDATA%\\C-Cord\\token)";
  const char *tok_ph = "Paste Discord user token or press Ctrl+V...";
#elif defined(__APPLE__)
  const char *tok_hdr = "STANDALONE DISCORD TOKEN (~/Library/Application Support/C-Cord/token)";
  const char *tok_ph = "Paste Discord user token or press Cmd+V...";
#else
  const char *tok_hdr = "STANDALONE DISCORD TOKEN ($XDG_CONFIG_HOME/ccord/token)";
  const char *tok_ph = "Paste Discord user token or press Ctrl+V...";
#endif
  ccord_text_draw(r, tok_hdr, strlen(tok_hdr), tbox.x, tbox.y - 15.0f, &ls, 0.0f);
  draw_input_field(ui, r, tbox, &ui->account_token_edit, ui->account_focus_field == 1, true, tok_ph);
  draw_button(ui, r, account_token_paste_rect(g), "Paste", false);
  draw_button(ui, r, account_token_save_rect(g), "Save & Connect", true);
  draw_button(ui, r, account_token_clear_rect(g), "Sign Out", false);

  // Multi-account switcher: the app pushes the keyring roster (names only, never tokens).
  {
    ccord_text_style rls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
    ccord_text_draw(r, "SAVED ACCOUNTS", 14, x, g->content.y + 338.0f, &rls, 0.0f);
    draw_button(ui, r, account_add_btn_rect(g), "Add Account", false);
    ccord_text_style rns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_normal));
    ccord_text_style ract = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(brand));
    size_t shown = ui->saved_account_count < SAVED_ACCOUNT_ROWS ? ui->saved_account_count : SAVED_ACCOUNT_ROWS;
    for (size_t i = 0; i < shown; i++) {
      const ccord_ui_saved_account *sa = &ui->saved_accounts[i];
      ccord_rect row = saved_account_row_rect(g, (int)i);
      uis_card(ui, r, row);
      if (sa->active) uis_list_row_fill(ui, r, row, (uis_state){.selected = true}, "sacc", (uint64_t)i);
      const char *name = sa->display_name[0] ? sa->display_name : (sa->username[0] ? sa->username : "Saved account");
      char line[160];
      snprintf(line, sizeof line, "%s%s", name, sa->active ? "  (active)" : "");
      ccord_text_draw_ellipsis(r, line, strlen(line), row.x + 8.0f,
                               row.y + (row.h - ui_line_h(ui, rns.size)) * 0.5f, row.w - 156.0f,
                               sa->active ? &ract : &rns);
      if (!sa->active) draw_button(ui, r, saved_account_switch_rect(g, (int)i), "Switch", false);
      draw_button(ui, r, saved_account_remove_rect(g, (int)i), "Remove", false);
    }
    if (ui->saved_account_count == 0) {
      ccord_text_style empty = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
      const char *msg = "No saved accounts yet. Sign in to add one to the switcher.";
      ccord_text_draw_ellipsis(r, msg, strlen(msg), x, g->content.y + 372.0f, w - 120.0f, &empty);
    } else if (ui->saved_account_count > SAVED_ACCOUNT_ROWS) {
      char more[48];
      snprintf(more, sizeof more, "+%zu more", ui->saved_account_count - SAVED_ACCOUNT_ROWS);
      ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_caption, COL(text_muted));
      ccord_text_draw(r, more, strlen(more), x, g->content.y + 372.0f + SAVED_ACCOUNT_ROWS * 40.0f + 8.0f, &ms, 0.0f);
    }
  }

  if (v->show_non_goals && v->non_goals && v->non_goal_count > 0 && v->signed_in && v->demo_chip_visible) {
    const size_t cols = 2, rows = 3;
    float ph = 26.0f + (float)rows * 17.0f + 12.0f;
    float py = g->content.y + g->content.h - 110.0f - ph;
    ccord_rect panel = {x, py, w, ph};
    uis_card(ui, r, panel);
    ccord_text_style pt = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(text_header));
    ccord_text_draw_ellipsis(r, "Not available with a Discord personal sign-in",
                             strlen("Not available with a Discord personal sign-in"), panel.x + 12.0f, panel.y + 8.0f,
                             panel.w - 24.0f, &pt);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
    float colw = (panel.w - 24.0f) / (float)cols;
    for (size_t i = 0; i < v->non_goal_count && i < cols * rows; i++) {
      float lx = panel.x + 12.0f + (float)(i / rows) * colw;
      float ly = panel.y + 26.0f + (float)(i % rows) * 17.0f;
      ccord_text_draw_ellipsis(r, v->non_goals[i], strlen(v->non_goals[i]), lx, ly, colw - 8.0f, &ps);
    }
  }

  ccord_text_style wls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  const char *web_head = "BILLING, NITRO, SHOP & ACCOUNT (OPEN IN WEB BROWSER)";
  ccord_text_draw(r, web_head, strlen(web_head), x, g->content.y + g->content.h - 98.0f, &wls, 0.0f);
  for (int i = 0; i < 5; i++) {
    draw_button(ui, r, account_web_btn_rect(g, i), k_web_labels[i], false);
  }

  ccord_rect primary = {0, 0, 0, 0}, secondary = {0, 0, 0, 0};
  account_buttons(ui, g, &primary, &secondary);
  if (v->signed_in) {
    if (v->can_refresh) draw_button(ui, r, primary, "Refresh", false);
    draw_button(ui, r, secondary, "Sign out", false);
  } else if (v->can_sign_in) {
    draw_button(ui, r, primary, "Sign in with Discord", true);
  }
}

// Secondary modal popup for 6-digit TOTP / 2FA verification (shown only when Discord requires 2FA).
static void draw_mfa_popup(ccord_ui *ui, ccord_renderer *r)
{
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, ui_alpha(COL(bg_tertiary), 0.72f));
  ccord_rect pop = mfa_popup_rect(ui);
  ccord_draw_rect(r, (ccord_rect){pop.x - 1.0f, pop.y - 1.0f, pop.w + 2.0f, pop.h + 2.0f}, COL(border_subtle));
  uis_card(ui, r, pop);
  ccord_push_clip(r, pop);

  ccord_draw_rect(r, (ccord_rect){pop.x, pop.y, pop.w, 4.0f}, COL(brand));

  const char *title = "Two-Factor Authentication";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
  ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
  ccord_text_draw(r, title, strlen(title), pop.x + (pop.w - tt.x) * 0.5f, pop.y + 22.0f, &ts, 0.0f);

  const char *sub = "Enter the 6-digit code from your authenticator app.";
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, 0.0f);
  float sub_x = st.x < pop.w - 32.0f ? pop.x + (pop.w - st.x) * 0.5f : pop.x + 16.0f;
  ccord_text_draw_ellipsis(r, sub, strlen(sub), sub_x, pop.y + 50.0f, pop.w - 32.0f, &ss);

  ccord_rect banner = {pop.x + 28.0f, pop.y + 80.0f, pop.w - 56.0f, 34.0f};
  if (ui->account_login_status[0] != '\0') {
    uis_card(ui, r, banner);
    ccord_color accent = ui->account_login_is_error ? COL(danger) : COL(brand);
    ccord_draw_rect(r, (ccord_rect){banner.x, banner.y, 4.0f, banner.h}, accent);
    ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control,
                                   ui->account_login_is_error ? COL(danger) : COL(text_header));
    ccord_text_draw_ellipsis(r, ui->account_login_status, strlen(ui->account_login_status), banner.x + 12.0f,
                             banner.y + (banner.h - ui_line_h(ui, es.size)) * 0.5f, banner.w - 18.0f, &es);
  }

  ccord_rect mbox = mfa_popup_input_rect(&pop);
  ccord_rect vbtn = mfa_popup_verify_rect(&pop);
  ccord_rect bbtn = mfa_popup_back_rect(&pop);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_draw(r, "6-DIGIT AUTHENTICATOR CODE", 26, mbox.x, mbox.y - 16.0f, &ls, 0.0f);
  draw_input_field(ui, r, mbox, &ui->account_mfa_edit, ui->account_focus_field == 4, false, "000000");
  draw_button(ui, r, vbtn, "Verify & Log In", true);
  draw_button(ui, r, bbtn, "Back to Login", false);

  ccord_pop_clip(r);
}

static void draw_login_screen(ccord_ui *ui, ccord_renderer *r)
{
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, ui->win_w, ui->win_h}, COL(bg_tertiary));

  ccord_rect card = login_card_rect(ui);
  ccord_draw_rrect(r, (ccord_rect){card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f},
                   uis_tokens_of(ui).r_surface + 1.0f, COL(border_subtle));
  uis_card(ui, r, card);
  ccord_push_clip(r, card);

  // Top Discord brand accent bar + badge
  ccord_draw_rrect(r, (ccord_rect){card.x, card.y, card.w, 8.0f}, 4.0f, COL(brand));
  ccord_rect badge = {card.x + (card.w - 96.0f) * 0.5f, card.y + 14.0f, 96.0f, 22.0f};
  ccord_draw_rrect(r, badge, 11.0f, COL(brand));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_on_brand));
  ccord_vec2 bt = ccord_text_measure(r, "DISCORD", 7, &bs, 0.0f);
  ccord_text_draw(r, "DISCORD", 7, badge.x + (badge.w - bt.x) * 0.5f, badge.y + (badge.h - bt.y) * 0.5f, &bs, 0.0f);

  // Header: "Welcome back!" + "We're so excited to see you again!"
  const char *title = "Welcome back!";
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
  ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
  ccord_text_draw(r, title, strlen(title), card.x + (card.w - tt.x) * 0.5f, card.y + 42.0f, &ts, 0.0f);

  const char *sub = "We're so excited to see you again!";
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, 0.0f);
  ccord_text_draw(r, sub, strlen(sub), card.x + (card.w - st.x) * 0.5f, card.y + 68.0f, &ss, 0.0f);

  // Two-tab bar: Tab 0 = Token Sign-In, Tab 1 = Email & Password
  static const char *const k_tab_labels[2] = {"Token Sign-In", "Email & Password"};
  for (int i = 0; i < 2; i++) {
    ccord_rect tr = login_tab_rect(&card, i);
    uis_state tst = {.hover = ui_in_rect(ui->mx, ui->my, tr)};
    uis_button(ui, r, tr, k_tab_labels[i], ui->login_tab == i ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY, UIS_BTN_S_MD,
               tst, "ltab", (uint64_t)i);
  }

  // Connecting state / Auth error banner / Info banner
  float cx = card.x + 32.0f, cw = card.w - 64.0f;
  ccord_rect banner = {cx, card.y + 136.0f, cw, 32.0f};
  bool connecting = (ui->d.session && ccord_session_state(ui->d.session) == CCORD_CONN_CONNECTING) ||
                    (ui->account_view.busy && !ui->account_view.signed_in);
  if (connecting) {
    uis_card(ui, r, banner);
    ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, 4.0f, banner.h}, 2.0f, COL(brand));
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(text_header));
    const char *cmsg = "Connecting to Discord Gateway...";
    ccord_text_draw_ellipsis(r, cmsg, strlen(cmsg), banner.x + 12.0f,
                             banner.y + (banner.h - ui_line_h(ui, cs.size)) * 0.5f, banner.w - 20.0f, &cs);
  } else if (ui->account_login_status[0] != '\0') {
    ccord_color accent = ui->account_login_is_error ? COL(danger) : COL(brand);
    uis_card(ui, r, banner);
    ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, 4.0f, banner.h}, 2.0f, accent);
    ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control,
                                   ui->account_login_is_error ? COL(danger) : COL(text_header));
    ccord_text_draw_ellipsis(r, ui->account_login_status, strlen(ui->account_login_status), banner.x + 12.0f,
                             banner.y + (banner.h - ui_line_h(ui, es.size)) * 0.5f, banner.w - 20.0f, &es);
  } else {
    uis_card(ui, r, banner);
    ccord_text_style is = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
    const char *imsg = ui->mobile_layout ? "Sign in to connect to Discord."
                                         : "Sign in with your Discord account to connect to the live Gateway.";
    ccord_text_draw_ellipsis(r, imsg, strlen(imsg), banner.x + 10.0f,
                             banner.y + (banner.h - ui_line_h(ui, is.size)) * 0.5f, banner.w - 20.0f, &is);
  }

  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  if (ui->login_tab == 0) {
    ccord_rect tbox = login_token_box_rect(&card);
    ccord_text_draw(r, "DISCORD USER TOKEN", 18, tbox.x, tbox.y - 16.0f, &ls, 0.0f);
    draw_input_field(ui, r, tbox, &ui->account_token_edit, ui->account_focus_field == 1, true,
                     ui->mobile_layout ? "Paste Discord user token..." : "Paste Discord user token or press Ctrl+V...");
    draw_button(ui, r, login_token_paste_rect(&card),
                ui->mobile_layout ? "Paste from Clipboard" : "Paste from Clipboard (Ctrl+V)", false);
    draw_button(ui, r, login_token_save_rect(&card), connecting ? "Connecting to Discord Gateway..." : "Connect to Discord", true);
#if defined(_WIN32)
    const char *h1 = "Token is saved locally in your Windows user profile at:";
    const char *h2 = "%APPDATA%\\C-Cord\\token";
#elif defined(__APPLE__)
    const char *h1 = "Token is saved locally with 0600 permissions at:";
    const char *h2 = "~/Library/Application Support/C-Cord/token";
#elif defined(__ANDROID__)
    const char *h1 = "Token is saved privately in app storage.";
    const char *h2 = "Stored locally with 0600 permissions.";
#else
    const char *h1 = "Token is saved locally with 0600 permissions at:";
    const char *h2 = "$XDG_CONFIG_HOME/ccord/token";
#endif
    ccord_text_draw_ellipsis(r, h1, strlen(h1), cx, card.y + 344.0f, cw, &hs);
    ccord_text_draw_ellipsis(r, h2, strlen(h2), cx, card.y + 362.0f, cw, &ls);
  } else {
    ccord_rect em_box = login_email_box_rect(&card);
    ccord_rect pw_box = login_password_box_rect(&card);
    ccord_rect lbtn = login_credentials_btn_rect(&card);
    ccord_text_draw(r, "EMAIL OR PHONE NUMBER", 21, em_box.x, em_box.y - 16.0f, &ls, 0.0f);
    draw_input_field(ui, r, em_box, &ui->account_email_edit, ui->account_focus_field == 2, false, "you@example.com");
    ccord_text_draw(r, "PASSWORD", 8, pw_box.x, pw_box.y - 16.0f, &ls, 0.0f);
    draw_input_field(ui, r, pw_box, &ui->account_password_edit, ui->account_focus_field == 3, true, "Password");
    draw_button(ui, r, lbtn, connecting ? "Connecting to Discord Gateway..." : "Connect to Discord", true);
    const char *h3 = ui->mobile_layout ? "CAPTCHA required? Switch to Token Sign-In."
                                       : "If Discord requires a browser CAPTCHA, switch to Token Sign-In.";
    ccord_text_draw_ellipsis(r, h3, strlen(h3), cx, card.y + 372.0f, cw, &hs);
  }

  ccord_pop_clip(r);
  if (ui->account_mfa_required) {
    draw_mfa_popup(ui, r);
  }
}

// One sidebar scroll button: the shared scroll arrow (uis_scroll_arrow), with its own audit namespace.
static void sidebar_scroll_btn(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, bool up, uint64_t id)
{
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, rc)};
  uis_scroll_arrow(ui, r, rc, up, st, "sbscroll", id);
}

void ui_settings_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->settings_open) return;
  settings_init_defaults(ui);
  settings_fix_page(ui);
  if (ui_is_login_screen(ui)) {
    draw_login_screen(ui, r);
    return;
  }
  sgeom g = geometry(ui);
  ccord_draw_rect(r, g.panel, COL(bg_primary));
  if (g.sidebar.w > 0.0f) {
    ccord_draw_rect(r, g.sidebar, COL(bg_secondary));
    if (ui->mobile_layout) {
      ccord_text_style sts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
      ccord_text_draw(r, "Settings", 8, g.sidebar.x + UIS_SP_4, g.close.y + UIS_SP_1, &sts, 0.0f);
    }
    ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
    int last_cat = -1;
    int order[SP_COUNT];
    int norder = sidebar_order(order);
    s_drawn.count = 0;
    s_drawn.body = ui->settings_page;
    ccord_push_clip(r, g.list);
    for (int k = 0; k < norder; k++) {
      int i = order[k];
      if (k_pages[i].category != last_cat) {
        last_cat = k_pages[i].category;
        float cy = g.page[i].y - 20.0f;
        ccord_text_draw(r, k_categories[last_cat], strlen(k_categories[last_cat]), g.page[i].x + 6.0f, cy, &hs, 0.0f);
      }
      if (s_drawn.count < 24) {
        s_drawn.page[s_drawn.count] = i;
        snprintf(s_drawn.label[s_drawn.count], sizeof s_drawn.label[0], "%s", k_pages[i].label);
        s_drawn.rect[s_drawn.count] = g.page[i];
        s_drawn.count++;
      }
      uis_state st = {.hover = i == ui->hover_settings_page, .selected = !ui->mobile_layout && i == ui->settings_page};
      uis_list_row_fill(ui, r, g.page[i], st, "spage", (uint64_t)i);
      ccord_color tc = st.selected ? COL(text_interactive_active) : st.hover ? COL(text_interactive_hover) : COL(text_normal);
      if (!ui->mobile_layout && !st.selected && !st.hover) tc = COL(text_muted);
      ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_body, tc);
      ccord_text_draw_ellipsis(r, k_pages[i].label, strlen(k_pages[i].label), g.page[i].x + 10.0f,
                               g.page[i].y + (g.page[i].h - ui_line_h(ui, ps.size)) * 0.5f, g.page[i].w - 16.0f, &ps);
      if (ui->mobile_layout) {
        ui_icon_chevron(r, g.page[i].x + g.page[i].w - UIS_SP_4, g.page[i].y + g.page[i].h * 0.5f, UIS_SP_3, ICON_RIGHT, COL(text_muted));
      }
    }
    ccord_pop_clip(r);
    if (g.sb_up.w > 0.0f) {
      sidebar_scroll_btn(ui, r, g.sb_up, true, 0);
      sidebar_scroll_btn(ui, r, g.sb_down, false, 1);
    }
  }

  if (g.back.w > 0.0f) {
    uis_button(ui, r, g.back, "< Back", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){0}, "sback", 0);
  }
  if (g.content.w > 0.0f) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
    const char *title = k_pages[ui->settings_page].label;
    ccord_text_draw(r, title, strlen(title), g.content.x, g.content.y, &ts, 0.0f);
    ccord_push_clip(r, g.panel);
    ccord_push_alpha(r, ui_enter(ui, "spagebody", (uint64_t)ui->settings_page, 160));   // a page eases in when chosen
    switch (ui->settings_page) {
      case SP_ACCOUNT: draw_account(ui, r, &g); break;
      case SP_PROFILE_EDIT: ui_settings_profile_draw(ui, r, g.content, g.panel); break;
      case SP_APPEARANCE: draw_appearance(ui, r, &g); break;
      case SP_VOICE: draw_voice(ui, r, &g); break;
      case SP_STREAMING: draw_streaming(ui, r, &g); break;
      case SP_TEXT_IMAGES: draw_text_images(ui, r, &g); break;
      case SP_NOTIFICATIONS:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_NOTIFICATIONS, g.content, g.panel);
        break;
      case SP_ACCESSIBILITY:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_ACCESSIBILITY, g.content, g.panel);
        break;
      case SP_STREAMER:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_STREAMER, g.content, g.panel);
        break;
      case SP_LANGUAGE:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_LANGUAGE, g.content, g.panel);
        break;
      case SP_ADVANCED:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_ADVANCED, g.content, g.panel);
        break;
      case SP_CCORD:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_CCORD, g.content, g.panel);
        break;
      case SP_KEYBINDS: draw_keybinds(ui, r, &g); break;
      case SP_PRIVACY:
      case SP_DEVICES:
      case SP_CONNECTIONS:
        ui_settings_m6_draw(ui, r, ui->settings_page - SP_PRIVACY, g.content, g.panel);
        break;
      case SP_PLUGINS:
        draw_extensions_row(ui, r, &g);
        draw_plugins(ui, r, &g);
        break;
      default: break;
    }
    s_last_drawn_page = ui->settings_page;
    ccord_pop_alpha(r);
    ccord_pop_clip(r);
  }

  // Close control with an ESC badge (desktop only).
  uis_tokens tk = uis_tokens_of(ui);
  if (!ui->mobile_layout) {
    ccord_rect badge = {g.close.x - 34.0f, g.close.y + 8.0f, 26.0f, 16.0f};
    uis_plate(ui, r, badge, tk.r_control, COL(bg_tertiary));
    ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, "ESC", 3, &es, 0.0f);
    ccord_text_draw(r, "ESC", 3, badge.x + (badge.w - et.x) * 0.5f, badge.y + (badge.h - et.y) * 0.5f, &es, 0.0f);
  }
  uis_state cst = {.hover = ui->hover_settings_close};
  ccord_color cc = uis_icon_button(ui, r, g.close, cst, "sclose", 0);
  ui_icon_close(r, g.close.x + g.close.w * 0.5f, g.close.y + g.close.h * 0.5f, UIS_ICON_GLYPH, cc);
  if (ui->settings_page == SP_ACCOUNT && ui->account_mfa_required) {
    draw_mfa_popup(ui, r);
  }
}

void ccord_ui_get_settings_debug(const ccord_ui *ui, ccord_ui_settings_debug *out)
{
  *out = (ccord_ui_settings_debug){0};
  if (!ui) return;
  sgeom g = geometry(ui);
  out->open = ui->settings_open;
  out->page = ui->settings_page;
  out->sidebar_count = s_drawn.count;
  for (int i = 0; i < s_drawn.count; i++) {
    out->sidebar[i].page = s_drawn.page[i];
    snprintf(out->sidebar[i].label, sizeof out->sidebar[i].label, "%s", s_drawn.label[i]);
    out->sidebar[i].rect = s_drawn.rect[i];
  }
  out->drawn_page = s_drawn.body;
  // The fixed-size snapshot predates the M6 pages: it keeps carrying the original nine tabs
  // (indices unchanged). The M6 tab rects come from ui_settings_m6_host_rects instead.
  out->page_count = SP_COUNT < 9 ? SP_COUNT : 9;
  for (int i = 0; i < out->page_count; i++) out->tabs[i] = g.page[i];
  for (int i = 0; i < 2; i++) out->display_mode[i] = chips_rect(&g, 0, i, 2);
  // Voice & Video rows scroll; report their drawn positions (shifted by the page offset).
  sgeom gv = g;
  if (ui->settings_page == SP_VOICE) gv.content.y -= s_voice_scroll;
  out->input_device = device_rect(&gv, 0);
  out->output_device = device_rect(&gv, 1);
  out->input_volume = vol_track_rect(&gv, 2);
  out->output_volume = vol_track_rect(&gv, 3);
  for (int i = 0; i < 2; i++) out->voice_mode[i] = chips_rect(&gv, 4, i, 2);
  for (int i = 0; i < 5; i++) {
    out->encoder[i] = chips_rect(&g, 0, i, 5);
    out->framework[i] = chips_rect(&g, 1, i, 5);
  }
  for (int i = 0; i < 3; i++) {
    out->codec[i] = chips_rect(&g, 4, i, 3);
    out->fps[i] = chips_rect(&g, 3, i, 3);
  }
  for (int i = 0; i < 4; i++) out->resolution[i] = chips_rect(&g, 2, i, 4);
  out->embed_toggle = toggle_rect(&g, 0);
  out->attachment_toggle = toggle_rect(&g, 1);
  out->hardware_toggle = toggle_rect(&gv, 8);
  out->plugins_switch = ui_settings_rect(ui, false);
  out->billing_btn = account_web_btn_rect(&g, 0);
  out->nitro_btn = account_web_btn_rect(&g, 1);
  out->shop_btn = account_web_btn_rect(&g, 2);
  out->boost_btn = account_web_btn_rect(&g, 3);
  out->quickcss_toggle = toggle_rect(&g, 5);
  out->reduced_motion_toggle = toggle_rect(&g, 7);
  out->notify_desktop_toggle = toggle_rect(&g, 3);
  for (int i = 0; i < 4; i++) out->notify_timeout[i] = chips_rect(&g, 4, i, 4);

  out->encoder_count = 5;
  out->codec_count = 3;
  out->framework_count = 5;
  out->resolution_count = 4;
  out->fps_count = 3;
  out->display_mode_value = ui->settings_display_mode;
  out->voice_mode_value = ui->settings_voice_mode;
  out->mic_device_value = ui->settings_mic_device;
  out->speaker_device_value = ui->settings_speaker_device;
  out->input_volume_value = ui->settings_input_volume;
  out->output_volume_value = ui->settings_output_volume;
  out->encoder_value = ui->settings_encoder;
  out->codec_value = ui->settings_codec;
  out->framework_value = ui->settings_framework;
  out->resolution_value = ui->settings_resolution;
  out->fps_value = ui->settings_fps;
  out->embeds = ui->settings_embeds;
  out->attachments = ui->settings_attachments;
  out->hardware_accel = ui->settings_hardware_accel;
  out->noise_suppression = ui->settings_noise_suppression;
  out->echo_cancel = ui->settings_echo_cancel;
  out->auto_gain = ui->settings_auto_gain;
  out->quickcss_enabled = ui->settings_quickcss_enabled;
  out->reduced_motion = ui->settings_reduced_motion;
  out->notify_desktop = ui->settings_notify_desktop;
  out->notify_timeout_ms = ui->settings_notify_timeout_ms;
  out->window_width = ui->settings_win_w;
  out->window_height = ui->settings_win_h;
  snprintf(out->quickcss, sizeof out->quickcss, "%.255s", ui->settings_quickcss);
  out->quickcss_box = quickcss_box_rect(&g);
  out->quickcss_focused = ui->settings_page == SP_APPEARANCE && ui->account_focus_field == 6;
  out->close_to_tray = ccord_ui_pref_close_to_background();
  out->close_to_tray_toggle = ui_settings_m6_close_tray_rect(ui);
  for (int i = 0; i < 4; i++) {
    out->keybind_rows[i] = keybind_rect(&g, i);
  }
  out->keybind_capture = s_ptt_armed ? 0 : (s_kb_armed >= 0 ? s_kb_armed + 1 : -1);
  out->saved_account_count = ui->saved_account_count;
  out->account_add_btn = account_add_btn_rect(&g);
  for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
    out->saved_account_ids[i] = ui->saved_accounts[i].user_id;
    out->saved_account_rows[i] = saved_account_row_rect(&g, (int)i);
    out->saved_account_switch[i] = saved_account_switch_rect(&g, (int)i);
    out->saved_account_remove[i] = saved_account_remove_rect(&g, (int)i);
  }
  out->account_token_box = account_token_box_rect(&g);
  out->account_token_paste_btn = account_token_paste_rect(&g);
  out->account_token_save_btn = account_token_save_rect(&g);
  out->account_token_clear_btn = account_token_clear_rect(&g);
  out->account_email_box = account_email_box_rect(&g);
  out->account_password_box = account_password_box_rect(&g);
  out->account_mfa_box = (ccord_rect){0};
  out->account_login_btn = account_login_btn_rect(&g);
  out->login_screen = ui_is_login_screen(ui);
  out->login_tab = ui->login_tab;
  out->account_focus_field = ui->account_focus_field;
  out->account_mfa_required = ui->account_mfa_required;
  snprintf(out->account_login_status, sizeof out->account_login_status, "%s", ui->account_login_status);
  if (out->login_screen) {
    ccord_rect card = login_card_rect(ui);
    out->login_card = card;
    out->login_tab_token = login_tab_rect(&card, 0);
    out->login_tab_credentials = login_tab_rect(&card, 1);
    out->account_token_box = login_token_box_rect(&card);
    out->account_token_paste_btn = login_token_paste_rect(&card);
    out->account_token_save_btn = login_token_save_rect(&card);
    out->account_email_box = login_email_box_rect(&card);
    out->account_password_box = login_password_box_rect(&card);
    out->account_login_btn = login_credentials_btn_rect(&card);
  }
  if (ui->account_mfa_required) {
    ccord_rect pop = mfa_popup_rect(ui);
    out->mfa_popup_rect = pop;
    out->account_mfa_box = mfa_popup_input_rect(&pop);
    out->account_login_btn = mfa_popup_verify_rect(&pop);
    out->mfa_back_btn = mfa_popup_back_rect(&pop);
  }
  out->account_token_focused = (ui->account_focus_field == 1);
  out->account_token_len = ui->account_token_edit.len;
  ui_settings_m6_debug(ui, out);   // M6 prefs seam (ui.c): local theme/zoom/streamer/notify mirrors
  // Governed account settings: the store is the single source of truth (REST answer / gateway
  // push). Report the confirmed object when it has arrived; the mirrors only carry Discord defaults
  // until then. Local prefs above (theme/zoom/streamer/notify_sounds/hardware_accel) stay mirror-based.
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  if (us) {
    out->developer_mode = us->developer_mode;
    snprintf(out->locale, sizeof out->locale, "%.15s", us->locale);
    out->explicit_content_filter = us->explicit_content_filter;
    out->animate_stickers = us->animate_stickers;
    out->gif_auto_play = us->gif_auto_play;
    out->animate_emoji = us->animate_emoji;
    out->enable_tts_command = us->enable_tts_command;
    out->stream_notifications_enabled = us->stream_notifications_enabled;
    out->default_guilds_restricted = us->default_guilds_restricted;
    out->allow_age_restricted = us->allow_access_to_age_restricted_servers;
  }
}

// M5 test/introspection seam (m5-ui-settings): drawn rects of the Voice & Video controls that the
// fixed ccord_ui_settings_debug struct cannot carry, plus the page scroll state. Not declared in a
// public header; test_ui_voice_settings.c prototypes it. Rects reflect the current scroll offset.
void ui_settings_voice_m5_debug(const ccord_ui *ui, ccord_rect *out_camera, ccord_rect *out_ptt_key,
                                ccord_rect *out_sens_track, ccord_rect *out_scroll_up, ccord_rect *out_scroll_down,
                                ccord_rect *out_diag, float *out_scroll, float *out_max_scroll, bool *out_in_voice);
void ui_settings_voice_m5_debug(const ccord_ui *ui, ccord_rect *out_camera, ccord_rect *out_ptt_key,
                                ccord_rect *out_sens_track, ccord_rect *out_scroll_up, ccord_rect *out_scroll_down,
                                ccord_rect *out_diag, float *out_scroll, float *out_max_scroll, bool *out_in_voice)
{
  sgeom g = geometry(ui);
  sgeom gv = g;
  gv.content.y -= s_voice_scroll;
  if (out_camera) *out_camera = device_rect(&gv, 9);
  if (out_ptt_key) *out_ptt_key = device_rect(&gv, 10);
  if (out_sens_track) *out_sens_track = vol_track_rect(&gv, 11);
  if (out_scroll_up) *out_scroll_up = voice_scroll_up_rect(&g);
  if (out_scroll_down) *out_scroll_down = voice_scroll_down_rect(&g);
  if (out_diag) *out_diag = (ccord_rect){g.content.x, row_y(&gv, VOICE_ROWS), g.content.w, VOICE_DIAG_H};
  if (out_scroll) *out_scroll = s_voice_scroll;
  if (out_max_scroll) *out_max_scroll = voice_max_scroll(&g);
  if (out_in_voice) *out_in_voice = ui->d.store && ccord_store_active_voice_channel(ui->d.store) != CCORD_ID_NONE;
}

void ccord_ui_open_account_settings(ccord_ui *ui)
{
  if (!ui) return;
  settings_init_defaults(ui);
  ui_open_settings(ui, true);
  ui->settings_page = SP_ACCOUNT;
  ui->settings_category = k_pages[SP_ACCOUNT].category;
  if (!ui->account_view.signed_in && ccord_store_self_id(ui->d.store) == CCORD_ID_NONE) {
    ui->login_screen = true;
    ui->account_focus_field = (ui->login_tab == 0) ? 1 : (ui->account_mfa_required ? 4 : 2);
  } else if (!ui->account_view.signed_in && ui->account_focus_field == 0) {
    ui->account_focus_field = 2;   // focus Email field by default
  }
}

bool ui_open_plugin_install_prompt(ccord_ui *ui, const char *plugin_id)
{
  if (!ui || !plugin_id || !plugin_id[0]) return false;
  size_t len = strlen(plugin_id);
  if (len == 0 || len > 64) return false;
  if (!((plugin_id[0] >= 'a' && plugin_id[0] <= 'z') || (plugin_id[0] >= '0' && plugin_id[0] <= '9'))) return false;
  for (size_t i = 0; i < len; i++) {
    char c = plugin_id[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
    if (c == '.' && i + 1 < len && plugin_id[i + 1] == '.') return false;
  }
  settings_init_defaults(ui);
  ui_open_settings(ui, true);
  ui->settings_page = SP_PLUGINS;
  ui->settings_category = k_pages[SP_PLUGINS].category;
  s_plugin_subtab = 1;
  s_plugin_log_id[0] = '\0';
  s_plugin_consent_id[0] = '\0';
  snprintf(s_plugin_detail_id, sizeof s_plugin_detail_id, "%s", plugin_id);
  if (ui->d.contrib) {
    ccord_contrib_catalog_fetch_detail(ui->d.contrib, plugin_id);
  }
  ui_notice(ui, "Review plugin before installing: %.48s", plugin_id);
  ui->dirty = true;
  return true;
}

void ccord_ui_set_login_tab(ccord_ui *ui, int tab)
{
  if (!ui) return;
  ui->login_tab = (tab == 1) ? 1 : 0;
  if (ui_is_login_screen(ui)) {
    ui->account_focus_field = (ui->login_tab == 0) ? 1 : (ui->account_mfa_required ? 4 : 2);
  }
  ui->dirty = true;
}

const char *ccord_ui_account_token_text(const ccord_ui *ui)
{
  return ui ? ccord_text_edit_cstr(&ui->account_token_edit) : "";
}

void ccord_ui_clear_account_token_text(ccord_ui *ui)
{
  if (!ui) return;
  if (ui->account_token_edit.len > 0) {
    memset(ui->account_token_edit.data, 0, ui->account_token_edit.len);
  }
  ccord_text_edit_clear(&ui->account_token_edit);
  if (ui->account_password_edit.len > 0) {
    memset(ui->account_password_edit.data, 0, ui->account_password_edit.len);
  }
  ccord_text_edit_clear(&ui->account_password_edit);
  ccord_text_edit_clear(&ui->account_mfa_edit);
  ui->account_focus_field = 0;
  ui->account_mfa_required = false;
  ui->account_login_is_error = false;
  ui->account_login_status[0] = '\0';
  ui->dirty = true;
}

void ccord_ui_account_login_fields(const ccord_ui *ui, const char **out_email,
                                   const char **out_password, const char **out_mfa_code)
{
  if (out_email) *out_email = ui ? ccord_text_edit_cstr(&ui->account_email_edit) : "";
  if (out_password) *out_password = ui ? ccord_text_edit_cstr(&ui->account_password_edit) : "";
  if (out_mfa_code) *out_mfa_code = ui ? ccord_text_edit_cstr(&ui->account_mfa_edit) : "";
}

void ccord_ui_set_account_login_field(ccord_ui *ui, int field, const char *utf8)
{
  if (!ui || !utf8) return;
  ccord_text_edit *dst = nullptr;
  if (field == 1) {
    dst = &ui->account_email_edit;
    if (ui_is_login_screen(ui)) ui->login_tab = 1;
  } else if (field == 2) {
    dst = &ui->account_password_edit;
    if (ui_is_login_screen(ui)) ui->login_tab = 1;
  } else if (field == 3) {
    dst = &ui->account_mfa_edit;
  }
  if (!dst) return;
  ccord_text_edit_clear(dst);
  if (utf8[0]) {
    ccord_error ierr = {0};
    (void)ccord_text_edit_insert(dst, utf8, strlen(utf8), &ierr);
  }
  ui->dirty = true;
}

void ccord_ui_set_login_status(ccord_ui *ui, const char *status_msg, bool mfa_required)
{
  if (!ui) return;
  bool was_mfa = ui->account_mfa_required;
  ui->account_mfa_required = mfa_required;
  ui->account_login_is_error = (!mfa_required && status_msg && status_msg[0] != '\0');
  if (status_msg && status_msg[0]) {
    snprintf(ui->account_login_status, sizeof ui->account_login_status, "%s", status_msg);
  } else {
    ui->account_login_status[0] = '\0';
  }
  if (mfa_required) {
    ui->login_tab = 1;
    ui->account_focus_field = 4;
    if (!was_mfa) {
      ccord_text_edit_clear(&ui->account_mfa_edit);
      ccord_rect pop = mfa_popup_rect(ui);
      ui->text_area = mfa_popup_input_rect(&pop);
      if (ui->touch_mode) ui->touch_text_focused = true;
    }
  }
  ui->dirty = true;
}

void ccord_ui_set_login_busy(ccord_ui *ui, const char *status_msg)
{
  if (!ui) return;
  ui->account_login_is_error = false;
  if (status_msg && status_msg[0]) {
    snprintf(ui->account_login_status, sizeof ui->account_login_status, "%s", status_msg);
  } else {
    ui->account_login_status[0] = '\0';
  }
  ui->dirty = true;
}

void ccord_ui_set_login_error(ccord_ui *ui, const char *status_msg, bool keep_mfa)
{
  if (!ui) return;
  ui->account_mfa_required = keep_mfa;
  ui->account_login_is_error = true;
  if (status_msg && status_msg[0]) {
    snprintf(ui->account_login_status, sizeof ui->account_login_status, "%s", status_msg);
  } else {
    ui->account_login_status[0] = '\0';
  }
  if (keep_mfa) ui->account_focus_field = 4;
  ui->dirty = true;
}
