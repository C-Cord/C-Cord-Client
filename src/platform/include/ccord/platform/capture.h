// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Screen / window capture for Go Live. No SDL types. Backends: X11 (XShm + XComposite), xdg-desktop-portal
// ScreenCast + PipeWire (Wayland), and a deterministic FAKE backend (headless/mock verification).
//
// Threading: ccord_capture_list / ccord_capture_thumbnail are safe to call from any thread (each call uses its
// own connection). A ccord_capture handle must be used from ONE thread at a time.
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

typedef enum ccord_capture_kind : uint8_t { CCORD_CAPTURE_WINDOW = 0, CCORD_CAPTURE_SCREEN = 1 } ccord_capture_kind;
typedef enum ccord_capture_backend_id : uint8_t {
  CCORD_CAPBE_AUTO = 0,   // OS-native default (X11/portal on Linux, Win32 on Windows, ScreenCaptureKit/CG on macOS)
  CCORD_CAPBE_X11,
  CCORD_CAPBE_PORTAL,
  CCORD_CAPBE_FAKE,       // synthetic windows/screens with animated test patterns
  CCORD_CAPBE_WIN32,      // Windows DXGI / GDI desktop & window capture
  CCORD_CAPBE_MACOS,      // macOS ScreenCaptureKit / CoreGraphics capture
  CCORD_CAPBE_ANDROID,    // Android MediaProjection screen capture
} ccord_capture_backend_id;

#define CCORD_CAPTURE_ID_MAX 64
#define CCORD_CAPTURE_TITLE_MAX 128
#define CCORD_CAPTURE_APP_MAX 64

typedef struct ccord_capture_source {
  ccord_capture_kind kind;
  ccord_capture_backend_id backend;
  char id[CCORD_CAPTURE_ID_MAX];       // stable within a session: "x11:win:0x3a00007", "x11:mon:HDMI-1", "portal:pick", "fake:win:2"
  char title[CCORD_CAPTURE_TITLE_MAX]; // window title / monitor name, UTF-8, never empty
  char app[CCORD_CAPTURE_APP_MAX];     // application name (WM_CLASS) for windows, "" for screens
  int x, y, width, height;             // root coordinates (screens: monitor rect; windows: client rect); 0 size = unknown
  bool primary;                        // primary monitor
  uint64_t native;                     // backend handle (X11 window id), 0 if none
} ccord_capture_source;

// Pixel data is always BGRA8 (B,G,R,A byte order; alpha is 255 or ignored) with a row stride in bytes.
typedef struct ccord_capture_frame {
  const uint8_t *data;
  int width, height, stride;
} ccord_capture_frame;

typedef struct ccord_capture ccord_capture;   // an open capture session (opaque to callers)

// Resolves CCORD_CAPBE_AUTO to a concrete backend. Returns CCORD_CAPBE_FAKE if the process-wide fake switch is on
// (ccord_capture_set_fake(true)), otherwise X11 if DISPLAY is usable and the session is not Wayland-only,
// else PORTAL if XDG_SESSION_TYPE=wayland (or WAYLAND_DISPLAY set) and the portal is reachable, else AUTO (= none).
ccord_capture_backend_id ccord_capture_resolve_backend(ccord_capture_backend_id requested);
const char *ccord_capture_backend_name(ccord_capture_backend_id id);   // "X11", "PipeWire portal", "Test pattern", "none"
bool ccord_capture_backend_available(ccord_capture_backend_id id);

// Process-wide switch used by --mock / headless runs so the picker never exposes the real desktop.
void ccord_capture_set_fake(bool on);
bool ccord_capture_fake_enabled(void);

// Enumerates sources of `kind`; returns the number written to `out` (<= max). Windows are the visible top-level
// application windows (own process window excluded via ccord_capture_set_own_window_title/pid), most recently
// stacked first. Screens are the active monitors, primary first. 0 on failure (err filled if non-null).
size_t ccord_capture_list(ccord_capture_backend_id be, ccord_capture_kind kind, ccord_capture_source *out,
                          size_t max, ccord_error *err);
// Windows owned by this process (by pid via _NET_WM_PID, or exact title) are skipped by list(). Optional.
void ccord_capture_set_own_pid(int pid);

// Writes an RGBA8 thumbnail no larger than max_w x max_h (aspect preserved) into `rgba` (capacity >= max_w*max_h*4).
// Returns false on failure. Blocking, may take tens of ms; call from a worker thread.
bool ccord_capture_thumbnail(const ccord_capture_source *src, int max_w, int max_h, uint8_t *rgba, int *out_w,
                             int *out_h, ccord_error *err);

// Opens a capture session. For the portal backend this may show the system picker and block until the user
// chooses or cancels (call from a worker thread); `src` may be the "portal:pick" pseudo source.
ccord_capture *ccord_capture_open(const ccord_capture_source *src, ccord_error *err);
// Grabs the newest frame. Never blocks longer than ~100 ms. Returns true and fills *out when a frame is
// available (out->data valid until the next grab/close on this handle); false = no new frame / failure
// (call ccord_capture_failed() to distinguish a dead source such as a closed window).
bool ccord_capture_grab(ccord_capture *c, ccord_capture_frame *out);
bool ccord_capture_failed(const ccord_capture *c);       // source is gone (window closed, portal stream ended)
void ccord_capture_close(ccord_capture *c);

// ---- desktop / monitor loopback audio (Go Live "share audio") --------------------------------
// Captures what the desktop is playing (a PulseAudio/PipeWire sink monitor), never the microphone.
// PCM is always 48 kHz mono S16LE; one frame is 20 ms = CCORD_LOOPBACK_FRAME_SAMPLES samples.
// Threading: a ccord_loopback_audio handle must be used from ONE thread at a time.
#define CCORD_LOOPBACK_RATE 48000
#define CCORD_LOOPBACK_FRAME_SAMPLES 960u

typedef struct ccord_loopback_audio ccord_loopback_audio;   // an open loopback session (opaque)

// Process-wide fake switch for --mock / headless runs: open() then yields a deterministic 440 Hz
// tone instead of probing the real desktop. Fake mode is also enabled by the environment variable
// CCORD_CAPTURE_BACKEND_FAKE (any non-empty value other than "0") or by the capture fake switch
// (ccord_capture_set_fake), so mock runs never record the real desktop.
void ccord_loopback_audio_set_fake(bool on);
bool ccord_loopback_audio_fake_enabled(void);

// Opens monitor capture. Real backend (Linux): parec / pw-record reading a sink monitor over a
// non-blocking pipe. Returns null with err set when no monitor source is available (no server,
// no helper binaries, permission denied): callers fail soft and retry later. Fake always succeeds.
ccord_loopback_audio *ccord_loopback_audio_open(ccord_error *err);
// Non-blocking read of up to max_samples mono S16LE samples. Returns the number of samples written
// (0 = nothing available right now). The fake source emits one frame per call, paced by the caller.
size_t ccord_loopback_audio_read(ccord_loopback_audio *l, int16_t *pcm, size_t max_samples);
// True when the source died (capture child exited, monitor removed): close and reopen the handle.
bool ccord_loopback_audio_failed(const ccord_loopback_audio *l);
void ccord_loopback_audio_close(ccord_loopback_audio *l);

// ---- backend plumbing (implemented by capture_x11.c / capture_portal.c / capture.c fake) ----
typedef struct ccord_capture_ops {
  void (*close)(ccord_capture *c);
  bool (*grab)(ccord_capture *c, ccord_capture_frame *out);
  bool (*failed)(const ccord_capture *c);
} ccord_capture_ops;
struct ccord_capture {
  const ccord_capture_ops *ops;   // first member; backends embed `ccord_capture base;` first
  ccord_capture_source src;
};
typedef struct ccord_capture_backend {
  const char *name;
  bool (*available)(void);
  size_t (*list)(ccord_capture_kind kind, ccord_capture_source *out, size_t max, ccord_error *err);
  ccord_capture *(*open)(const ccord_capture_source *src, ccord_error *err);
  // Optional fast path; when null capture.c opens the source, grabs one frame and box-downscales it.
  bool (*thumbnail)(const ccord_capture_source *src, int max_w, int max_h, uint8_t *rgba, int *out_w, int *out_h,
                    ccord_error *err);
} ccord_capture_backend;
#if defined(_WIN32)
extern const ccord_capture_backend ccord_capture_win32_backend;    // capture_win32.c
#elif defined(__APPLE__)
extern const ccord_capture_backend ccord_capture_macos_backend;    // capture_macos.m
#elif defined(__ANDROID__)
extern const ccord_capture_backend ccord_capture_android_backend;  // capture_android.c
#else
extern const ccord_capture_backend ccord_capture_x11_backend;      // capture_x11.c
extern const ccord_capture_backend ccord_capture_portal_backend;   // capture_portal.c
#endif
int ccord_capture_own_pid(void);
// Shared helper (capture.c): BGRA -> RGBA box/nearest downscale into a max_w x max_h bound, aspect preserved.
bool ccord_capture_downscale_rgba(const ccord_capture_frame *in, int max_w, int max_h, uint8_t *rgba, int *out_w,
                                  int *out_h);
