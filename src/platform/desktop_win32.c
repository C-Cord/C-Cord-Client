// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Native Win32 desktop services: Shell_NotifyIconW system tray with badge + context menu,
// balloon notifications with click-to-open target tracking, and RegisterHotKey / GetAsyncKeyState
// global shortcuts. Offline mode (headless/tests) skips Shell32/User32 registration cleanly.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "desktop.h"

#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/platform/tray_icon.h>

#define DESKTOP_APP_ID_MAX 64u
#define DESKTOP_TITLE_MAX 128u
#define DESKTOP_BODY_MAX 256u
#define DESKTOP_SHORTCUT_MAX 16u
#define DESKTOP_NOTIFY_MAP_MAX 32u
#define NOTIFY_ACTION_KEY "default"

#define WM_CCORD_TRAY (WM_APP + 1)
#define CMD_TRAY_SHOW 1001
#define CMD_TRAY_MUTE 1002
#define CMD_TRAY_DEAFEN 1003
#define CMD_TRAY_QUIT 1004
#define TRAY_ICON_UID 1u

static const int k_tray_sizes[CCORD_DESKTOP_TRAY_PIXMAPS] = {22, 24, 32, 48};
#define TRAY_PIXMAP_PIXELS (22 * 22 + 24 * 24 + 32 * 32 + 48 * 48)
#define TRAY_PIXMAP_BYTES (TRAY_PIXMAP_PIXELS * 4)

typedef struct desktop_notify_entry {
  uint32_t id;
  uint64_t seq;
  ccord_notify_target target;
} desktop_notify_entry;

typedef struct win_shortcut {
  char id[DESKTOP_TITLE_MAX];
  int hk_id;
  UINT mods;
  UINT vk;
  bool registered;
  bool was_down;
} win_shortcut;

struct ccord_desktop {
  bool offline;
  char app_id[DESKTOP_APP_ID_MAX];
  char last_title[DESKTOP_TITLE_MAX];
  char last_body[DESKTOP_BODY_MAX];
  uint32_t unread, mentions;
  uint32_t badge_count;
  int icon_key;
  uint32_t new_icon_emits;
  uint8_t pixmaps[TRAY_PIXMAP_BYTES];
  ccord_desktop_hotkey_fn hotkey_fn;
  void *hotkey_ud;
  ccord_desktop_shortcut_state_fn state_fn;
  void *state_ud;
  ccord_desktop_tray_fn tray_fn;
  void *tray_ud;
  ccord_desktop_notify_action_fn notify_fn;
  void *notify_ud;
  desktop_notify_entry notify_map[DESKTOP_NOTIFY_MAP_MAX];
  uint64_t notify_seq;
  uint32_t last_notify_id;
  uint32_t local_notify_id;
  char last_actions[16];
  bool muted, deafened;

  // Win32 native handles
  HWND hwnd;
  HICON hicon;
  bool tray_added;
  uint32_t active_balloon_id;
  win_shortcut shortcuts[DESKTOP_SHORTCUT_MAX];
  size_t shortcut_count;
};

static size_t pixmap_offset(size_t index)
{
  size_t off = 0;
  for (size_t i = 0; i < index; i++) off += (size_t)k_tray_sizes[i] * (size_t)k_tray_sizes[i] * 4u;
  return off;
}

static int icon_key_for(uint32_t count, bool has_unread)
{
  if (count > 0) return 1 + (int)(count > 100u ? 100u : count);
  return has_unread ? 1 : 0;
}

static void render_pixmaps(ccord_desktop *d, uint32_t count, bool has_unread)
{
  uint8_t rgba[48 * 48 * 4];
  int badge = (int)(count > 100u ? 100u : count);
  for (size_t i = 0; i < CCORD_DESKTOP_TRAY_PIXMAPS; i++) {
    int sz = k_tray_sizes[i];
    size_t nbytes = (size_t)sz * (size_t)sz * 4u;
    if (!ccord_tray_icon_rgba(sz, badge, has_unread, rgba, nbytes)) {
      memset(d->pixmaps + pixmap_offset(i), 0, nbytes);
      continue;
    }
    uint8_t *dst = d->pixmaps + pixmap_offset(i);
    for (size_t p = 0; p < (size_t)sz * (size_t)sz; p++) {
      dst[p * 4 + 0] = rgba[p * 4 + 3];
      dst[p * 4 + 1] = rgba[p * 4 + 0];
      dst[p * 4 + 2] = rgba[p * 4 + 1];
      dst[p * 4 + 3] = rgba[p * 4 + 2];
    }
  }
}

static HICON create_hicon_from_rgba(int size, const uint8_t *rgba)
{
  BITMAPV5HEADER bi;
  memset(&bi, 0, sizeof bi);
  bi.bV5Size = sizeof bi;
  bi.bV5Width = size;
  bi.bV5Height = -size;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000u;
  bi.bV5GreenMask = 0x0000FF00u;
  bi.bV5BlueMask = 0x000000FFu;
  bi.bV5AlphaMask = 0xFF000000u;

  void *bits = nullptr;
  HDC hdc = GetDC(nullptr);
  HBITMAP color_bmp = CreateDIBSection(hdc, (const BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, hdc);
  if (!color_bmp || !bits) {
    if (color_bmp) DeleteObject(color_bmp);
    return nullptr;
  }
  uint8_t *dst = (uint8_t *)bits;
  size_t count = (size_t)size * (size_t)size;
  for (size_t i = 0; i < count; i++) {
    dst[i * 4 + 0] = rgba[i * 4 + 2];   // B
    dst[i * 4 + 1] = rgba[i * 4 + 1];   // G
    dst[i * 4 + 2] = rgba[i * 4 + 0];   // R
    dst[i * 4 + 3] = rgba[i * 4 + 3];   // A
  }
  HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO ii = {.fIcon = TRUE, .xHotspot = 0, .yHotspot = 0, .hbmMask = mask_bmp, .hbmColor = color_bmp};
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color_bmp);
  if (mask_bmp) DeleteObject(mask_bmp);
  return icon;
}

static void update_win32_tray_icon(ccord_desktop *d)
{
  if (d->offline || !d->hwnd) return;
  uint8_t rgba[32 * 32 * 4];
  int badge = (int)(d->badge_count > 100u ? 100u : d->badge_count);
  bool has_unread = d->unread > 0 || d->mentions > 0 || d->badge_count > 0;
  HICON next_icon = nullptr;
  if (ccord_tray_icon_rgba(32, badge, has_unread, rgba, sizeof rgba)) {
    next_icon = create_hicon_from_rgba(32, rgba);
  }
  NOTIFYICONDATAW nid;
  memset(&nid, 0, sizeof nid);
  nid.cbSize = sizeof nid;
  nid.hWnd = d->hwnd;
  nid.uID = TRAY_ICON_UID;
  nid.uFlags = NIF_MESSAGE | NIF_TIP | (next_icon ? NIF_ICON : 0u);
  nid.uCallbackMessage = WM_CCORD_TRAY;
  nid.hIcon = next_icon ? next_icon : d->hicon;
  if (d->badge_count > 0) {
    swprintf(nid.szTip, sizeof nid.szTip / sizeof nid.szTip[0], L"C-Cord (%u unread)", (unsigned)d->badge_count);
  } else {
    wcscpy(nid.szTip, L"C-Cord");
  }
  if (!d->tray_added) {
    if (Shell_NotifyIconW(NIM_ADD, &nid)) d->tray_added = true;
  } else {
    (void)Shell_NotifyIconW(NIM_MODIFY, &nid);
  }
  if (next_icon) {
    if (d->hicon) DestroyIcon(d->hicon);
    d->hicon = next_icon;
  }
}

static void notify_map_put(ccord_desktop *d, uint32_t id, const ccord_notify_target *t)
{
  if (!id || !t) return;
  size_t slot = DESKTOP_NOTIFY_MAP_MAX;
  uint64_t oldest_seq = UINT64_MAX;
  size_t oldest_idx = 0;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) { slot = i; break; }
    if (d->notify_map[i].id == 0 && slot == DESKTOP_NOTIFY_MAP_MAX) slot = i;
    if (d->notify_map[i].id != 0 && d->notify_map[i].seq < oldest_seq) {
      oldest_seq = d->notify_map[i].seq;
      oldest_idx = i;
    }
  }
  if (slot == DESKTOP_NOTIFY_MAP_MAX) slot = oldest_idx;
  d->notify_map[slot] = (desktop_notify_entry){.id = id, .seq = ++d->notify_seq, .target = *t};
}

static void show_tray_menu(ccord_desktop *d)
{
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING, CMD_TRAY_SHOW, L"Show / Hide C-Cord");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (d->muted ? MF_CHECKED : 0u), CMD_TRAY_MUTE, L"Mute");
  AppendMenuW(menu, MF_STRING | (d->deafened ? MF_CHECKED : 0u), CMD_TRAY_DEAFEN, L"Deafen");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, CMD_TRAY_QUIT, L"Quit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(d->hwnd);
  UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, d->hwnd, nullptr);
  DestroyMenu(menu);
  if (!d->tray_fn) return;
  switch (cmd) {
    case CMD_TRAY_SHOW: d->tray_fn(d->tray_ud, CCORD_TRAY_ACTIVATE); break;
    case CMD_TRAY_MUTE: d->tray_fn(d->tray_ud, CCORD_TRAY_TOGGLE_MUTE); break;
    case CMD_TRAY_DEAFEN: d->tray_fn(d->tray_ud, CCORD_TRAY_TOGGLE_DEAFEN); break;
    case CMD_TRAY_QUIT: d->tray_fn(d->tray_ud, CCORD_TRAY_QUIT); break;
    default: break;
  }
}

static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  ccord_desktop *d = (ccord_desktop *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (d && msg == WM_CCORD_TRAY) {
    switch (lp) {
      case WM_LBUTTONUP:
      case WM_LBUTTONDBLCLK:
        if (d->tray_fn) d->tray_fn(d->tray_ud, CCORD_TRAY_ACTIVATE);
        return 0;
      case WM_RBUTTONUP:
      case WM_CONTEXTMENU:
        show_tray_menu(d);
        return 0;
      case NIN_BALLOONUSERCLICK:
        if (d->active_balloon_id != 0) {
          ccord_desktop_inject_notify_action(d, d->active_balloon_id, NOTIFY_ACTION_KEY);
          d->active_balloon_id = 0;
        }
        return 0;
      case NIN_BALLOONTIMEOUT:
      case NIN_BALLOONHIDE:
        d->active_balloon_id = 0;
        return 0;
      default:
        break;
    }
  } else if (d && msg == WM_HOTKEY) {
    int id = (int)wp;
    for (size_t i = 0; i < d->shortcut_count; i++) {
      if (d->shortcuts[i].hk_id == id) {
        if (d->hotkey_fn) d->hotkey_fn(d->hotkey_ud, d->shortcuts[i].id);
        break;
      }
    }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool parse_trigger(const char *trigger, UINT *out_mods, UINT *out_vk)
{
  if (!trigger || !trigger[0]) return false;
  UINT mods = 0;
  UINT vk = 0;
  const char *p = trigger;
  while (*p) {
    const char *sep = strchr(p, '+');
    size_t len = sep ? (size_t)(sep - p) : strlen(p);
    if (len == 4 && _strnicmp(p, "CTRL", 4) == 0) mods |= MOD_CONTROL;
    else if (len == 7 && _strnicmp(p, "CONTROL", 7) == 0) mods |= MOD_CONTROL;
    else if (len == 3 && _strnicmp(p, "ALT", 3) == 0) mods |= MOD_ALT;
    else if (len == 5 && _strnicmp(p, "SHIFT", 5) == 0) mods |= MOD_SHIFT;
    else if ((len == 5 && _strnicmp(p, "SUPER", 5) == 0) || (len == 4 && _strnicmp(p, "LOGO", 4) == 0) ||
             (len == 3 && _strnicmp(p, "WIN", 3) == 0)) mods |= MOD_WIN;
    else if (len == 1) {
      unsigned char c = (unsigned char)p[0];
      if (c >= 'a' && c <= 'z') vk = (UINT)(c - 'a' + 'A');
      else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) vk = (UINT)c;
    } else if (len >= 2 && (p[0] == 'F' || p[0] == 'f')) {
      int fn = atoi(p + 1);
      if (fn >= 1 && fn <= 24) vk = (UINT)(VK_F1 + fn - 1);
    } else if (len == 5 && _strnicmp(p, "SPACE", 5) == 0) {
      vk = VK_SPACE;
    }
    if (!sep) break;
    p = sep + 1;
  }
  if (vk == 0) return false;
  *out_mods = mods;
  *out_vk = vk;
  return true;
}

static void unregister_all_hotkeys(ccord_desktop *d)
{
  if (!d->hwnd) return;
  for (size_t i = 0; i < d->shortcut_count; i++) {
    if (d->shortcuts[i].registered) {
      UnregisterHotKey(d->hwnd, d->shortcuts[i].hk_id);
      d->shortcuts[i].registered = false;
    }
  }
}

ccord_desktop *ccord_desktop_create(const char *app_id, bool offline)
{
  ccord_desktop *d = ccord_calloc(1, sizeof *d);
  if (!d) return nullptr;
  d->offline = offline;
  snprintf(d->app_id, sizeof d->app_id, "%s", (app_id && app_id[0]) ? app_id : "ccord");
  render_pixmaps(d, 0, false);
  if (!offline) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = tray_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CCordTrayWindowClass";
    (void)RegisterClassW(&wc);
    d->hwnd = CreateWindowExW(0, wc.lpszClassName, L"C-Cord", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (d->hwnd) {
      SetWindowLongPtrW(d->hwnd, GWLP_USERDATA, (LONG_PTR)d);
      update_win32_tray_icon(d);
    }
  }
  return d;
}

void ccord_desktop_destroy(ccord_desktop *d)
{
  if (!d) return;
  unregister_all_hotkeys(d);
  if (d->tray_added && d->hwnd) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = d->hwnd;
    nid.uID = TRAY_ICON_UID;
    (void)Shell_NotifyIconW(NIM_DELETE, &nid);
  }
  if (d->hicon) DestroyIcon(d->hicon);
  if (d->hwnd) DestroyWindow(d->hwnd);
  ccord_free(d);
}

void ccord_desktop_pump(ccord_desktop *d)
{
  if (!d || d->offline || !d->hwnd) return;
  MSG msg;
  while (PeekMessageW(&msg, d->hwnd, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (d->state_fn) {
    for (size_t i = 0; i < d->shortcut_count; i++) {
      if (d->shortcuts[i].vk == 0) continue;
      bool down = (GetAsyncKeyState((int)d->shortcuts[i].vk) & 0x8000) != 0;
      if (down && (d->shortcuts[i].mods & MOD_CONTROL) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) down = false;
      if (down && (d->shortcuts[i].mods & MOD_ALT) && !(GetAsyncKeyState(VK_MENU) & 0x8000)) down = false;
      if (down && (d->shortcuts[i].mods & MOD_SHIFT) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) down = false;
      if (down != d->shortcuts[i].was_down) {
        d->shortcuts[i].was_down = down;
        d->state_fn(d->state_ud, d->shortcuts[i].id, down);
      }
    }
  }
}

bool ccord_desktop_notifications_available(const ccord_desktop *d)
{
  return d && !d->offline && d->tray_added;
}

bool ccord_desktop_notify_target(ccord_desktop *d, const char *title, const char *body,
                                 const ccord_notify_target *target)
{
  if (!d) return false;
  const char *t = title ? title : "";
  const char *b = body ? body : "";
  if (!ccord_utf8_valid(t, strlen(t)) || !ccord_utf8_valid(b, strlen(b))) return false;
  snprintf(d->last_title, sizeof d->last_title, "%s", t);
  snprintf(d->last_body, sizeof d->last_body, "%s", b);
  uint32_t id = ++d->local_notify_id;
  if (id == 0) id = ++d->local_notify_id;
  d->last_notify_id = id;
  if (target) {
    snprintf(d->last_actions, sizeof d->last_actions, "%s", NOTIFY_ACTION_KEY);
    notify_map_put(d, id, target);
  } else {
    d->last_actions[0] = '\0';
  }
  if (!d->offline && d->tray_added && d->hwnd) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = d->hwnd;
    nid.uID = TRAY_ICON_UID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    (void)MultiByteToWideChar(CP_UTF8, 0, t, -1, nid.szInfoTitle, (int)(sizeof nid.szInfoTitle / sizeof nid.szInfoTitle[0]));
    (void)MultiByteToWideChar(CP_UTF8, 0, b, -1, nid.szInfo, (int)(sizeof nid.szInfo / sizeof nid.szInfo[0]));
    if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
      d->active_balloon_id = target ? id : 0;
    }
  }
  return true;
}

bool ccord_desktop_notify(ccord_desktop *d, const char *title, const char *body)
{
  return ccord_desktop_notify_target(d, title, body, nullptr);
}

const char *ccord_desktop_last_notify_title(const ccord_desktop *d) { return d ? d->last_title : ""; }
const char *ccord_desktop_last_notify_body(const ccord_desktop *d) { return d ? d->last_body : ""; }
uint32_t ccord_desktop_last_notify_id(const ccord_desktop *d) { return d ? d->last_notify_id : 0; }
const char *ccord_desktop_last_notify_actions(const ccord_desktop *d) { return d ? d->last_actions : ""; }

void ccord_desktop_set_notify_action_callback(ccord_desktop *d, ccord_desktop_notify_action_fn fn, void *ud)
{
  if (!d) return;
  d->notify_fn = fn;
  d->notify_ud = ud;
}

bool ccord_desktop_notify_target_for(const ccord_desktop *d, uint32_t id, ccord_notify_target *out)
{
  if (!d || !id) return false;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) {
      if (out) *out = d->notify_map[i].target;
      return true;
    }
  }
  return false;
}

size_t ccord_desktop_notify_map_count(const ccord_desktop *d)
{
  if (!d) return 0;
  size_t n = 0;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) if (d->notify_map[i].id != 0) n++;
  return n;
}

void ccord_desktop_inject_notify_action(ccord_desktop *d, uint32_t id, const char *action_key)
{
  if (!d || !id || !action_key || strcmp(action_key, NOTIFY_ACTION_KEY) != 0) return;
  ccord_notify_target t;
  if (ccord_desktop_notify_target_for(d, id, &t) && d->notify_fn) {
    d->notify_fn(d->notify_ud, &t);
  }
}

void ccord_desktop_inject_notify_closed(ccord_desktop *d, uint32_t id)
{
  if (!d || !id) return;
  for (size_t i = 0; i < DESKTOP_NOTIFY_MAP_MAX; i++) {
    if (d->notify_map[i].id == id) {
      d->notify_map[i].id = 0;
      break;
    }
  }
}

bool ccord_desktop_tray_available(const ccord_desktop *d) { return d && !d->offline && d->tray_added; }

void ccord_desktop_set_badge(ccord_desktop *d, uint32_t unread, uint32_t mentions, uint32_t count)
{
  if (!d) return;
  d->unread = unread;
  d->mentions = mentions;
  d->badge_count = count;
  bool has_unread = unread > 0 || mentions > 0 || count > 0;
  int key = icon_key_for(count, has_unread);
  if (key != d->icon_key) {
    d->icon_key = key;
    d->new_icon_emits++;
    render_pixmaps(d, count, has_unread);
    update_win32_tray_icon(d);
  }
}

bool ccord_desktop_tray_pixmap(const ccord_desktop *d, size_t index, int *w, int *h, const uint8_t **argb)
{
  if (!d || index >= CCORD_DESKTOP_TRAY_PIXMAPS) return false;
  if (w) *w = k_tray_sizes[index];
  if (h) *h = k_tray_sizes[index];
  if (argb) *argb = d->pixmaps + pixmap_offset(index);
  return true;
}

uint32_t ccord_desktop_new_icon_count(const ccord_desktop *d) { return d ? d->new_icon_emits : 0; }

void ccord_desktop_set_tray_callback(ccord_desktop *d, ccord_desktop_tray_fn fn, void *ud)
{
  if (!d) return;
  d->tray_fn = fn;
  d->tray_ud = ud;
}

void ccord_desktop_set_voice_state(ccord_desktop *d, bool muted, bool deafened)
{
  if (!d) return;
  d->muted = muted;
  d->deafened = deafened;
}

bool ccord_desktop_global_shortcuts_available(const ccord_desktop *d)
{
  return d && !d->offline && d->hwnd != nullptr;
}

bool ccord_desktop_bind_shortcuts(ccord_desktop *d, const ccord_global_shortcut *defs, size_t n)
{
  if (!d || d->offline || !d->hwnd) return false;
  unregister_all_hotkeys(d);
  d->shortcut_count = 0;
  if (!defs || n == 0) return true;
  if (n > DESKTOP_SHORTCUT_MAX) n = DESKTOP_SHORTCUT_MAX;
  for (size_t i = 0; i < n; i++) {
    win_shortcut *s = &d->shortcuts[d->shortcut_count];
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", defs[i].id ? defs[i].id : "");
    s->hk_id = (int)(i + 1u);
    if (parse_trigger(defs[i].preferred_trigger, &s->mods, &s->vk)) {
      s->registered = RegisterHotKey(d->hwnd, s->hk_id, s->mods | MOD_NOREPEAT, s->vk) != 0;
    }
    d->shortcut_count++;
  }
  return true;
}

void ccord_desktop_set_hotkey_callback(ccord_desktop *d, ccord_desktop_hotkey_fn fn, void *ud)
{
  if (!d) return;
  d->hotkey_fn = fn;
  d->hotkey_ud = ud;
}

void ccord_desktop_set_shortcut_state_callback(ccord_desktop *d, ccord_desktop_shortcut_state_fn fn, void *ud)
{
  if (!d) return;
  d->state_fn = fn;
  d->state_ud = ud;
}
