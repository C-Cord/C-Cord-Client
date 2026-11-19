// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Native Windows screen/window capture (GDI / PrintWindow BGRA8) + WASAPI loopback desktop audio.
// Zero third-party dependencies beyond standard Windows SDK libraries (user32, gdi32, dwmapi, ole32, mmdevapi).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <dwmapi.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <psapi.h>

#include <ccord/platform/capture.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- screen & window capture

typedef struct win32_cap {
  ccord_capture base;
  HWND hwnd;            // nullptr for monitor capture
  int cap_x, cap_y;
  int w, h;
  HDC mem_dc;
  HBITMAP dib;
  HGDIOBJ old_bmp;
  uint8_t *bits;
  bool dead;
} win32_cap;

static bool wide_to_utf8_buf(const wchar_t *ws, char *out, size_t cap)
{
  if (!ws || !out || cap == 0) return false;
  int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, out, (int)cap, nullptr, nullptr);
  if (n <= 0) {
    out[0] = '\0';
    return false;
  }
  return true;
}

typedef struct mon_enum_ctx {
  ccord_capture_source *out;
  size_t max;
  size_t count;
} mon_enum_ctx;

static BOOL CALLBACK enum_monitors_cb(HMONITOR mon, HDC hdc, LPRECT rect, LPARAM lp)
{
  (void)hdc;
  (void)rect;
  mon_enum_ctx *ctx = (mon_enum_ctx *)lp;
  if (ctx->count >= ctx->max) return FALSE;
  MONITORINFOEXW mi;
  memset(&mi, 0, sizeof mi);
  mi.cbSize = sizeof mi;
  if (!GetMonitorInfoW(mon, (LPMONITORINFO)&mi)) return TRUE;
  ccord_capture_source *s = &ctx->out[ctx->count++];
  memset(s, 0, sizeof *s);
  s->kind = CCORD_CAPTURE_SCREEN;
  s->backend = CCORD_CAPBE_WIN32;
  s->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  s->x = (int)mi.rcMonitor.left;
  s->y = (int)mi.rcMonitor.top;
  s->width = (int)(mi.rcMonitor.right - mi.rcMonitor.left);
  s->height = (int)(mi.rcMonitor.bottom - mi.rcMonitor.top);
  s->native = (uint64_t)(uintptr_t)mon;
  char dev[64] = "";
  wide_to_utf8_buf(mi.szDevice, dev, sizeof dev);
  snprintf(s->id, sizeof s->id, "win32:mon:%zu", ctx->count);
  snprintf(s->title, sizeof s->title, "Screen %zu%s", ctx->count, s->primary ? " (primary)" : "");
  snprintf(s->app, sizeof s->app, "%s", dev);
  return TRUE;
}

typedef struct win_enum_ctx {
  ccord_capture_source *out;
  size_t max;
  size_t count;
  DWORD own_pid;
} win_enum_ctx;

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lp)
{
  win_enum_ctx *ctx = (win_enum_ctx *)lp;
  if (ctx->count >= ctx->max) return FALSE;
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (ex & WS_EX_TOOLWINDOW) return TRUE;
  BOOL cloaked = FALSE;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return TRUE;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (ctx->own_pid != 0 && pid == ctx->own_pid) return TRUE;

  wchar_t wtitle[128];
  int tlen = GetWindowTextW(hwnd, wtitle, (int)(sizeof wtitle / sizeof wtitle[0]));
  if (tlen <= 0) return TRUE;

  RECT rc;
  if (!GetWindowRect(hwnd, &rc)) return TRUE;
  int w = (int)(rc.right - rc.left);
  int h = (int)(rc.bottom - rc.top);
  if (w < 64 || h < 64) return TRUE;

  ccord_capture_source *s = &ctx->out[ctx->count++];
  memset(s, 0, sizeof *s);
  s->kind = CCORD_CAPTURE_WINDOW;
  s->backend = CCORD_CAPBE_WIN32;
  s->x = (int)rc.left;
  s->y = (int)rc.top;
  s->width = w;
  s->height = h;
  s->native = (uint64_t)(uintptr_t)hwnd;
  snprintf(s->id, sizeof s->id, "win32:win:%llx", (unsigned long long)(uintptr_t)hwnd);
  wide_to_utf8_buf(wtitle, s->title, sizeof s->title);

  HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (hp) {
    wchar_t wexe[MAX_PATH];
    DWORD elen = MAX_PATH;
    if (QueryFullProcessImageNameW(hp, 0, wexe, &elen) && elen > 0) {
      const wchar_t *base = wcsrchr(wexe, L'\\');
      wide_to_utf8_buf(base ? base + 1 : wexe, s->app, sizeof s->app);
    }
    CloseHandle(hp);
  }
  return TRUE;
}

static bool win32_available(void) { return true; }

static size_t win32_list(ccord_capture_kind kind, ccord_capture_source *out, size_t max, ccord_error *err)
{
  (void)err;
  if (kind == CCORD_CAPTURE_SCREEN) {
    mon_enum_ctx ctx = {.out = out, .max = max, .count = 0};
    EnumDisplayMonitors(nullptr, nullptr, enum_monitors_cb, (LPARAM)&ctx);
    // Keep primary monitor first.
    for (size_t i = 1; i < ctx.count; i++) {
      if (out[i].primary) {
        ccord_capture_source tmp = out[0];
        out[0] = out[i];
        out[i] = tmp;
        break;
      }
    }
    return ctx.count;
  }
  int own = ccord_capture_own_pid();
  win_enum_ctx ctx = {.out = out, .max = max, .count = 0, .own_pid = own > 0 ? (DWORD)own : GetCurrentProcessId()};
  EnumWindows(enum_windows_cb, (LPARAM)&ctx);
  return ctx.count;
}

static bool win32_ensure_dib(win32_cap *c, int w, int h)
{
  if (c->dib && c->w == w && c->h == h) return true;
  if (c->mem_dc && c->old_bmp) SelectObject(c->mem_dc, c->old_bmp);
  if (c->dib) { DeleteObject(c->dib); c->dib = nullptr; }
  c->bits = nullptr;
  if (!c->mem_dc) {
    HDC sdc = GetDC(nullptr);
    c->mem_dc = CreateCompatibleDC(sdc);
    ReleaseDC(nullptr, sdc);
    if (!c->mem_dc) return false;
  }
  BITMAPINFO bi;
  memset(&bi, 0, sizeof bi);
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down BGRA
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  c->dib = CreateDIBSection(c->mem_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!c->dib || !bits) {
    if (c->dib) { DeleteObject(c->dib); c->dib = nullptr; }
    return false;
  }
  c->old_bmp = SelectObject(c->mem_dc, c->dib);
  c->bits = (uint8_t *)bits;
  c->w = w;
  c->h = h;
  return true;
}

static bool win32_grab(ccord_capture *base, ccord_capture_frame *out)
{
  win32_cap *c = (win32_cap *)base;
  if (c->dead) return false;
  int w = c->w, h = c->h;
  if (c->hwnd) {
    if (!IsWindow(c->hwnd)) { c->dead = true; return false; }
    RECT rc;
    if (!GetWindowRect(c->hwnd, &rc)) return false;
    w = (int)(rc.right - rc.left);
    h = (int)(rc.bottom - rc.top);
    if (w < 2 || h < 2) return false;
    if (!win32_ensure_dib(c, w, h)) return false;
    if (!PrintWindow(c->hwnd, c->mem_dc, PW_RENDERFULLCONTENT)) {
      HDC wdc = GetWindowDC(c->hwnd);
      if (!wdc) return false;
      BitBlt(c->mem_dc, 0, 0, w, h, wdc, 0, 0, SRCCOPY | CAPTUREBLT);
      ReleaseDC(c->hwnd, wdc);
    }
  } else {
    if (w < 2 || h < 2) {
      w = GetSystemMetrics(SM_CXSCREEN);
      h = GetSystemMetrics(SM_CYSCREEN);
    }
    if (!win32_ensure_dib(c, w, h)) return false;
    HDC sdc = GetDC(nullptr);
    if (!sdc) return false;
    BitBlt(c->mem_dc, 0, 0, w, h, sdc, c->cap_x, c->cap_y, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, sdc);
  }
  *out = (ccord_capture_frame){.data = c->bits, .width = c->w, .height = c->h, .stride = c->w * 4};
  return true;
}

static bool win32_failed(const ccord_capture *base)
{
  const win32_cap *c = (const win32_cap *)base;
  return c->dead || (c->hwnd && !IsWindow(c->hwnd));
}

static void win32_close(ccord_capture *base)
{
  win32_cap *c = (win32_cap *)base;
  if (c->mem_dc && c->old_bmp) SelectObject(c->mem_dc, c->old_bmp);
  if (c->dib) DeleteObject(c->dib);
  if (c->mem_dc) DeleteDC(c->mem_dc);
  ccord_free(c);
}

static const ccord_capture_ops k_win32_ops = {.close = win32_close, .grab = win32_grab, .failed = win32_failed};

static ccord_capture *win32_open(const ccord_capture_source *src, ccord_error *err)
{
  win32_cap *c = ccord_calloc(1, sizeof *c);
  if (!c) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "capture: out of memory");
    return nullptr;
  }
  c->base.ops = &k_win32_ops;
  c->base.src = *src;
  if (src->kind == CCORD_CAPTURE_WINDOW) {
    c->hwnd = (HWND)(uintptr_t)src->native;
    if (!c->hwnd || !IsWindow(c->hwnd)) {
      ccord_free(c);
      ccord_error_set(err, CCORD_ERR_NOT_FOUND, "capture: window no longer exists");
      return nullptr;
    }
  } else {
    c->cap_x = src->x;
    c->cap_y = src->y;
    c->w = src->width > 0 ? src->width : GetSystemMetrics(SM_CXSCREEN);
    c->h = src->height > 0 ? src->height : GetSystemMetrics(SM_CYSCREEN);
  }
  return &c->base;
}

const ccord_capture_backend ccord_capture_win32_backend = {
  .name = "Windows Capture",
  .available = win32_available,
  .list = win32_list,
  .open = win32_open,
  .thumbnail = nullptr,
};

// ---------------------------------------------------------------- WASAPI loopback desktop audio

typedef struct ccord_loopback_win32 ccord_loopback_win32;
ccord_loopback_win32 *ccord_loopback_win32_open(void);
size_t ccord_loopback_win32_read(ccord_loopback_win32 *w, int16_t *pcm, size_t max_samples);
bool ccord_loopback_win32_failed(const ccord_loopback_win32 *w);
void ccord_loopback_win32_close(ccord_loopback_win32 *w);

struct ccord_loopback_win32 {
  bool com_init;
  bool failed;
  IMMDeviceEnumerator *enumerator;
  IMMDevice *device;
  IAudioClient *client;
  IAudioCaptureClient *capture;
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  bool is_float;
  int16_t ring[48000];
  size_t rhead, rcount;
};

static const CLSID k_CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID k_IID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID k_IID_IAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID k_IID_IAudioCaptureClient =
    {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID k_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static void ring_push(ccord_loopback_win32 *w, int16_t sample)
{
  const size_t cap = sizeof w->ring / sizeof w->ring[0];
  if (w->rcount == cap) {
    w->rhead = (w->rhead + 1u) % cap;
    w->rcount--;
  }
  w->ring[(w->rhead + w->rcount) % cap] = sample;
  w->rcount++;
}

ccord_loopback_win32 *ccord_loopback_win32_open(void)
{
  ccord_loopback_win32 *w = ccord_calloc(1, sizeof *w);
  if (!w) return nullptr;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) w->com_init = true;
  hr = CoCreateInstance(&k_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                        &k_IID_IMMDeviceEnumerator, (void **)&w->enumerator);
  if (FAILED(hr) || !w->enumerator) goto fail;
  hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(w->enumerator, eRender, eConsole, &w->device);
  if (FAILED(hr) || !w->device) goto fail;
  hr = IMMDevice_Activate(w->device, &k_IID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&w->client);
  if (FAILED(hr) || !w->client) goto fail;

  WAVEFORMATEX *mix = nullptr;
  hr = IAudioClient_GetMixFormat(w->client, &mix);
  if (FAILED(hr) || !mix) goto fail;
  w->rate = mix->nSamplesPerSec;
  w->channels = mix->nChannels;
  w->bits = mix->wBitsPerSample;
  if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    w->is_float = true;
  } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)mix;
    w->is_float = memcmp(&ext->SubFormat, &k_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(GUID)) == 0;
  }

  // 100 ms buffer in 100-ns units
  hr = IAudioClient_Initialize(w->client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                               1000000, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) goto fail;
  hr = IAudioClient_GetService(w->client, &k_IID_IAudioCaptureClient, (void **)&w->capture);
  if (FAILED(hr) || !w->capture) goto fail;
  hr = IAudioClient_Start(w->client);
  if (FAILED(hr)) goto fail;
  return w;

fail:
  ccord_loopback_win32_close(w);
  return nullptr;
}

size_t ccord_loopback_win32_read(ccord_loopback_win32 *w, int16_t *pcm, size_t max_samples)
{
  if (!w || w->failed || !w->capture || !pcm || max_samples == 0) return 0;
  for (;;) {
    UINT32 pkt = 0;
    HRESULT hr = IAudioCaptureClient_GetNextPacketSize(w->capture, &pkt);
    if (FAILED(hr)) { w->failed = true; break; }
    if (pkt == 0) break;
    BYTE *data = nullptr;
    UINT32 nframes = 0;
    DWORD flags = 0;
    hr = IAudioCaptureClient_GetBuffer(w->capture, &data, &nframes, &flags, nullptr, nullptr);
    if (FAILED(hr)) { w->failed = true; break; }
    uint32_t ch = w->channels ? w->channels : 2u;
    bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
    // Downmix to mono S16LE and linearly resample if device rate != 48 kHz.
    uint32_t out_frames = (w->rate == CCORD_LOOPBACK_RATE || w->rate == 0)
                              ? nframes
                              : (uint32_t)(((uint64_t)nframes * CCORD_LOOPBACK_RATE) / w->rate);
    for (uint32_t o = 0; o < out_frames; o++) {
      if (silent) {
        ring_push(w, 0);
        continue;
      }
      uint32_t src_i = (w->rate == CCORD_LOOPBACK_RATE || w->rate == 0)
                           ? o
                           : (uint32_t)(((uint64_t)o * w->rate) / CCORD_LOOPBACK_RATE);
      if (src_i >= nframes) src_i = nframes - 1u;
      float acc = 0.0f;
      if (w->is_float && w->bits == 32) {
        const float *fp = (const float *)data + (size_t)src_i * ch;
        for (uint32_t c = 0; c < ch; c++) acc += fp[c];
        acc /= (float)ch;
      } else if (w->bits == 16) {
        const int16_t *sp = (const int16_t *)data + (size_t)src_i * ch;
        for (uint32_t c = 0; c < ch; c++) acc += (float)sp[c] / 32768.0f;
        acc /= (float)ch;
      }
      if (acc > 1.0f) acc = 1.0f;
      if (acc < -1.0f) acc = -1.0f;
      ring_push(w, (int16_t)(acc * 32767.0f));
    }
    (void)IAudioCaptureClient_ReleaseBuffer(w->capture, nframes);
  }
  size_t take = w->rcount < max_samples ? w->rcount : max_samples;
  const size_t cap = sizeof w->ring / sizeof w->ring[0];
  for (size_t i = 0; i < take; i++) {
    pcm[i] = w->ring[w->rhead];
    w->rhead = (w->rhead + 1u) % cap;
    w->rcount--;
  }
  return take;
}

bool ccord_loopback_win32_failed(const ccord_loopback_win32 *w) { return !w || w->failed; }

void ccord_loopback_win32_close(ccord_loopback_win32 *w)
{
  if (!w) return;
  if (w->client) (void)IAudioClient_Stop(w->client);
  if (w->capture) IAudioCaptureClient_Release(w->capture);
  if (w->client) IAudioClient_Release(w->client);
  if (w->device) IMMDevice_Release(w->device);
  if (w->enumerator) IMMDeviceEnumerator_Release(w->enumerator);
  if (w->com_init) CoUninitialize();
  ccord_free(w);
}
