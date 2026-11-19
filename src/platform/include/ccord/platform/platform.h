// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/core/input.h>
#include <ccord/core/link_event.h>
// The only module (with render) that includes SDL headers. Public headers expose no SDL types.
typedef struct ccord_platform ccord_platform;
typedef struct ccord_platform_config {
  const char *title;
  int width, height;               // logical px, default 1280x800
  int min_width, min_height;
  bool headless;                   // true: SDL video driver "dummy" (no display needed)
  bool start_minimized;            // true: create the window hidden (tray-only start); window_visible=false
} ccord_platform_config;

// Event type emitted by ccord_platform_next_event when a file is dropped onto the window
// (SDL_EVENT_DROP_FILE), chosen via ccord_platform_open_file_dialog, or injected in tests.
// Note: ccord_input_event.text holds a 31-byte preview (NUL-terminated); callers requiring the
// full path must use ccord_platform_last_drop_file() or ccord_platform_pop_drop_file().
#define CCORD_EV_DROP_FILE ((ccord_input_type)(CCORD_IN_TEXT + 1))
// IME composition (preedit) update from SDL_EVENT_TEXT_EDITING: text holds the composition string
// (empty text clears the preedit); ime_start/ime_length are codepoint cursor/selection within it.
#define CCORD_EV_IME_PREEDIT ((ccord_input_type)(CCORD_IN_TEXT + 2))
// Global shortcut triggered via the XDG GlobalShortcuts portal (or injected in tests); text holds the shortcut id.
#define CCORD_EV_HOTKEY ((ccord_input_type)(CCORD_IN_TEXT + 3))

ccord_platform *ccord_platform_create(const ccord_platform_config *cfg, ccord_error *err);
void ccord_platform_destroy(ccord_platform *p);
// Waits up to timeout_ms (0 = poll only) for the first event, then returns true and fills `out`; call
// again with timeout_ms = 0 to drain. Events are translated to ccord_input_event; unknown SDL events are dropped.
bool ccord_platform_next_event(ccord_platform *p, ccord_input_event *out, uint32_t timeout_ms);
void ccord_platform_size(const ccord_platform *p, float *logical_w, float *logical_h, float *scale);   // scale = display scale * pixel density
void ccord_platform_set_title(ccord_platform *p, const char *utf8);
void ccord_platform_set_cursor(ccord_platform *p, ccord_cursor_kind kind);
void ccord_platform_text_input(ccord_platform *p, bool active, float x, float y, float w, float h);   // IME area
[[nodiscard]] ccord_status ccord_platform_clipboard_set(ccord_platform *p, const char *utf8, ccord_error *err);
char *ccord_platform_clipboard_get(ccord_platform *p);       // ccord_free() it; nullptr if empty/unavailable; only valid UTF-8 returned
void *ccord_platform_sdl_window(ccord_platform *p);           // SDL_Window*, for ccord_render only

// Browser URL launch, image clipboard, file drop/picker, and window urgency notifications ----
// Opens an http:// or https:// URL in the default browser (SDL_OpenURL). Rejects other schemes.
bool ccord_platform_open_url(const char *url);
const char *ccord_platform_last_opened_url(void);

// Reads clipboard image bytes ("image/png" or test-injected bytes). Caller frees returned buffer with ccord_free().
uint8_t *ccord_platform_get_clipboard_image(ccord_platform *p, size_t *out_len);
// Test/headless injection for clipboard image paste; pass data=nullptr or len=0 to clear.
[[nodiscard]] ccord_status ccord_platform_set_clipboard_image(ccord_platform *p, const uint8_t *data, size_t len,
                                                              ccord_error *err);

// Opens the native asynchronous file dialog (SDL_ShowOpenFileDialog); chosen paths arrive as CCORD_EV_DROP_FILE.
void ccord_platform_open_file_dialog(ccord_platform *p);
// Queues a file path to be delivered as a CCORD_EV_DROP_FILE input event (and sets last_drop_file).
[[nodiscard]] ccord_status ccord_platform_inject_drop_file(ccord_platform *p, const char *path, ccord_error *err);
// Returns the full UTF-8 path of the most recently delivered/injected CCORD_EV_DROP_FILE event (or nullptr).
// ccord_input_event.text holds only a 31-byte preview; use this or ccord_platform_pop_drop_file() for the full path.
const char *ccord_platform_last_drop_file(const ccord_platform *p);
// Pops the next pending dropped/picked file path into `out`. Returns true if a path was copied.
// Provides the full path for multi-file drops; ccord_input_event.text only holds a 31-byte preview.
bool ccord_platform_pop_drop_file(ccord_platform *p, char *out, size_t cap);

// Window focus state. C-Cord never flashes the taskbar entry; unread counts go to the tray icon.
bool ccord_platform_window_focused(const ccord_platform *p);
void ccord_platform_set_window_focused(ccord_platform *p, bool focused);
// Triggers a desktop notification when the window is unfocused: org.freedesktop.Notifications.Notify
// over the session bus when available (non-blocking), always with notify_count. No window flash.
// Returns true if notified. Invalid UTF-8 or a focused window is rejected (returns false).
bool ccord_platform_notify(ccord_platform *p, const char *title, const char *body);
uint32_t ccord_platform_notify_count(const ccord_platform *p);

// Click-to-open notifications. A message notification is sent with a "default" action (the click on its body) and the
// platform remembers which message it was for, keyed by the server's notification id (bounded table, an entry goes when
// the server reports the notification closed). When the server reports the default action, the callback gets that
// target during ccord_platform_next_event pumping, on the main thread. An unknown id or another action key does nothing.
typedef struct ccord_notify_target {
  uint64_t guild_id;     // ccord_id of the server (the DM pseudo-server for direct messages)
  uint64_t channel_id;
  uint64_t message_id;
} ccord_notify_target;
typedef void (*ccord_notify_click_fn)(void *ud, const ccord_notify_target *target);
void ccord_platform_set_notify_click_callback(ccord_platform *p, ccord_notify_click_fn fn, void *ud);
// Same gating as ccord_platform_notify; `target` (nullable) is what a click opens.
bool ccord_platform_notify_message(ccord_platform *p, const char *title, const char *body,
                                   const ccord_notify_target *target);
// Sink for tests: the id of the last notification (a local one when no server answers; 0 before the first), the
// action keys it was sent with ("default" for a message notification, "" otherwise), its target, and table size.
uint32_t ccord_platform_last_notify_id(const ccord_platform *p);
const char *ccord_platform_last_notify_actions(const ccord_platform *p);
bool ccord_platform_notify_target_for(const ccord_platform *p, uint32_t notification_id, ccord_notify_target *out);
size_t ccord_platform_notify_map_count(const ccord_platform *p);
// Test hooks: act as the ActionInvoked / NotificationClosed signals of the notification server.
void ccord_platform_inject_notify_action(ccord_platform *p, uint32_t notification_id, const char *action_key);
void ccord_platform_inject_notify_closed(ccord_platform *p, uint32_t notification_id);

// Desktop services: DBus notifications, SNI tray/unread badge, GlobalShortcuts portal ----
// Honest capability queries: false when no session bus / service is reachable (e.g. headless).
bool ccord_platform_notifications_available(const ccord_platform *p);
bool ccord_platform_tray_available(const ccord_platform *p);
bool ccord_platform_global_shortcuts_available(const ccord_platform *p);
// Deterministic notification sink: last accepted title/body (empty strings before the first notify).
// Recorded regardless of transport, so headless tests can inspect what would have been sent.
const char *ccord_platform_last_notify_title(const ccord_platform *p);
const char *ccord_platform_last_notify_body(const ccord_platform *p);
// Updates the SNI tray icon/tooltip when a StatusNotifierWatcher is present, and the "(N)" window-title badge.
// The tray number is the mention count here; ccord_platform_set_badge_counts takes it explicitly (e.g. mentions +
// unread DMs). The icon is only re-rendered (and NewIcon emitted) when the picture changes. Counts are always stored.
void ccord_platform_set_unread_badge(ccord_platform *p, uint32_t unread_count, uint32_t mention_count);
void ccord_platform_set_badge_counts(ccord_platform *p, uint32_t unread_count, uint32_t mention_count,
                                     uint32_t tray_count);
// Test hooks. Taskbar attention requests made (SDL_FlashWindow with anything but cancel); always 0.
uint32_t ccord_platform_flash_requests(const ccord_platform *p);
// The IconPixmap images served on the bus: index 0..3 = 22, 24, 32, 48 px, ARGB32 network byte order (A,R,G,B).
bool ccord_platform_tray_pixmap(const ccord_platform *p, size_t index, int *w, int *h, const uint8_t **argb);
// Test hook: how many embedded logo sizes were attached to the window icon at creation (0 = none/failed).
uint32_t ccord_platform_window_icon_sizes(const ccord_platform *p);
// The app id the window system sees (X11 WM_CLASS / Wayland app_id); "C-Cord" once a platform was created.
const char *ccord_platform_app_id(void);
// True when the video driver accepted the window icon (false under the dummy/headless driver).
bool ccord_platform_window_icon_accepted(const ccord_platform *p);
// NewIcon signals decided so far (picture changes only).
uint32_t ccord_platform_tray_new_icon_count(const ccord_platform *p);
void ccord_platform_unread_badge(const ccord_platform *p, uint32_t *unread_count, uint32_t *mention_count);
typedef struct ccord_global_shortcut {
  const char *id;                  // stable shortcut id delivered as CCORD_EV_HOTKEY text, e.g. "toggle-mute"
  const char *description;         // human-readable description shown by the portal
  const char *preferred_trigger;   // e.g. "CTRL+SHIFT+M"; nullptr lets the user choose
} ccord_global_shortcut;
// Creates a GlobalShortcuts portal session and binds the shortcuts (async). Returns false when the
// portal is unavailable; bindings are remembered and applied once the session is ready.
bool ccord_platform_bind_global_shortcuts(ccord_platform *p, const ccord_global_shortcut *shortcuts, size_t count);
// Queues a shortcut id to be delivered as a CCORD_EV_HOTKEY input event (test/headless injection).
// Injecting CCORD_SHORTCUT_PUSH_TO_TALK is a portal Activated: it sets the held state instead of queuing an event.
[[nodiscard]] ccord_status ccord_platform_inject_hotkey(ccord_platform *p, const char *shortcut_id, ccord_error *err);
// Push-to-talk shortcut id. The portal's Activated sets, and Deactivated or a lost portal session clears,
// ccord_platform_global_ptt_held; no CCORD_EV_HOTKEY event is emitted for it.
#define CCORD_SHORTCUT_PUSH_TO_TALK "push-to-talk"
// True while the push-to-talk shortcut is held, even when the window is unfocused. False without a portal.
bool ccord_platform_global_ptt_held(const ccord_platform *p);
// Test/headless injection of the portal's Deactivated signal and of the portal session ending.
[[nodiscard]] ccord_status ccord_platform_inject_shortcut_deactivated(ccord_platform *p, const char *shortcut_id,
                                                                      ccord_error *err);
void ccord_platform_inject_portal_session_closed(ccord_platform *p);

// Tray actions, voice state, window visibility and close-to-tray ----------------
// Actions emitted by SNI tray clicks and the com.canonical.dbusmenu context menu.
typedef enum ccord_tray_action : uint8_t {
  CCORD_TRAY_ACTIVATE = 0,       // left click / "Show/Hide C-Cord": platform toggles window visibility
  CCORD_TRAY_TOGGLE_MUTE = 1,    // middle click / "Mute Microphone" menu item
  CCORD_TRAY_TOGGLE_DEAFEN = 2,  // "Deafen Audio" menu item
  CCORD_TRAY_QUIT = 3,           // "Quit" menu item
  CCORD_TRAY_DISCONNECT_VOICE = 4, // Android voice notification "Disconnect" action
} ccord_tray_action;
typedef void (*ccord_tray_action_fn)(void *ud, ccord_tray_action action);
// Invoked on the main thread during ccord_platform_next_event pumping for each tray action.
// For CCORD_TRAY_ACTIVATE the platform has already toggled window visibility before the callback
// runs; the other actions are notifications for the app to apply (voice mute/deafen, quit).
void ccord_platform_set_tray_callback(ccord_platform *p, ccord_tray_action_fn fn, void *ud);
// Link events: the machine woke from suspend (CCORD_LINK_RESUMED) or the default routes changed (CCORD_LINK_CHANGED).
// The callback runs on the main thread inside ccord_platform_next_event (never a CCORD_IN_* input event); a platform
// with no callback drops them. Headless platforms open no netlink socket, so only injected events arrive there.
typedef void (*ccord_link_event_fn)(void *ud, ccord_link_event ev);
void ccord_platform_set_link_callback(ccord_platform *p, ccord_link_event_fn fn, void *ud);
// Test/headless injection: goes through the same queue and callback as a real event, at the next
// ccord_platform_next_event (like ccord_platform_inject_hotkey).
void ccord_platform_inject_link_event(ccord_platform *p, ccord_link_event ev);
// Syncs the tray menu Mute/Deafen checkmarks and the tray icon overlay with the local voice state.
void ccord_platform_set_voice_state(ccord_platform *p, bool muted, bool deafened);

// Window visibility. start_minimized (config) creates the window hidden; tracked even headless.
void ccord_platform_set_window_visible(ccord_platform *p, bool visible);
// Unconditionally shows, unminimizes (if minimized), and raises the window without ever toggling hide.
void ccord_platform_show_window(ccord_platform *p);
bool ccord_platform_window_visible(const ccord_platform *p);
// When enabled, a window-manager close request hides the window (to tray) instead of emitting
// CCORD_IN_QUIT; SDL_EVENT_QUIT still quits. Disabled by default.
void ccord_platform_set_close_to_tray(ccord_platform *p, bool enabled);
bool ccord_platform_close_to_tray(const ccord_platform *p);
// Handles a window close request. Returns true when the app should quit (close-to-tray off); otherwise hides
// the window (tray registered) or minimizes it (no tray, so it stays reachable) and returns false.
bool ccord_platform_request_close(ccord_platform *p);
bool ccord_platform_window_minimized(const ccord_platform *p);
// Test hooks: mode -1 = tray unavailable, 1 = available, 0 = real tray. inject runs a tray action as if clicked.
void ccord_platform_set_tray_override(ccord_platform *p, int mode);
void ccord_platform_inject_tray_action(ccord_platform *p, ccord_tray_action action);

// Audio media plane -----------------------------------------------------------
// Opaque SDL3 audio stream. Capture and playback are opened lazily so a
// headless/no-device host still gets a valid handle: ccord_audio_available()
// then returns false and read/write become safe no-ops.
typedef struct ccord_audio ccord_audio;
typedef struct ccord_audio_config {
  int sample_rate;                 // default 48000
  int channels;                    // default 2
  int frame_samples;               // default 960 (20 ms at 48 kHz)
  bool capture;                    // true: microphone; false: playback
  const char *device_id;           // decimal SDL device id from ccord_audio_devices_list; nullptr/"" = default
} ccord_audio_config;
ccord_audio *ccord_audio_create(const ccord_audio_config *cfg, ccord_error *err);
void ccord_audio_destroy(ccord_audio *a);
bool ccord_audio_available(const ccord_audio *a);
// Captures at most `frames` interleaved S16 frames; returns frames read (0 when idle/unavailable).
size_t ccord_audio_read(ccord_audio *a, int16_t *pcm, size_t frames);
// Queues at most `frames` interleaved S16 frames; returns frames accepted (0 when unavailable).
size_t ccord_audio_write(ccord_audio *a, const int16_t *pcm, size_t frames);
// Discards any buffered playback/capture samples already queued in the audio stream.
void ccord_audio_clear(ccord_audio *a);

// Video streaming codec pipeline (libavcodec / libswscale / libpipewire-0.3) --
typedef enum ccord_video_codec_id : uint8_t {
  CCORD_VIDEO_CODEC_H264 = 0,
  CCORD_VIDEO_CODEC_MPEG4 = 1,
} ccord_video_codec_id;

typedef struct ccord_video_config {
  int width;                       // default 640 (rounded to even in [16, 3840])
  int height;                      // default 360 (rounded to even in [16, 2160])
  int fps;                         // default 30
  int bitrate;                     // default 1500000
  ccord_video_codec_id codec;      // H.264 (with MPEG4 fallback) or MPEG4
  bool use_pipewire;               // probe PipeWire daemon socket availability; falls back to software frames when unavailable
  const char *preferred_encoder;   // optional encoder name ("h264_nvenc", "h264_vaapi", "h264_amf", "libx264", "libopenh264", or NULL/"auto")
} ccord_video_config;

typedef struct ccord_video_stream ccord_video_stream;
[[nodiscard]] ccord_video_stream *ccord_video_stream_create(const ccord_video_config *cfg, ccord_error *err);
void ccord_video_stream_destroy(ccord_video_stream *v);
const char *ccord_video_stream_codec_name(const ccord_video_stream *v);
int ccord_video_stream_width(const ccord_video_stream *v);
int ccord_video_stream_height(const ccord_video_stream *v);
int ccord_video_stream_fps(const ccord_video_stream *v);
bool ccord_video_stream_last_keyframe(const ccord_video_stream *v);
void ccord_video_stream_force_keyframe(ccord_video_stream *v);
// Resolves target stream dimensions (even in [16..3840]x[16..2160]) for a preset height (720, 1080, 1440, or 0 for Source).
void ccord_video_resolve_dimensions(int src_w, int src_h, int target_height, int *out_w, int *out_h);
// Returns true if PipeWire library and daemon socket connection were successfully probed during stream creation.
bool ccord_video_stream_pipewire_available(const ccord_video_stream *v);
// Encodes an RGBA32 frame (width x height, stride bytes/row or 0 for width*4) into an H.264/MPEG4 packet.
[[nodiscard]] ccord_status ccord_video_stream_encode_rgba(ccord_video_stream *v, const uint8_t *rgba, int width,
                                                          int height, int stride, uint8_t *out_pkt, size_t out_cap,
                                                          size_t *out_len, ccord_error *err);
// Encodes a BGRA32 frame (width x height, stride bytes/row or 0 for width*4) into an H.264/MPEG4 packet.
[[nodiscard]] ccord_status ccord_video_stream_encode_bgra(ccord_video_stream *v, const uint8_t *bgra, int width,
                                                          int height, int stride, uint8_t *out_pkt, size_t out_cap,
                                                          size_t *out_len, ccord_error *err);
// Decodes an encoded packet into a newly ccord_malloc'd RGBA32 buffer (*out_rgba, *out_w, *out_h) and updates last_rgba.
[[nodiscard]] ccord_status ccord_video_stream_decode_rgba(ccord_video_stream *v, const uint8_t *pkt, size_t pkt_len,
                                                          uint8_t **out_rgba, int *out_w, int *out_h, ccord_error *err);
// Synthesizes a procedural RGBA frame and runs it through encode -> decode so last_rgba holds a fresh stage frame.
[[nodiscard]] ccord_status ccord_video_stream_step_test_frame(ccord_video_stream *v, ccord_error *err);
// Returns the most recently decoded RGBA32 frame buffer owned by `v` (valid until next decode or destroy).
const uint8_t *ccord_video_stream_last_rgba(const ccord_video_stream *v, int *out_w, int *out_h);

// ---- M5 devices & codecs (implemented by m5-devices / m5-codecs) ------------------------------------------------
// Audio device enumeration (SDL_GetAudioRecordingDevices / SDL_GetAudioPlaybackDevices). Returns the number
// of devices written (<= max); 0 when none/headless. `id` is the decimal SDL device id, stable until unplug.
typedef struct ccord_audio_device_info {
  char id[64];
  char name[128];
  bool is_default;
} ccord_audio_device_info;
size_t ccord_audio_devices_list(bool capture, ccord_audio_device_info *out, size_t max);
// Hot-plug notification (SDL_EVENT_AUDIO_DEVICE_ADDED/REMOVED): invoked on the main thread from
// ccord_platform_next_event pumping; `capture` distinguishes recording vs playback devices. nullptr clears.
typedef void (*ccord_audio_hotplug_fn)(void *ud, bool capture, bool added);
void ccord_audio_set_hotplug_callback(ccord_audio_hotplug_fn fn, void *ud);

// Camera (SDL camera API; frames are BGRA32). Under the dummy video driver the list is empty and open fails.
typedef struct ccord_camera ccord_camera;
typedef struct ccord_camera_info {
  char id[64];
  char name[128];
} ccord_camera_info;
size_t ccord_camera_list(ccord_camera_info *out, size_t max);
[[nodiscard]] ccord_camera *ccord_camera_open(const char *device_id, ccord_error *err);
// Copies the newest frame into `bgra` (cap bytes); false when no new frame is ready. Dimensions via out_w/h.
bool ccord_camera_grab(ccord_camera *c, uint8_t *bgra, size_t cap, int *out_w, int *out_h);
void ccord_camera_close(ccord_camera *c);

// Standalone H.264 Annex-B decoder -> RGBA32 (Go Live receive, camera preview); libavcodec + libswscale.
typedef struct ccord_video_decoder ccord_video_decoder;
[[nodiscard]] ccord_video_decoder *ccord_video_decoder_create(ccord_error *err);
// Feeds one access unit; decoded frames surface via ccord_video_decoder_last_rgba.
[[nodiscard]] ccord_status ccord_video_decoder_feed(ccord_video_decoder *d, const uint8_t *annexb, size_t len,
                                                    ccord_error *err);
// Most recently decoded RGBA32 frame owned by `d` (valid until next feed or destroy); nullptr before the first.
const uint8_t *ccord_video_decoder_last_rgba(const ccord_video_decoder *d, int *out_w, int *out_h);
// Caps the output size (nullptr-safe). Each axis <= 0 means unbounded; both <= 0 (the default) is native size.
// Scales down only, keeps the aspect ratio, rounds to even; applies from the next decoded frame.
void ccord_video_decoder_set_max_dim(ccord_video_decoder *d, int max_w, int max_h);
void ccord_video_decoder_destroy(ccord_video_decoder *d);

// Soundboard decode: probes mp3/vorbis (libavcodec) and resamples (libswresample) to 48 kHz stereo S16LE.
// `cap` and *out_samples count interleaved stereo samples (frames * 2).
[[nodiscard]] ccord_status ccord_sound_decode(const uint8_t *data, size_t len, int16_t *out, size_t cap,
                                              size_t *out_samples, ccord_error *err);

// Voice-message encoder: streaming interleaved S16 -> Ogg/Opus (libavcodec libopus encoder + Ogg muxer), no temp file.
// _create fails with CCORD_ERR_UNSUPPORTED when the build has no Opus encoder and CCORD_ERR_INVALID for a bad
// sample_rate (48000 is what Discord plays) or channels (1 or 2).
typedef struct ccord_sound_encoder ccord_sound_encoder;
[[nodiscard]] ccord_sound_encoder *ccord_sound_encoder_create(uint32_t sample_rate, uint32_t channels,
                                                              ccord_error *err);
// Encodes `frames` interleaved frames (channels samples each); call as often as the mic delivers audio.
[[nodiscard]] ccord_status ccord_sound_encoder_feed(ccord_sound_encoder *enc, const int16_t *pcm, size_t frames,
                                                    ccord_error *err);
// Flushes the last packets and hands over the whole Ogg file: *out is ccord_malloc'd, the caller owns it and
// releases it with ccord_free(). Valid only after a successful finish.
[[nodiscard]] ccord_status ccord_sound_encoder_finish(ccord_sound_encoder *enc, uint8_t **out, size_t *out_len,
                                                      ccord_error *err);
void ccord_sound_encoder_destroy(ccord_sound_encoder *enc);

// ---- Mobile & Cross-Platform Capabilities (Android JNI + desktop safe defaults) --------------------------------
typedef enum ccord_form_factor : uint8_t {
  CCORD_FORM_DESKTOP = 0,
  CCORD_FORM_PHONE = 1,
  CCORD_FORM_TABLET = 2,
} ccord_form_factor;

typedef enum ccord_haptic_style : uint8_t {
  CCORD_HAPTIC_LIGHT = 0,
  CCORD_HAPTIC_MEDIUM = 1,
  CCORD_HAPTIC_HEAVY = 2,
  CCORD_HAPTIC_SELECTION = 3,
} ccord_haptic_style;

typedef enum ccord_haptic_notify_kind : uint8_t {
  CCORD_HAPTIC_NOTIFY_SUCCESS = 0,
  CCORD_HAPTIC_NOTIFY_WARNING = 1,
  CCORD_HAPTIC_NOTIFY_ERROR = 2,
} ccord_haptic_notify_kind;

typedef enum ccord_orientation_mode : uint8_t {
  CCORD_ORIENT_AUTO = 0,
  CCORD_ORIENT_PORTRAIT = 1,
  CCORD_ORIENT_LANDSCAPE = 2,
} ccord_orientation_mode;

typedef struct ccord_safe_insets {
  float top, right, bottom, left;
} ccord_safe_insets;

bool ccord_platform_is_mobile(const ccord_platform *p);
ccord_form_factor ccord_platform_form_factor(const ccord_platform *p);
bool ccord_platform_has_touch(const ccord_platform *p);
bool ccord_platform_has_haptics(const ccord_platform *p);
bool ccord_platform_has_share(const ccord_platform *p);
bool ccord_platform_in_background(const ccord_platform *p);
ccord_safe_insets ccord_platform_safe_area(const ccord_platform *p);
float ccord_platform_keyboard_inset(const ccord_platform *p);
bool ccord_platform_haptic_impact(ccord_platform *p, ccord_haptic_style style);
bool ccord_platform_haptic_notify(ccord_platform *p, ccord_haptic_notify_kind kind);
bool ccord_platform_share_text(ccord_platform *p, const char *title, const char *text);
bool ccord_platform_keep_screen_on(ccord_platform *p, bool on);
bool ccord_platform_set_orientation(ccord_platform *p, ccord_orientation_mode mode);
void ccord_platform_dismiss_keyboard(ccord_platform *p);
void ccord_platform_set_voice_call_active(ccord_platform *p, bool active, const char *channel_name,
                                          bool muted, bool deafened);
bool ccord_platform_request_permission(ccord_platform *p, const char *permission);
