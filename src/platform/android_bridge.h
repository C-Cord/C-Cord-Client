// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Internal Android JNI bridge declarations (compiled on Android only).
#include <stdbool.h>
#include <stdint.h>

#if defined(__ANDROID__)
#include <jni.h>
#include <ccord/platform/platform.h>

// Registers the active ccord_platform instance so JNI callbacks (insets, file picker, network, voice actions)
// can push into its thread-safe queues. Pass nullptr on ccord_platform_destroy().
void ccord_android_register_platform(ccord_platform *p);

// Initializes FFmpeg's Android JavaVM handle for MediaCodec hardware acceleration.
void ccord_android_init_ffmpeg_jni(void);

// Queries Android device form factor from Kotlin (1 = phone, 2 = tablet).
int ccord_android_get_form_factor(void);

// Invokes native Android UX helpers via org.ccord.android.CCordBridge.
bool ccord_android_haptic_impact(int style);
bool ccord_android_haptic_notify(int kind);
bool ccord_android_share_text(const char *title, const char *text);
bool ccord_android_open_file_picker(void);
bool ccord_android_set_orientation(int mode);
bool ccord_android_keep_screen_on(bool on);
bool ccord_android_notify_message(uint32_t id, const char *title, const char *body,
                                  uint64_t guild_id, uint64_t channel_id, uint64_t message_id);
void ccord_android_set_voice_service(bool active, const char *channel_name, bool muted, bool deafened);
void ccord_android_set_audio_comm_mode(bool in_call);
bool ccord_android_request_screen_capture(void);
void ccord_android_stop_screen_capture(void);
void ccord_android_on_first_frame(void);
void ccord_android_set_auth_autofill(int mode, int active_field, bool commit,
                                     int email_x, int email_y, int email_w, int email_h,
                                     int pw_x, int pw_y, int pw_w, int pw_h,
                                     int otp_x, int otp_y, int otp_w, int otp_h,
                                     const char *email_text, const char *pw_text, const char *otp_text);

// Drains queued JNI events (picked files, voice notification actions, network changes) on the SDL main thread.
void ccord_android_drain_events(ccord_platform *p);

// Consumes pending insets/IME updates pushed by CCordActivity's WindowInsetsListener.
// Values are physical pixels; platform.c converts to logical px using display scale.
bool ccord_android_poll_insets(float *top, float *right, float *bottom, float *left, float *ime_bottom);

// Consumes one pending password-manager autofill value pushed by CCordActivity's AutofillOverlay.
bool ccord_android_poll_autofill(int *out_field, char *out_text, size_t cap);

// Called from desktop_android.c JNI entrypoints into capture_android.c.
void ccord_capture_android_push_frame(const uint8_t *rgba, int width, int height, int stride);
void ccord_capture_android_mark_stopped(void);

// JNI entrypoints invoked from Kotlin org.ccord.android.CCordBridge.
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnInsetsChanged(
    JNIEnv *env, jclass cls, jfloat top, jfloat right, jfloat bottom, jfloat left, jfloat ime_bottom);
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnAutofillField(
    JNIEnv *env, jclass cls, jint field, jstring jtext);
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnFilePicked(
    JNIEnv *env, jclass cls, jstring jpath);
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnNotificationClicked(
    JNIEnv *env, jclass cls, jint id, jlong guild_id, jlong channel_id, jlong message_id);
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnVoiceAction(
    JNIEnv *env, jclass cls, jint action);
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnNetworkChanged(
    JNIEnv *env, jclass cls);
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnCaptureFrame(
    JNIEnv *env, jclass cls, jobject buf, jint width, jint height, jint stride);
JNIEXPORT void JNICALL Java_org_ccord_android_CCordBridge_nativeOnCaptureStopped(
    JNIEnv *env, jclass cls);
#endif
