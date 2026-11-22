// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_internal.h"

const page_def st_k_pages[SP_COUNT] = {
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

const char *const st_k_categories[CAT_COUNT] = {"USER SETTINGS", "APP SETTINGS", "C-CORD", "EXTENSIONS"};

// M5: device lists come from the platform (SDL) enumeration, never hardcoded names.
// Entry 0 of every selector is "Default"; entries 1..N are the enumerated devices.
#if defined(_WIN32)
const char *const st_k_encoders[STREAM_ENC_COUNT] = {"NVENC", "AMF", "Quick Sync", "Media Found.", "OpenH264"};
const int st_k_encoder_vals[STREAM_ENC_COUNT] = {
  (int)CCORD_STREAM_ENCODER_NVENC, (int)CCORD_STREAM_ENCODER_AMF, (int)CCORD_STREAM_ENCODER_QSV,
  (int)CCORD_STREAM_ENCODER_MF, (int)CCORD_STREAM_ENCODER_OPENH264,
};
const char *const st_k_frameworks[STREAM_FW_COUNT] = {"Win. Capture", "DXGI Dup", "GDI"};
const int st_k_framework_vals[STREAM_FW_COUNT] = {
  (int)CCORD_STREAM_FRAMEWORK_WGC, (int)CCORD_STREAM_FRAMEWORK_DXGI, (int)CCORD_STREAM_FRAMEWORK_GDI,
};
#elif defined(__APPLE__)
const char *const st_k_encoders[STREAM_ENC_COUNT] = {"VideoToolbox", "x264", "OpenH264"};
const int st_k_encoder_vals[STREAM_ENC_COUNT] = {
  (int)CCORD_STREAM_ENCODER_VIDEOTOOLBOX, (int)CCORD_STREAM_ENCODER_X264, (int)CCORD_STREAM_ENCODER_OPENH264,
};
const char *const st_k_frameworks[STREAM_FW_COUNT] = {"ScreenCaptureKit", "CoreGraphics"};
const int st_k_framework_vals[STREAM_FW_COUNT] = {
  (int)CCORD_STREAM_FRAMEWORK_SCK, (int)CCORD_STREAM_FRAMEWORK_CG,
};
#else
const char *const st_k_encoders[STREAM_ENC_COUNT] = {"NVENC", "VAAPI", "AMF", "Vulkan", "OpenH264"};
const int st_k_encoder_vals[STREAM_ENC_COUNT] = {
  (int)CCORD_STREAM_ENCODER_NVENC, (int)CCORD_STREAM_ENCODER_VAAPI, (int)CCORD_STREAM_ENCODER_AMF,
  (int)CCORD_STREAM_ENCODER_VULKAN, (int)CCORD_STREAM_ENCODER_OPENH264,
};
const char *const st_k_frameworks[STREAM_FW_COUNT] = {"PipeWire", "DMA-BUF", "NVFBC", "Vulkan", "X11"};
const int st_k_framework_vals[STREAM_FW_COUNT] = {
  (int)CCORD_STREAM_FRAMEWORK_PIPEWIRE, (int)CCORD_STREAM_FRAMEWORK_DMABUF, (int)CCORD_STREAM_FRAMEWORK_NVFBC,
  (int)CCORD_STREAM_FRAMEWORK_VULKAN, (int)CCORD_STREAM_FRAMEWORK_X11,
};
#endif
int st_find_val_index(const int *vals, int count, int value)
{
  for (int i = 0; i < count; i++) {
    if (vals[i] == value) return i;
  }
  return -1;
}
const char *const st_k_codecs[2] = {"H.264", "AV1"};
const char *const st_k_resolutions[4] = {"720p", "1080p", "1440p", "Source"};
const char *const st_k_fps[3] = {"15", "30", "60"};
const char *const st_k_display_mode[2] = {"Compact", "Cozy"};
const char *const st_k_voice_mode[2] = {"Voice Activity", "Push to Talk"};
const char *const st_k_web_labels[5] = {"Billing", "Nitro", "Shop", "Server Boost", "Account"};
const char *const st_k_web_urls[5] = {
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
st_dev_cache st_s_dev;
bool st_s_ptt_armed;        // keybind capture: the next key press binds Push to Talk
int st_s_kb_armed = -1;     // keybind capture on the Keybinds page: settings_keybinds index, -1 = none
int st_s_plugin_kb_armed = -1; // keybind capture for plugin keybind row index, -1 = none
// Press feedback: Settings never sees the button release, so a left press holds the "pressed" look for a short time.
float st_s_press_x;
float st_s_press_y;
uint64_t st_s_press_until_ms;
float st_s_voice_scroll;    // Voice & Video page scroll offset in px
ccord_scroller st_s_voice_glide;   // wheel glide of st_s_voice_scroll (scroll.h)
float st_s_page_scroll;     // vertical scroll offset for tall Settings pages on mobile / short viewports
float st_s_plugins_content_h;
float st_s_login_scroll;    // vertical scroll offset for Login/MFA card when keyboard shrinks viewport
bool st_s_save_pending;
ccord_ui *st_s_save_ui;
uint64_t st_s_save_due_ms;
int64_t st_s_hover_key = -1;
int st_s_last_drawn_page = -1;

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

void st_voice_devices_refresh(ccord_ui *ui, bool force)
{
  if (st_s_dev.valid && !force && ui->now_ms >= st_s_dev.stamp_ms && ui->now_ms - st_s_dev.stamp_ms < VOICE_DEV_REFRESH_MS) {
    return;
  }
  st_s_dev.n_in = ccord_audio_devices_list(true, st_s_dev.in, VOICE_DEV_MAX);
  st_s_dev.n_out = ccord_audio_devices_list(false, st_s_dev.out, VOICE_DEV_MAX);
  st_s_dev.n_cam = ccord_camera_list(st_s_dev.cam, VOICE_DEV_MAX);
  st_s_dev.valid = true;
  st_s_dev.stamp_ms = ui->now_ms;
  // Reconcile the dropdown index from the persisted id when that device is present. This is
  // deliberately non-destructive: a missing id (unplugged device, or a transient dummy-driver id)
  // must not clobber the user's selection back to Default.
  int idx = voice_audio_index_of_id(ui->settings_mic_device_id, st_s_dev.in, st_s_dev.n_in);
  if (idx >= 0) ui->settings_mic_device = idx;
  if (ui->settings_mic_device > (int)st_s_dev.n_in) ui->settings_mic_device = 0;
  idx = voice_audio_index_of_id(ui->settings_speaker_device_id, st_s_dev.out, st_s_dev.n_out);
  if (idx >= 0) ui->settings_speaker_device = idx;
  if (ui->settings_speaker_device > (int)st_s_dev.n_out) ui->settings_speaker_device = 0;
  idx = voice_camera_index_of_id(ui->settings_camera_device_id, st_s_dev.cam, st_s_dev.n_cam);
  if (idx >= 0) ui->settings_camera_device = idx;
  if (ui->settings_camera_device > (int)st_s_dev.n_cam) ui->settings_camera_device = 0;
}

int st_voice_in_count(void) { return 1 + (int)st_s_dev.n_in; }
int st_voice_out_count(void) { return 1 + (int)st_s_dev.n_out; }
int st_voice_cam_count(void) { return 1 + (int)st_s_dev.n_cam; }

const char *st_voice_input_name(const ccord_ui *ui)
{
  int idx = ui->settings_mic_device;
  if (idx <= 0 || idx > (int)st_s_dev.n_in) return "Default";
  return st_s_dev.in[idx - 1].name;
}

const char *st_voice_output_name(const ccord_ui *ui)
{
  int idx = ui->settings_speaker_device;
  if (idx <= 0 || idx > (int)st_s_dev.n_out) return "Default";
  return st_s_dev.out[idx - 1].name;
}

const char *st_voice_camera_name(const ccord_ui *ui)
{
  if (st_s_dev.n_cam == 0) return "No camera detected";
  int idx = ui->settings_camera_device;
  if (idx <= 0 || idx > (int)st_s_dev.n_cam) return "Default";
  return st_s_dev.cam[idx - 1].name;
}

void st_voice_select_input(ccord_ui *ui, int idx)
{
  ui->settings_mic_device = idx;
  if (idx <= 0) ui->settings_mic_device_id[0] = '\0';
  else snprintf(ui->settings_mic_device_id, sizeof ui->settings_mic_device_id, "%s", st_s_dev.in[idx - 1].id);
}

void st_voice_select_output(ccord_ui *ui, int idx)
{
  ui->settings_speaker_device = idx;
  if (idx <= 0) ui->settings_speaker_device_id[0] = '\0';
  else snprintf(ui->settings_speaker_device_id, sizeof ui->settings_speaker_device_id, "%s", st_s_dev.out[idx - 1].id);
}

void st_voice_select_camera(ccord_ui *ui, int idx)
{
  ui->settings_camera_device = idx;
  if (idx <= 0) ui->settings_camera_device_id[0] = '\0';
  else snprintf(ui->settings_camera_device_id, sizeof ui->settings_camera_device_id, "%s", st_s_dev.cam[idx - 1].id);
}

// SDL scancode for a UI key event. The platform only forwards the ccord_key set, so capture binds
// exactly those keys; the stored value is a real SDL scancode the app polls with SDL_GetKeyboardState.
SDL_Scancode st_key_to_scancode(ccord_key k)
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

void st_ptt_binding_name(const ccord_ui *ui, char *out, size_t cap)
{
  if (ui->settings_ptt_scancode <= 0 || ui->settings_ptt_scancode >= SDL_SCANCODE_COUNT) {
    snprintf(out, cap, "Not bound");
    return;
  }
  const char *name = SDL_GetScancodeName((SDL_Scancode)ui->settings_ptt_scancode);
  snprintf(out, cap, "%s", name && name[0] ? name : "Unknown key");
}

// ---- rebindable action keybinds (Keybinds page) -------------------------------------------------

void st_keybind_name(int scancode, uint8_t mods, char *out, size_t cap)
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
    if (st_key_to_scancode(ev->key) != (SDL_Scancode)sc) return false;
    if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_ALT | CCORD_MOD_SUPER)) != 0) return false;
    if (ui->edit.len > 0 || ui->editing_message != CCORD_ID_NONE) return false;
    return true;
  }
  if (which < 0 || which >= 3) return false;
  int sc = ui->settings_keybinds[which].scancode;
  if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return false;
  if (st_key_to_scancode(ev->key) != (SDL_Scancode)sc) return false;
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

// Sidebar scroll offset in px (the up and down buttons below the list move it one row at a time; st_geometry() clamps).
float st_s_sidebar_scroll;
#define SIDEBAR_HEAD_H 48.0f    // space above the first sidebar row
#define SIDEBAR_SCROLL_H 40.0f  // strip under the list that holds the scroll buttons, when the list overflows


// Sidebar display order: pages grouped by category (USER, APP, EXTENSIONS), enum order within a
// category. The M6 account pages are appended to the enum but belong under USER SETTINGS.
int st_sidebar_order(int out[SP_COUNT])
{
  int n = 0;
  for (int c = 0; c < CAT_COUNT; c++) {
    for (int i = 0; i < SP_COUNT; i++) {
      if (i != SP_PROFILE_EDIT && st_k_pages[i].category == c) out[n++] = i;
      if (i == SP_ACCOUNT && st_k_pages[SP_PROFILE_EDIT].category == c) out[n++] = SP_PROFILE_EDIT;   // right after My Account
    }
  }
  return n;
}

// What the last settings draw produced, read through ccord_ui_get_settings_debug.
st_drawn_cache st_s_drawn = {.body = -1};

// A page index that has a sidebar entry. A stale index (the retired slot, out of range) falls back to My Account.
static bool page_valid(int page)
{
  return page >= 0 && page < SP_COUNT && st_k_pages[page].label != nullptr;
}

void st_settings_fix_page(ccord_ui *ui)
{
  if (page_valid(ui->settings_page)) return;
  ui->settings_page = SP_ACCOUNT;
  ui->settings_category = st_k_pages[SP_ACCOUNT].category;
  ui->dirty = true;
}

// Up/Down on the sidebar: previous/next entry in the order the sidebar draws.
bool st_sidebar_key_step(ccord_ui *ui, int dir)
{
  int order[SP_COUNT];
  int n = st_sidebar_order(order);
  int cur = -1;
  for (int k = 0; k < n; k++) {
    if (order[k] == ui->settings_page) cur = k;
  }
  int next = cur < 0 ? 0 : cur + dir;
  if (next < 0 || next >= n) return true;
  ui->settings_page = order[next];
  ui->settings_category = st_k_pages[order[next]].category;
  ui->dirty = true;
  return true;
}

sgeom st_geometry(const ccord_ui *ui)
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
    int n = st_sidebar_order(order);
    float total = 0.0f;
    for (int k = 0; k < n; k++) {
      if (k == 0 || st_k_pages[order[k]].category != st_k_pages[order[k - 1]].category) total += (k != 0 ? 12.0f : 0.0f) + 24.0f;
      total += pitch;
    }
    float top = st + UIS_SP_3 + UIS_ICON_BTN + UIS_SP_2;
    float view_h = uh - (top - st) - UIS_SP_2;
    g.list = (ccord_rect){sl, top, uw, view_h};
    g.step = pitch;
    float max_scroll = total > view_h ? total - view_h : 0.0f;
    if (st_s_sidebar_scroll > max_scroll) st_s_sidebar_scroll = max_scroll;
    if (st_s_sidebar_scroll < 0.0f) st_s_sidebar_scroll = 0.0f;
    float x = sl + UIS_SP_4, w = uw - 2.0f * UIS_SP_4;
    float y = top - st_s_sidebar_scroll;
    for (int k = 0; k < n; k++) {
      int i = order[k];
      if (k == 0 || st_k_pages[i].category != st_k_pages[order[k - 1]].category) {
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
  int n = st_sidebar_order(order);
  float total = 0.0f;
  for (int k = 0; k < n; k++) {
    if (k == 0 || st_k_pages[order[k]].category != st_k_pages[order[k - 1]].category) total += (k != 0 ? 12.0f : 0.0f) + 24.0f;
    total += pitch;
  }
  float top = g.panel.y + SIDEBAR_HEAD_H;
  float full_h = ph - SIDEBAR_HEAD_H - 8.0f;
  bool scrolls = total > full_h;
  float view_h = scrolls ? full_h - SIDEBAR_SCROLL_H : full_h;
  g.list = (ccord_rect){g.sidebar.x, top, sb, view_h};
  g.step = pitch;
  float max_scroll = total > view_h ? total - view_h : 0.0f;
  if (st_s_sidebar_scroll > max_scroll) st_s_sidebar_scroll = max_scroll;
  if (st_s_sidebar_scroll < 0.0f) st_s_sidebar_scroll = 0.0f;
  if (scrolls) {
    float by = top + view_h + 4.0f;
    g.sb_up = (ccord_rect){g.sidebar.x + 14.0f, by, UIS_ICON_BTN, UIS_ICON_BTN};
    g.sb_down = (ccord_rect){g.sidebar.x + 14.0f + UIS_ICON_BTN + 4.0f, by, UIS_ICON_BTN, UIS_ICON_BTN};
  } else {
    g.sb_up = (ccord_rect){0.0f, 0.0f, 0.0f, 0.0f};
    g.sb_down = g.sb_up;
  }
  float x = g.sidebar.x + 14.0f, w = sb - 28.0f;
  float y = top - st_s_sidebar_scroll;
  for (int k = 0; k < n; k++) {
    int i = order[k];
    if (k == 0 || st_k_pages[i].category != st_k_pages[order[k - 1]].category) {
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
  sgeom g = st_geometry(ui);
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
  sgeom g = st_geometry(ui);
  if (out_tabs) {
    out_tabs[CCORD_M6_PAGE_PRIVACY] = g.page[SP_PRIVACY];
    out_tabs[CCORD_M6_PAGE_DEVICES] = g.page[SP_DEVICES];
    out_tabs[CCORD_M6_PAGE_CONNECTIONS] = g.page[SP_CONNECTIONS];
  }
  if (out_account_web_btn) *out_account_web_btn = st_account_web_btn_rect(ui, &g, 4);
}

// M6 preference-page sidebar wiring (settings_m6_prefs.h): tab rects of the five preference pages.
void ui_settings_m6_prefs_host_rects(const ccord_ui *ui, ccord_rect out_tabs[CCORD_M6_PREFS_COUNT])
{
  if (!out_tabs) return;
  sgeom g = st_geometry(ui);
  out_tabs[CCORD_M6_PREFS_NOTIFICATIONS] = g.page[SP_NOTIFICATIONS];
  out_tabs[CCORD_M6_PREFS_ACCESSIBILITY] = g.page[SP_ACCESSIBILITY];
  out_tabs[CCORD_M6_PREFS_STREAMER] = g.page[SP_STREAMER];
  out_tabs[CCORD_M6_PREFS_LANGUAGE] = g.page[SP_LANGUAGE];
  out_tabs[CCORD_M6_PREFS_ADVANCED] = g.page[SP_ADVANCED];
  out_tabs[CCORD_M6_PREFS_CCORD] = g.page[SP_CCORD];
}

size_t ui_settings_sidebar_pages(const ccord_ui *ui, const char **out_category, ccord_rect *out_rect, int *out_page, size_t cap)
{
  sgeom g = st_geometry(ui);
  int order[SP_COUNT];
  int n = st_sidebar_order(order);
  size_t w = 0;
  for (int k = 0; k < n && w < cap; k++, w++) {
    if (out_category) out_category[w] = st_k_categories[st_k_pages[order[k]].category];
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

// ---- shared row st_geometry (used by both drawing and hit-testing) ----

float st_row_y(const sgeom *g, int row)
{
  return g->content.y + 44.0f + (float)row * 54.0f;
}

// The row a toggle sits in: UIS_H_BTN_SM tall, the switch is centred in it and flush right.
ccord_rect st_row_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x, st_row_y(g, row), g->content.w, UIS_H_BTN_SM};
}

ccord_rect st_toggle_rect(const sgeom *g, int row)
{
  return uis_toggle_rect(st_row_rect(g, row));
}

ccord_rect st_chips_rect(const sgeom *g, int row, int opt, int count)
{
  if (count < 1) return (ccord_rect){0, 0, 0, 0};
  float gap = 8.0f;
  float cw = (g->content.w - gap * (float)(count - 1)) / (float)count;
  if (cw > 132.0f) cw = 132.0f;
  if (cw < 44.0f) cw = 44.0f;
  return (ccord_rect){g->content.x + (float)opt * (cw + gap), st_row_y(g, row) + 20.0f, cw, UIS_H_CHIP};
}

ccord_rect st_device_rect(const sgeom *g, int row)
{
  float w = g->content.w * 0.7f;
  if (w > 300.0f) w = 300.0f;
  return (ccord_rect){g->content.x, st_row_y(g, row) + 18.0f, w, UIS_H_BTN_SM};
}

// Volume row: [-] track [+] value. The buttons are UIS_H_BTN_SM squares; the track is UIS_SLIDER_H tall and
// centred on the buttons.
ccord_rect st_vol_minus_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x, st_row_y(g, row) + 18.0f, UIS_H_BTN_SM, UIS_H_BTN_SM};
}

ccord_rect st_vol_track_rect(const sgeom *g, int row)
{
  float tw = g->content.w < 330.0f ? fmaxf(g->content.w - 130.0f, 80.0f) : 200.0f;
  return (ccord_rect){g->content.x + 36.0f, st_row_y(g, row) + 28.0f, tw, UIS_SLIDER_H};
}

ccord_rect st_vol_plus_rect(const sgeom *g, int row)
{
  ccord_rect tr = st_vol_track_rect(g, row);
  return (ccord_rect){tr.x + tr.w + 8.0f, st_row_y(g, row) + 18.0f, UIS_H_BTN_SM, UIS_H_BTN_SM};
}

// Streaming rows: on narrow viewports (< 420dp), 5-chip rows wrap into 3 + 2 so encoder/framework
// labels remain readable instead of crushing to ~55dp; desktop layout stays pixel-identical.
static bool stream_wraps_row(const sgeom *g, int row)
{
  if (g->content.w >= 420.0f) return false;
  if (row == 0 && STREAM_ENC_COUNT > 3) return true;
  if (row == 1 && STREAM_FW_COUNT > 3) return true;
  return false;
}

float st_stream_row_y(const sgeom *g, int row)
{
  float extra = 0.0f;
  for (int r = 0; r < row; r++) {
    if (stream_wraps_row(g, r)) extra += UIS_H_CHIP + 6.0f;
  }
  return st_row_y(g, row) + extra;
}

ccord_rect st_stream_auto_pill_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x + g->content.w - 92.0f, st_stream_row_y(g, row) - 4.0f, 92.0f, UIS_H_CHIP};
}

ccord_rect st_stream_chip_rect(const sgeom *g, int row, int opt, int count)
{
  if (count < 1) return (ccord_rect){0, 0, 0, 0};
  float ry = st_stream_row_y(g, row);
  if (stream_wraps_row(g, row)) {
    int per_row = 3;
    int sub_row = opt / per_row;
    int col = opt % per_row;
    float gap = 6.0f;
    float cw = (g->content.w - gap * (float)(per_row - 1)) / (float)per_row;
    if (cw > 132.0f) cw = 132.0f;
    if (cw < 44.0f) cw = 44.0f;
    return (ccord_rect){g->content.x + (float)col * (cw + gap),
                        ry + 20.0f + (float)sub_row * (UIS_H_CHIP + 6.0f), cw, UIS_H_CHIP};
  }
  float gap = g->content.w < 380.0f ? 6.0f : 8.0f;
  float cw = (g->content.w - gap * (float)(count - 1)) / (float)count;
  if (cw > 132.0f) cw = 132.0f;
  if (cw < 44.0f) cw = 44.0f;
  return (ccord_rect){g->content.x + (float)opt * (cw + gap), ry + 20.0f, cw, UIS_H_CHIP};
}

// Keybind box (click it, then press the chord) and the "Automatic" pill at the end of a streaming chip row.
ccord_rect st_keybind_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x, st_row_y(g, row) + 18.0f, 168.0f, UIS_H_BTN_SM};
}

ccord_rect st_auto_pill_rect(const sgeom *g, int row)
{
  return (ccord_rect){g->content.x + g->content.w - 92.0f, st_row_y(g, row) - 4.0f, 92.0f, UIS_H_CHIP};
}

ccord_rect st_extensions_toggle_rect(const sgeom *g)
{
  return st_toggle_rect(g, 0);
}

float st_account_web_y(const ccord_ui *ui, const sgeom *g)
{
  size_t shown = ui ? (ui->saved_account_count < SAVED_ACCOUNT_ROWS ? ui->saved_account_count : SAVED_ACCOUNT_ROWS) : 0;
  float min_y = g->content.y + 372.0f + (float)(shown > 0 ? shown : 1u) * 40.0f + 32.0f;
  if (ui && ui->saved_account_count > SAVED_ACCOUNT_ROWS) min_y += 22.0f;
  float bottom_y = g->content.y + g->content.h - (g->content.w < 380.0f ? 122.0f : 80.0f);
  return bottom_y > min_y ? bottom_y : min_y;
}

ccord_rect st_account_web_btn_rect(const ccord_ui *ui, const sgeom *g, int idx)
{
  float by = st_account_web_y(ui, g);
  if (g->content.w < 380.0f) {
    float gap = 6.0f;
    int row = idx < 3 ? 0 : 1;
    int col = idx < 3 ? idx : (idx - 3);
    int cols = row == 0 ? 3 : 2;
    float bw = floorf((g->content.w - (float)(cols - 1) * gap) / (float)cols);
    if (bw < 60.0f) bw = 60.0f;
    return (ccord_rect){g->content.x + (float)col * (bw + gap), by + (float)row * (UIS_H_BTN + 6.0f), bw, UIS_H_BTN};
  }
  float gap = 8.0f;
  float bw = (g->content.w - 4.0f * gap) / 5.0f;
  if (bw < 60.0f) bw = 60.0f;
  return (ccord_rect){g->content.x + (float)idx * (bw + gap), by, bw, UIS_H_BTN};
}

ccord_rect st_account_email_box_rect(const sgeom *g)
{
  float w = (g->content.w - 8.0f) * 0.52f;
  return (ccord_rect){g->content.x, g->content.y + 114.0f, w, UIS_H_INPUT};
}

ccord_rect st_account_password_box_rect(const sgeom *g)
{
  ccord_rect em = st_account_email_box_rect(g);
  float x = em.x + em.w + 8.0f;
  float w = g->content.x + g->content.w - x;
  return (ccord_rect){x, g->content.y + 114.0f, w, UIS_H_INPUT};
}

ccord_rect st_account_login_btn_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x, g->content.y + 168.0f, 180.0f, UIS_H_BTN};
}

ccord_rect st_account_token_box_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x, g->content.y + 236.0f, g->content.w, UIS_H_INPUT};
}

ccord_rect st_account_token_paste_rect(const sgeom *g)
{
  float pw = g->content.w < 320.0f ? floorf((g->content.w - 12.0f) * 0.22f) : 68.0f;
  return (ccord_rect){g->content.x, g->content.y + 284.0f, pw, UIS_H_BTN};
}

ccord_rect st_account_token_save_rect(const sgeom *g)
{
  ccord_rect p = st_account_token_paste_rect(g);
  float gap = g->content.w < 320.0f ? 6.0f : 8.0f;
  float sw = g->content.w < 320.0f ? floorf((g->content.w - 12.0f) * 0.46f) : 136.0f;
  return (ccord_rect){p.x + p.w + gap, g->content.y + 284.0f, sw, UIS_H_BTN};
}

ccord_rect st_account_token_clear_rect(const sgeom *g)
{
  ccord_rect s = st_account_token_save_rect(g);
  float gap = g->content.w < 320.0f ? 6.0f : 8.0f;
  float cw = g->content.w < 320.0f ? fmaxf(g->content.x + g->content.w - (s.x + s.w + gap), 60.0f) : 96.0f;
  return (ccord_rect){s.x + s.w + gap, g->content.y + 284.0f, cw, UIS_H_BTN};
}

// QuickCSS snippet editor (Appearance, row 6).
ccord_rect st_quickcss_box_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x, st_row_y(g, 6) + 18.0f, g->content.w, UIS_H_INPUT};
}

// Multi-account switcher (My Account): roster rows between the token buttons and the web links.
ccord_rect st_account_add_btn_rect(const sgeom *g)
{
  return (ccord_rect){g->content.x + g->content.w - 136.0f, g->content.y + 330.0f, 136.0f, UIS_H_BTN};
}

ccord_rect st_saved_account_row_rect(const sgeom *g, int i)
{
  return (ccord_rect){g->content.x, g->content.y + 372.0f + (float)i * 40.0f, g->content.w, UIS_H_BTN};
}

ccord_rect st_saved_account_switch_rect(const sgeom *g, int i)
{
  ccord_rect row = st_saved_account_row_rect(g, i);
  return (ccord_rect){row.x + row.w - 156.0f, row.y, 72.0f, UIS_H_BTN};
}

ccord_rect st_saved_account_remove_rect(const sgeom *g, int i)
{
  ccord_rect row = st_saved_account_row_rect(g, i);
  return (ccord_rect){row.x + row.w - 80.0f, row.y, 72.0f, UIS_H_BTN};
}

bool ui_is_login_screen(const ccord_ui *ui)
{
  if (!ui || !ui->settings_open) return false;
  if (ui->login_screen) return true;
  return ccord_store_self_id(ui->d.store) == CCORD_ID_NONE && !ui->account_view.signed_in;
}

ccord_rect st_login_card_rect(const ccord_ui *ui)
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

ccord_rect st_login_tab_rect(const ccord_rect *card, int tab)
{
  float sc = st_login_scroll_offset(nullptr, card);
  float cx = card->x + 32.0f;
  float cw = card->w - 64.0f;
  float tw = (cw - 8.0f) * 0.5f;
  return (ccord_rect){cx + (float)tab * (tw + 8.0f), card->y + 96.0f - sc, tw, UIS_H_BTN};
}

ccord_rect st_login_token_box_rect(const ccord_rect *card)
{
  float sc = st_login_scroll_offset(nullptr, card);
  return (ccord_rect){card->x + 32.0f, card->y + 198.0f - sc, card->w - 64.0f, UIS_H_INPUT};
}

ccord_rect st_login_token_paste_rect(const ccord_rect *card)
{
  float sc = st_login_scroll_offset(nullptr, card);
  return (ccord_rect){card->x + 32.0f, card->y + 246.0f - sc, card->w - 64.0f, UIS_H_BTN};
}

ccord_rect st_login_token_save_rect(const ccord_rect *card)
{
  float sc = st_login_scroll_offset(nullptr, card);
  return (ccord_rect){card->x + 32.0f, card->y + 290.0f - sc, card->w - 64.0f, UIS_H_BTN_LG};
}

ccord_rect st_login_email_box_rect(const ccord_rect *card)
{
  float sc = st_login_scroll_offset(nullptr, card);
  return (ccord_rect){card->x + 32.0f, card->y + 196.0f - sc, card->w - 64.0f, UIS_H_INPUT};
}

ccord_rect st_login_password_box_rect(const ccord_rect *card)
{
  float sc = st_login_scroll_offset(nullptr, card);
  return (ccord_rect){card->x + 32.0f, card->y + 262.0f - sc, card->w - 64.0f, UIS_H_INPUT};
}

ccord_rect st_login_credentials_btn_rect(const ccord_rect *card)
{
  float sc = st_login_scroll_offset(nullptr, card);
  return (ccord_rect){card->x + 32.0f, card->y + 320.0f - sc, card->w - 64.0f, UIS_H_BTN_LG};
}

// Secondary 2FA / TOTP popup modal (only shown after Discord requests 2FA for the account).
ccord_rect st_mfa_popup_rect(const ccord_ui *ui)
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

float st_login_scroll_offset(const ccord_ui *ui, const ccord_rect *card)
{
  (void)ui;
  float need = 404.0f;
  float max_s = need > card->h ? (need - card->h) : 0.0f;
  if (st_s_login_scroll < 0.0f) st_s_login_scroll = 0.0f;
  if (st_s_login_scroll > max_s) st_s_login_scroll = max_s;
  return st_s_login_scroll;
}

float st_mfa_scroll_offset(const ccord_ui *ui, const ccord_rect *pop)
{
  (void)ui;
  float need = 290.0f;
  float max_s = need > pop->h ? (need - pop->h) : 0.0f;
  if (st_s_login_scroll < 0.0f) st_s_login_scroll = 0.0f;
  if (st_s_login_scroll > max_s) st_s_login_scroll = max_s;
  return st_s_login_scroll;
}

ccord_rect st_mfa_popup_input_rect(const ccord_rect *pop)
{
  float sc = st_mfa_scroll_offset(nullptr, pop);
  return (ccord_rect){pop->x + 28.0f, pop->y + 148.0f - sc, pop->w - 56.0f, UIS_H_INPUT};
}

ccord_rect st_mfa_popup_verify_rect(const ccord_rect *pop)
{
  float sc = st_mfa_scroll_offset(nullptr, pop);
  return (ccord_rect){pop->x + 28.0f, pop->y + 196.0f - sc, pop->w - 56.0f, UIS_H_BTN_LG};
}

ccord_rect st_mfa_popup_back_rect(const ccord_rect *pop)
{
  float sc = st_mfa_scroll_offset(nullptr, pop);
  return (ccord_rect){pop->x + 28.0f, pop->y + 244.0f - sc, pop->w - 56.0f, UIS_H_BTN};
}

void ui_settings_auth_autofill_rects(const ccord_ui *ui, ccord_rect *email_r, ccord_rect *pw_r, ccord_rect *otp_r)
{
  if (email_r) *email_r = (ccord_rect){0};
  if (pw_r) *pw_r = (ccord_rect){0};
  if (otp_r) *otp_r = (ccord_rect){0};
  if (!ui) return;
  if (ui->account_mfa_required) {
    ccord_rect pop = st_mfa_popup_rect(ui);
    if (otp_r) *otp_r = st_mfa_popup_input_rect(&pop);
    return;
  }
  if (ui_is_login_screen(ui)) {
    ccord_rect card = st_login_card_rect(ui);
    if (email_r) *email_r = st_login_email_box_rect(&card);
    if (pw_r) *pw_r = st_login_password_box_rect(&card);
  } else if (ui->settings_open && ui->settings_page == SP_ACCOUNT && !ui->account_view.signed_in) {
    sgeom g = st_geometry(ui);
    if (email_r) *email_r = st_account_email_box_rect(&g);
    if (pw_r) *pw_r = st_account_password_box_rect(&g);
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
  sgeom g = st_geometry(ui);
  if (plugins_tab) return g.page[SP_PLUGINS];
  return st_extensions_toggle_rect(&g);
}

// The one place the Extensions switch changes state: UI flag first, then the app's callback (start or stop hosts).
void st_set_extensions(ccord_ui *ui, bool on)
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

void st_settings_init_defaults(ccord_ui *ui)
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

// ---- hit-testing ----

int st_clamp_volume(const ccord_rect *track, float x)
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
// through a content rect shifted by -st_s_voice_scroll; the scrollbar itself stays fixed.
#define VOICE_SCROLL_STEP 108.0f

static float voice_content_height(void)
{
  return 44.0f + (float)VOICE_ROWS * 54.0f + VOICE_DIAG_H + 8.0f;
}

float st_voice_max_scroll(const sgeom *g)
{
  float m = voice_content_height() - g->content.h;
  return m > 0.0f ? m : 0.0f;
}

ccord_rect st_voice_scroll_track(const sgeom *g)
{
  float tx = g->content.x + g->content.w + 12.0f;
  float max_tx = g->panel.x + g->panel.w - 12.0f;
  if (tx > max_tx) tx = max_tx;
  return (ccord_rect){tx, g->content.y + 44.0f, 8.0f, g->content.h - 44.0f};
}

float st_page_max_scroll(const ccord_ui *ui, const sgeom *g)
{
  if (!ui || !g) return 0.0f;
  float total = 0.0f;
  switch (ui->settings_page) {
    case SP_ACCOUNT: {
      float web_y = st_account_web_y(ui, g);
      float extra_web = g->content.w < 380.0f ? (UIS_H_BTN + 6.0f) : 0.0f;
      total = (web_y - g->content.y) + UIS_H_BTN + extra_web + 52.0f;
      break;
    }
    case SP_APPEARANCE:
      total = 44.0f + 8.0f * 54.0f + 16.0f;
      break;
    case SP_STREAMING:
      total = (st_stream_row_y(g, 5) - g->content.y) + 40.0f;
      break;
    case SP_KEYBINDS: {
      size_t npk = ui->d.contrib ? ccord_contrib_keybind_count(ui->d.contrib) : 0;
      if (npk > 8u) npk = 8u;
      total = 44.0f + (float)(6u + npk) * 54.0f + 16.0f;
      break;
    }
    case SP_PLUGINS: {
      float body_top = st_plugin_body_y(g);
      float view_h = fmaxf((g->content.y + g->content.h) - body_top, 40.0f);
      float m = st_s_plugins_content_h - view_h;
      return m > 0.0f ? m : 0.0f;
    }
    default:
      return 0.0f;
  }
  float m = total - g->content.h;
  return m > 0.0f ? m : 0.0f;
}

ccord_rect st_voice_scroll_up_rect(const sgeom *g)
{
  ccord_rect t = st_voice_scroll_track(g);
  return (ccord_rect){t.x - 4.0f, t.y - 24.0f, 16.0f, 20.0f};
}

ccord_rect st_voice_scroll_down_rect(const sgeom *g)
{
  ccord_rect t = st_voice_scroll_track(g);
  return (ccord_rect){t.x - 4.0f, t.y + t.h + 4.0f, 16.0f, 20.0f};
}

float st_voice_scroll_thumb_h(const sgeom *g)
{
  ccord_rect t = st_voice_scroll_track(g);
  float h = t.h * (g->content.h / voice_content_height());
  return h < 24.0f ? 24.0f : h;
}

// g is the unshifted st_geometry. Returns true when the click landed on a scroll control.
bool st_voice_scroll_click(ccord_ui *ui, const sgeom *g, float x, float y)
{
  float max = st_voice_max_scroll(g);
  if (max <= 0.0f) return false;
  ccord_rect up = st_voice_scroll_up_rect(g), down = st_voice_scroll_down_rect(g), tr = st_voice_scroll_track(g);
  ccord_scroller_cancel(&st_s_voice_glide);   // a scrollbar press replaces any wheel glide
  if (ui_in_rect(x, y, up)) {
    st_s_voice_scroll -= VOICE_SCROLL_STEP;
  } else if (ui_in_rect(x, y, down)) {
    st_s_voice_scroll += VOICE_SCROLL_STEP;
  } else if (ui_in_rect(x, y, tr)) {
    float th = st_voice_scroll_thumb_h(g);
    float ty = tr.y + (tr.h - th) * (st_s_voice_scroll / max);
    if (y < ty) st_s_voice_scroll -= VOICE_SCROLL_STEP;
    else st_s_voice_scroll += VOICE_SCROLL_STEP;
  } else {
    return false;
  }
  if (st_s_voice_scroll < 0.0f) st_s_voice_scroll = 0.0f;
  if (st_s_voice_scroll > max) st_s_voice_scroll = max;
  ui->dirty = true;
  return true;
}

// Wheel over the settings panel: the Voice & Video page and the M6 list pages ease one notch per
// event (108 px, the scrollbar step). Returns true when the page moved.
bool ui_settings_handle_wheel(ccord_ui *ui, const ccord_input_event *ev)
{
  if (ui_is_login_screen(ui) || (ui->settings_page == SP_ACCOUNT && ui->account_mfa_required)) {
    ccord_rect box = ui->account_mfa_required ? st_mfa_popup_rect(ui) : st_login_card_rect(ui);
    float need = ui->account_mfa_required ? 290.0f : 404.0f;
    float max_s = need > box.h ? (need - box.h) : 0.0f;
    if (max_s <= 0.0f) return false;
    st_s_login_scroll -= ev->wheel * 48.0f;
    if (st_s_login_scroll < 0.0f) st_s_login_scroll = 0.0f;
    if (st_s_login_scroll > max_s) st_s_login_scroll = max_s;
    ui->dirty = true;
    return true;
  }
  sgeom g = st_geometry(ui);
  if (!ui_in_rect(ev->x, ev->y, g.panel)) return false;
  if (g.list.w > 0.0f && ui_in_rect(ev->x, ev->y, g.list)) {
    st_s_sidebar_scroll -= ev->wheel * g.step;
    if (st_s_sidebar_scroll < 0.0f) st_s_sidebar_scroll = 0.0f;
    (void)st_geometry(ui);   // clamps st_s_sidebar_scroll to max_scroll
    ui->dirty = true;
    return true;
  }
  if (ui->settings_page == SP_VOICE) {
    float max = st_voice_max_scroll(&g);
    if (max <= 0.0f) return false;
    ccord_scroller_scroll(&st_s_voice_glide, &st_s_voice_scroll, -ev->wheel * VOICE_SCROLL_STEP, max, ui_wheel_direct(ui, ev));
    ui->dirty = true;
    return true;
  }
  if (ui->settings_page == SP_PROFILE_EDIT) return ui_settings_profile_wheel(ui, ev->wheel);
  if (ui->settings_page == SP_NOTIFICATIONS && ui->settings_sounds_open) {
    return ui_settings_m6_prefs_wheel(ui, CCORD_M6_PREFS_NOTIFICATIONS, g.content, ev->wheel);
  }
  if (ui->settings_page == SP_PRIVACY || ui->settings_page == SP_DEVICES || ui->settings_page == SP_CONNECTIONS) {
    return ui_settings_m6_wheel(ui, ui->settings_page - SP_PRIVACY, g.content, ev->wheel, ui_wheel_direct(ui, ev));
  }
  {
    float max_p = st_page_max_scroll(ui, &g);
    if (max_p > 0.0f) {
      st_s_page_scroll -= ev->wheel * 56.0f;
      if (st_s_page_scroll < 0.0f) st_s_page_scroll = 0.0f;
      if (st_s_page_scroll > max_p) st_s_page_scroll = max_p;
      ui->dirty = true;
      return true;
    }
  }
  return false;
}

// Ticks the Voice & Video glide and the M6 list-page glides by fdt_ms (ccord_ui_update). Glides stop with the modal.
bool ui_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  bool moving = ui_settings_m6_scroll_tick(ui, fdt_ms, reduced);
  if (ui_settings_profile_tick(ui)) moving = true;   // Discord's answer to Save, and the brief success state
  if (st_s_save_pending && st_s_save_ui == ui) {
    if (!ui->settings_open || ui->now_ms >= st_s_save_due_ms) {
      st_settings_save(ui);
    } else {
      moving = true;
    }
  }
  if (!ui->settings_open) {
    st_s_hover_key = -1;
    st_s_last_drawn_page = -1;
  }
  if (st_s_voice_glide.pending == 0.0f) return moving;
  if (!ui->settings_open || ui->settings_page != SP_VOICE) {
    ccord_scroller_cancel(&st_s_voice_glide);
    return moving;
  }
  sgeom g = st_geometry(ui);
  moving = ccord_scroller_tick(&st_s_voice_glide, &st_s_voice_scroll, fdt_ms, st_voice_max_scroll(&g), reduced) || moving;
  ui->dirty = true;
  return moving;
}

// Voice preferences are local (never Discord state): they are stored first, then the effective
// config is forwarded to the session so the media plane picks it up. DSP toggles only take effect
// when the speexdsp backend is built in; without it the chain is a passthrough, so they are masked.
void st_voice_config_apply(ccord_ui *ui)
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

