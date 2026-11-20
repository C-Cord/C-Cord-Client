// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Android platform services & JNI bridge (implements desktop.h and android_bridge.h on Android).
#include "android_bridge.h"
#include "desktop.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <libavcodec/jni.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>

#define ANDROID_NOTIFY_MAP_CAP 32u
#define ANDROID_NOTIFY_TEXT_CAP 256u
#define ANDROID_BRIDGE_CLASS "org/ccord/android/CCordBridge"

typedef struct notify_entry {
  uint32_t id;
  ccord_notify_target target;
  bool used;
} notify_entry;

struct ccord_desktop {
  bool offline;
  uint32_t next_id;
  uint32_t last_id;
  char last_title[ANDROID_NOTIFY_TEXT_CAP];
  char last_body[ANDROID_NOTIFY_TEXT_CAP];
  char last_actions[32];
  notify_entry map[ANDROID_NOTIFY_MAP_CAP];
  size_t map_next;
  ccord_desktop_notify_action_fn notify_fn;
  void *notify_ud;
  ccord_desktop_tray_fn tray_fn;
  void *tray_ud;
  ccord_desktop_hotkey_fn hotkey_fn;
  void *hotkey_ud;
  ccord_desktop_shortcut_state_fn state_fn;
  void *state_ud;
  uint32_t pending_click_ids[16];
  size_t pending_click_count;
  ccord_notify_target pending_direct_targets[16];
  size_t pending_direct_count;
  bool voice_muted;
  bool voice_deafened;
};

static ccord_spin_lock g_jni_lock = CCORD_SPIN_LOCK_INIT;
static ccord_platform *g_platform = nullptr;
static ccord_desktop *g_desktop = nullptr;
static JavaVM *g_jvm = nullptr;
static jclass g_bridge_cls = nullptr;

// Pending WindowInsets pushed from Kotlin UI thread.
static bool g_insets_dirty = false;
static float g_insets_top = 0.0f;
static float g_insets_right = 0.0f;
static float g_insets_bottom = 0.0f;
static float g_insets_left = 0.0f;
static float g_insets_ime = 0.0f;

// Pending Autofill values pushed from Android AutofillManager (Bitwarden / password managers).
#define ANDROID_AUTOFILL_CAP 8u
#define ANDROID_AUTOFILL_TEXT_CAP 512u
typedef struct android_autofill_item {
  int field;
  char text[ANDROID_AUTOFILL_TEXT_CAP];
} android_autofill_item;
static android_autofill_item g_autofill_q[ANDROID_AUTOFILL_CAP];
static size_t g_autofill_head = 0;
static size_t g_autofill_count = 0;

// Queued main-thread events pushed from Kotlin UI / Service / Connectivity threads.
#define ANDROID_FILE_Q_CAP 8u
#define ANDROID_FILE_PATH_CAP 1024u
static char g_file_q[ANDROID_FILE_Q_CAP][ANDROID_FILE_PATH_CAP];
static size_t g_file_head = 0;
static size_t g_file_count = 0;

#define ANDROID_VOICE_ACTION_Q_CAP 16u
static int g_voice_action_q[ANDROID_VOICE_ACTION_Q_CAP];
static size_t g_voice_action_head = 0;
static size_t g_voice_action_count = 0;

static bool g_network_changed_pending = false;

// Standard UTF-8 -> Java UTF-16 jstring (safe for astral code points / emojis unlike NewStringUTF).
static jstring jni_new_string_utf8(JNIEnv *env, const char *utf8)
{
  if (!env) return nullptr;
  if (!utf8 || !utf8[0]) return (*env)->NewString(env, nullptr, 0);
  size_t len = strlen(utf8);
  // Each UTF-8 byte maps to at most 1 UTF-16 code unit (4-byte astral -> 2 code units).
  jchar stack_buf[256];
  jchar *u16 = len <= 256u ? stack_buf : (jchar *)ccord_malloc(len * sizeof(jchar));
  if (!u16) return nullptr;
  size_t out_n = 0;
  size_t pos = 0;
  while (pos < len) {
    uint32_t cp = 0;
    size_t step = ccord_utf8_decode(utf8 + pos, len - pos, &cp);
    if (step == 0) {
      cp = 0xFFFDu;
      step = 1u;
    }
    pos += step;
    if (cp <= 0xFFFFu) {
      u16[out_n++] = (jchar)cp;
    } else if (cp <= 0x10FFFFu) {
      cp -= 0x10000u;
      u16[out_n++] = (jchar)(0xD800u + (cp >> 10));
      u16[out_n++] = (jchar)(0xDC00u + (cp & 0x3FFu));
    } else {
      u16[out_n++] = (jchar)0xFFFDu;
    }
  }
  jstring res = (*env)->NewString(env, u16, (jsize)out_n);
  if (u16 != stack_buf) ccord_free(u16);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    return nullptr;
  }
  return res;
}

// Java UTF-16 jstring -> standard UTF-8 (merges surrogate pairs unlike GetStringUTFChars).
static bool jni_copy_string_utf8(JNIEnv *env, jstring jstr, char *out, size_t cap)
{
  if (!env || !jstr || !out || cap == 0) return false;
  out[0] = '\0';
  jsize n16 = (*env)->GetStringLength(env, jstr);
  if (n16 < 0 || (*env)->ExceptionCheck(env)) {
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    return false;
  }
  if (n16 == 0) return true;
  const jchar *u16 = (*env)->GetStringChars(env, jstr, nullptr);
  if (!u16 || (*env)->ExceptionCheck(env)) {
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (u16) (*env)->ReleaseStringChars(env, jstr, u16);
    return false;
  }
  size_t w = 0;
  for (jsize i = 0; i < n16; i++) {
    uint32_t cp = u16[i];
    if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < n16) {
      uint32_t lo = u16[i + 1];
      if (lo >= 0xDC00u && lo <= 0xDFFFu) {
        cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
        i++;
      } else {
        cp = 0xFFFDu;
      }
    } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
      cp = 0xFFFDu;
    }
    char enc[4];
    size_t elen = ccord_utf8_encode(cp, enc);
    if (elen == 0) continue;
    if (w + elen >= cap) break;
    memcpy(out + w, enc, elen);
    w += elen;
  }
  out[w] = '\0';
  (*env)->ReleaseStringChars(env, jstr, u16);
  return true;
}

static JNIEnv *android_env(bool *attached_out)
{
  if (attached_out) *attached_out = false;
  JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env) return env;
  if (!g_jvm) return nullptr;
  jint rc = (*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6);
  if (rc == JNI_OK && env) return env;
  if (rc == JNI_EDETACHED) {
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, nullptr) == JNI_OK && env) {
      if (attached_out) *attached_out = true;
      return env;
    }
  }
  return nullptr;
}

static void android_env_release(bool attached)
{
  if (attached && g_jvm) (void)(*g_jvm)->DetachCurrentThread(g_jvm);
}

static jclass bridge_class(JNIEnv *env)
{
  if (!env) return nullptr;
  if (g_bridge_cls) return g_bridge_cls;
  jclass local = (*env)->FindClass(env, ANDROID_BRIDGE_CLASS);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
    return nullptr;
  }
  if (!local) return nullptr;
  g_bridge_cls = (jclass)(*env)->NewGlobalRef(env, local);
  (*env)->DeleteLocalRef(env, local);
  return g_bridge_cls;
}

static void clear_exception(JNIEnv *env)
{
  if (env && (*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
  }
}

void ccord_android_init_ffmpeg_jni(void)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  if (!env) return;
  JavaVM *vm = nullptr;
  if ((*env)->GetJavaVM(env, &vm) == JNI_OK && vm) {
    g_jvm = vm;
    (void)av_jni_set_java_vm(vm, nullptr);
  }
  (void)bridge_class(env);
  android_env_release(attached);
}

void ccord_android_register_platform(ccord_platform *p)
{
  ccord_spin_lock_acquire(&g_jni_lock);
  g_platform = p;
  ccord_spin_lock_release(&g_jni_lock);
}

int ccord_android_get_form_factor(void)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  int res = 1;  // default: phone
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "getFormFactor", "()I");
    if (mid) res = (int)(*env)->CallStaticIntMethod(env, cls, mid);
    clear_exception(env);
  }
  android_env_release(attached);
  return res;
}

bool ccord_android_haptic_impact(int style)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "hapticImpact", "(I)Z");
    if (mid) ok = (*env)->CallStaticBooleanMethod(env, cls, mid, (jint)style) == JNI_TRUE;
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

bool ccord_android_haptic_notify(int kind)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "hapticNotify", "(I)Z");
    if (mid) ok = (*env)->CallStaticBooleanMethod(env, cls, mid, (jint)kind) == JNI_TRUE;
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

bool ccord_android_share_text(const char *title, const char *text)
{
  if (!text) return false;
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "shareText", "(Ljava/lang/String;Ljava/lang/String;)Z");
    if (mid) {
      jstring jtitle = jni_new_string_utf8(env, title ? title : "");
      jstring jtext = jni_new_string_utf8(env, text);
      if (jtitle && jtext) {
        ok = (*env)->CallStaticBooleanMethod(env, cls, mid, jtitle, jtext) == JNI_TRUE;
      }
      if (jtitle) (*env)->DeleteLocalRef(env, jtitle);
      if (jtext) (*env)->DeleteLocalRef(env, jtext);
    }
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

bool ccord_android_open_file_picker(void)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "openFilePicker", "()Z");
    if (mid) ok = (*env)->CallStaticBooleanMethod(env, cls, mid) == JNI_TRUE;
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

bool ccord_android_set_orientation(int mode)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "setOrientation", "(I)Z");
    if (mid) ok = (*env)->CallStaticBooleanMethod(env, cls, mid, (jint)mode) == JNI_TRUE;
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

bool ccord_android_keep_screen_on(bool on)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "setKeepScreenOn", "(Z)Z");
    if (mid) ok = (*env)->CallStaticBooleanMethod(env, cls, mid, on ? JNI_TRUE : JNI_FALSE) == JNI_TRUE;
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

bool ccord_android_notify_message(uint32_t id, const char *title, const char *body,
                                  uint64_t guild_id, uint64_t channel_id, uint64_t message_id)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "notifyMessage",
                                              "(ILjava/lang/String;Ljava/lang/String;JJJ)Z");
    if (mid) {
      jstring jtitle = jni_new_string_utf8(env, title ? title : "C-Cord");
      jstring jbody = jni_new_string_utf8(env, body ? body : "");
      if (jtitle && jbody) {
        ok = (*env)->CallStaticBooleanMethod(env, cls, mid, (jint)id, jtitle, jbody,
                                             (jlong)guild_id, (jlong)channel_id, (jlong)message_id) == JNI_TRUE;
      }
      if (jtitle) (*env)->DeleteLocalRef(env, jtitle);
      if (jbody) (*env)->DeleteLocalRef(env, jbody);
    }
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

void ccord_android_set_voice_service(bool active, const char *channel_name, bool muted, bool deafened)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "setVoiceService", "(ZLjava/lang/String;ZZ)V");
    if (mid) {
      jstring jch = jni_new_string_utf8(env, channel_name ? channel_name : "Voice");
      if (jch) {
        (*env)->CallStaticVoidMethod(env, cls, mid, active ? JNI_TRUE : JNI_FALSE, jch,
                                     muted ? JNI_TRUE : JNI_FALSE, deafened ? JNI_TRUE : JNI_FALSE);
        (*env)->DeleteLocalRef(env, jch);
      }
    }
    clear_exception(env);
  }
  android_env_release(attached);
}

void ccord_android_set_audio_comm_mode(bool in_call)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "setAudioCommMode", "(Z)V");
    if (mid) (*env)->CallStaticVoidMethod(env, cls, mid, in_call ? JNI_TRUE : JNI_FALSE);
    clear_exception(env);
  }
  android_env_release(attached);
}

bool ccord_android_request_screen_capture(void)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  bool ok = false;
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "requestScreenCapture", "()Z");
    if (mid) ok = (*env)->CallStaticBooleanMethod(env, cls, mid) == JNI_TRUE;
    clear_exception(env);
  }
  android_env_release(attached);
  return ok;
}

void ccord_android_stop_screen_capture(void)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "stopScreenCapture", "()V");
    if (mid) (*env)->CallStaticVoidMethod(env, cls, mid);
    clear_exception(env);
  }
  android_env_release(attached);
}

void ccord_android_on_first_frame(void)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(env, cls, "onFirstFrameRendered", "()V");
    if (mid) (*env)->CallStaticVoidMethod(env, cls, mid);
    clear_exception(env);
  }
  android_env_release(attached);
}

void ccord_android_set_auth_autofill(int mode, int active_field, bool commit,
                                     int email_x, int email_y, int email_w, int email_h,
                                     int pw_x, int pw_y, int pw_w, int pw_h,
                                     int otp_x, int otp_y, int otp_w, int otp_h,
                                     const char *email_text, const char *pw_text, const char *otp_text)
{
  bool attached = false;
  JNIEnv *env = android_env(&attached);
  jclass cls = bridge_class(env);
  if (env && cls) {
    jmethodID mid = (*env)->GetStaticMethodID(
        env, cls, "setAuthAutofill",
        "(IIZ" "IIII" "IIII" "IIII" "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (mid) {
      jstring jemail = jni_new_string_utf8(env, email_text ? email_text : "");
      jstring jpw = jni_new_string_utf8(env, pw_text ? pw_text : "");
      jstring jotp = jni_new_string_utf8(env, otp_text ? otp_text : "");
      if (jemail && jpw && jotp) {
        (*env)->CallStaticVoidMethod(
            env, cls, mid, (jint)mode, (jint)active_field, commit ? JNI_TRUE : JNI_FALSE,
            (jint)email_x, (jint)email_y, (jint)email_w, (jint)email_h,
            (jint)pw_x, (jint)pw_y, (jint)pw_w, (jint)pw_h,
            (jint)otp_x, (jint)otp_y, (jint)otp_w, (jint)otp_h,
            jemail, jpw, jotp);
      }
      if (jemail) (*env)->DeleteLocalRef(env, jemail);
      if (jpw) (*env)->DeleteLocalRef(env, jpw);
      if (jotp) (*env)->DeleteLocalRef(env, jotp);
    }
    clear_exception(env);
  }
  android_env_release(attached);
}

bool ccord_android_poll_autofill(int *out_field, char *out_text, size_t cap)
{
  bool got = false;
  ccord_spin_lock_acquire(&g_jni_lock);
  if (g_autofill_count > 0) {
    android_autofill_item *it = &g_autofill_q[g_autofill_head];
    if (out_field) *out_field = it->field;
    if (out_text && cap > 0) {
      size_t len = strlen(it->text);
      size_t n = len < cap - 1u ? len : cap - 1u;
      while (n > 0 && !ccord_utf8_valid(it->text, n)) n--;
      if (n > 0) memcpy(out_text, it->text, n);
      out_text[n] = '\0';
    }
    memset(it, 0, sizeof *it);
    g_autofill_head = (g_autofill_head + 1u) % ANDROID_AUTOFILL_CAP;
    g_autofill_count--;
    got = true;
  }
  ccord_spin_lock_release(&g_jni_lock);
  return got;
}

void ccord_android_drain_events(ccord_platform *p)
{
  if (!p) return;
  char files[ANDROID_FILE_Q_CAP][ANDROID_FILE_PATH_CAP];
  size_t nfiles = 0;
  int vactions[ANDROID_VOICE_ACTION_Q_CAP];
  size_t nvactions = 0;
  bool net_changed = false;

  ccord_spin_lock_acquire(&g_jni_lock);
  while (g_file_count > 0 && nfiles < ANDROID_FILE_Q_CAP) {
    memcpy(files[nfiles++], g_file_q[g_file_head], ANDROID_FILE_PATH_CAP);
    g_file_q[g_file_head][0] = '\0';
    g_file_head = (g_file_head + 1u) % ANDROID_FILE_Q_CAP;
    g_file_count--;
  }
  while (g_voice_action_count > 0 && nvactions < ANDROID_VOICE_ACTION_Q_CAP) {
    vactions[nvactions++] = g_voice_action_q[g_voice_action_head];
    g_voice_action_head = (g_voice_action_head + 1u) % ANDROID_VOICE_ACTION_Q_CAP;
    g_voice_action_count--;
  }
  net_changed = g_network_changed_pending;
  g_network_changed_pending = false;
  ccord_spin_lock_release(&g_jni_lock);

  for (size_t i = 0; i < nfiles; i++) {
    ccord_error err = {0};
    (void)ccord_platform_inject_drop_file(p, files[i], &err);
  }
  for (size_t i = 0; i < nvactions; i++) {
    ccord_platform_inject_tray_action(p, (ccord_tray_action)vactions[i]);
  }
  if (net_changed) {
    ccord_platform_inject_link_event(p, CCORD_LINK_CHANGED);
  }
}

bool ccord_android_poll_insets(float *top, float *right, float *bottom, float *left, float *ime_bottom)
{
  ccord_spin_lock_acquire(&g_jni_lock);
  bool dirty = g_insets_dirty;
  if (dirty) {
    if (top) *top = g_insets_top;
    if (right) *right = g_insets_right;
    if (bottom) *bottom = g_insets_bottom;
    if (left) *left = g_insets_left;
    if (ime_bottom) *ime_bottom = g_insets_ime;
    g_insets_dirty = false;
  }
  ccord_spin_lock_release(&g_jni_lock);
  return dirty;
}

// ---- JNI entrypoints invoked from Kotlin org.ccord.android.CCordBridge ----

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnInsetsChanged(JNIEnv *env, jclass cls, jfloat top, jfloat right,
                                                         jfloat bottom, jfloat left, jfloat ime_bottom)
{
  (void)env;
  (void)cls;
  ccord_spin_lock_acquire(&g_jni_lock);
  g_insets_top = top >= 0.0f ? top : 0.0f;
  g_insets_right = right >= 0.0f ? right : 0.0f;
  g_insets_bottom = bottom >= 0.0f ? bottom : 0.0f;
  g_insets_left = left >= 0.0f ? left : 0.0f;
  g_insets_ime = ime_bottom >= 0.0f ? ime_bottom : 0.0f;
  g_insets_dirty = true;
  ccord_spin_lock_release(&g_jni_lock);
}

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnAutofillField(JNIEnv *env, jclass cls, jint field, jstring jtext)
{
  (void)cls;
  if (!env || !jtext || field < 1 || field > 3) return;
  char utf[ANDROID_AUTOFILL_TEXT_CAP];
  if (!jni_copy_string_utf8(env, jtext, utf, sizeof utf)) return;
  ccord_spin_lock_acquire(&g_jni_lock);
  size_t tail = (g_autofill_head + g_autofill_count) % ANDROID_AUTOFILL_CAP;
  android_autofill_item *it = &g_autofill_q[tail];
  it->field = (int)field;
  memcpy(it->text, utf, sizeof it->text);
  if (g_autofill_count < ANDROID_AUTOFILL_CAP) {
    g_autofill_count++;
  } else {
    g_autofill_head = (g_autofill_head + 1u) % ANDROID_AUTOFILL_CAP;
  }
  ccord_spin_lock_release(&g_jni_lock);
  memset(utf, 0, sizeof utf);
}

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnFilePicked(JNIEnv *env, jclass cls, jstring jpath)
{
  (void)cls;
  if (!env || !jpath) return;
  char path[ANDROID_FILE_PATH_CAP];
  if (!jni_copy_string_utf8(env, jpath, path, sizeof path) || !path[0]) return;
  ccord_spin_lock_acquire(&g_jni_lock);
  size_t tail = (g_file_head + g_file_count) % ANDROID_FILE_Q_CAP;
  memcpy(g_file_q[tail], path, sizeof g_file_q[tail]);
  if (g_file_count < ANDROID_FILE_Q_CAP) {
    g_file_count++;
  } else {
    g_file_head = (g_file_head + 1u) % ANDROID_FILE_Q_CAP;
  }
  ccord_spin_lock_release(&g_jni_lock);
}

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnNotificationClicked(JNIEnv *env, jclass cls, jint id, jlong guild_id,
                                                               jlong channel_id, jlong message_id)
{
  (void)env;
  (void)cls;
  ccord_spin_lock_acquire(&g_jni_lock);
  ccord_desktop *d = g_desktop;
  if (d) {
    if (guild_id != 0 || channel_id != 0 || message_id != 0) {
      if (d->pending_direct_count < 16u) {
        d->pending_direct_targets[d->pending_direct_count++] = (ccord_notify_target){
          .guild_id = (uint64_t)guild_id,
          .channel_id = (uint64_t)channel_id,
          .message_id = (uint64_t)message_id,
        };
      }
    } else if (id > 0 && d->pending_click_count < 16u) {
      d->pending_click_ids[d->pending_click_count++] = (uint32_t)id;
    }
  }
  ccord_spin_lock_release(&g_jni_lock);
}

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnVoiceAction(JNIEnv *env, jclass cls, jint action)
{
  (void)env;
  (void)cls;
  if (action < 0 || action > CCORD_TRAY_DISCONNECT_VOICE) return;
  ccord_spin_lock_acquire(&g_jni_lock);
  size_t tail = (g_voice_action_head + g_voice_action_count) % ANDROID_VOICE_ACTION_Q_CAP;
  g_voice_action_q[tail] = (int)action;
  if (g_voice_action_count < ANDROID_VOICE_ACTION_Q_CAP) {
    g_voice_action_count++;
  } else {
    g_voice_action_head = (g_voice_action_head + 1u) % ANDROID_VOICE_ACTION_Q_CAP;
  }
  ccord_spin_lock_release(&g_jni_lock);
}

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnNetworkChanged(JNIEnv *env, jclass cls)
{
  (void)env;
  (void)cls;
  ccord_spin_lock_acquire(&g_jni_lock);
  g_network_changed_pending = true;
  ccord_spin_lock_release(&g_jni_lock);
}

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnCaptureFrame(JNIEnv *env, jclass cls, jobject buf, jint width,
                                                        jint height, jint stride)
{
  (void)cls;
  if (!env || !buf || width <= 0 || height <= 0 || width > 8192 || height > 8192 || stride < width * 4) return;
  jlong cap = (*env)->GetDirectBufferCapacity(env, buf);
  if (cap <= 0) return;
  uint64_t needed = (uint64_t)(height - 1) * (uint64_t)stride + (uint64_t)width * 4u;
  if (needed > (uint64_t)cap) return;
  const uint8_t *rgba = (const uint8_t *)(*env)->GetDirectBufferAddress(env, buf);
  if (rgba) ccord_capture_android_push_frame(rgba, (int)width, (int)height, (int)stride);
}

JNIEXPORT void JNICALL
Java_org_ccord_android_CCordBridge_nativeOnCaptureStopped(JNIEnv *env, jclass cls)
{
  (void)env;
  (void)cls;
  ccord_capture_android_mark_stopped();
}

// ---- desktop.h implementation for Android ----

static void copy_str(char *dst, size_t cap, const char *src)
{
  if (cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  size_t n = len < cap - 1u ? len : cap - 1u;
  while (n > 0 && !ccord_utf8_valid(src, n)) n--;
  if (n > 0) memcpy(dst, src, n);
  dst[n] = '\0';
}

ccord_desktop *ccord_desktop_create(const char *app_id, bool offline)
{
  (void)app_id;
  ccord_desktop *d = ccord_calloc(1, sizeof *d);
  if (!d) return nullptr;
  d->offline = offline;
  d->next_id = 1u;
  ccord_spin_lock_acquire(&g_jni_lock);
  g_desktop = d;
  ccord_spin_lock_release(&g_jni_lock);
  return d;
}

void ccord_desktop_destroy(ccord_desktop *d)
{
  if (!d) return;
  ccord_spin_lock_acquire(&g_jni_lock);
  if (g_desktop == d) g_desktop = nullptr;
  ccord_spin_lock_release(&g_jni_lock);
  ccord_free(d);
}

void ccord_desktop_pump(ccord_desktop *d)
{
  if (!d) return;
  uint32_t ids[16];
  size_t nids = 0;
  ccord_notify_target direct[16];
  size_t ndirect = 0;
  ccord_spin_lock_acquire(&g_jni_lock);
  nids = d->pending_click_count;
  if (nids > 0) {
    memcpy(ids, d->pending_click_ids, nids * sizeof ids[0]);
    d->pending_click_count = 0;
  }
  ndirect = d->pending_direct_count;
  if (ndirect > 0) {
    memcpy(direct, d->pending_direct_targets, ndirect * sizeof direct[0]);
    d->pending_direct_count = 0;
  }
  ccord_spin_lock_release(&g_jni_lock);

  for (size_t i = 0; i < nids; i++) {
    ccord_desktop_inject_notify_action(d, ids[i], "default");
  }
  for (size_t i = 0; i < ndirect; i++) {
    if (d->notify_fn) d->notify_fn(d->notify_ud, &direct[i]);
  }
}

bool ccord_desktop_notifications_available(const ccord_desktop *d)
{
  return d && !d->offline;
}

bool ccord_desktop_notify(ccord_desktop *d, const char *title, const char *body)
{
  return ccord_desktop_notify_target(d, title, body, nullptr);
}

bool ccord_desktop_notify_target(ccord_desktop *d, const char *title, const char *body,
                                 const ccord_notify_target *target)
{
  if (!d) return false;
  if (title && !ccord_utf8_valid(title, strlen(title))) return false;
  if (body && !ccord_utf8_valid(body, strlen(body))) return false;
  copy_str(d->last_title, sizeof d->last_title, title);
  copy_str(d->last_body, sizeof d->last_body, body);
  uint32_t id = d->next_id++;
  if (d->next_id == 0) d->next_id = 1u;
  d->last_id = id;
  if (target) {
    snprintf(d->last_actions, sizeof d->last_actions, "default");
    size_t slot = d->map_next++ % ANDROID_NOTIFY_MAP_CAP;
    d->map[slot] = (notify_entry){.id = id, .target = *target, .used = true};
  } else {
    d->last_actions[0] = '\0';
  }
  if (!d->offline) {
    (void)ccord_android_notify_message(id, d->last_title, d->last_body,
                                       target ? target->guild_id : 0,
                                       target ? target->channel_id : 0,
                                       target ? target->message_id : 0);
  }
  return true;
}

const char *ccord_desktop_last_notify_title(const ccord_desktop *d) { return d ? d->last_title : ""; }
const char *ccord_desktop_last_notify_body(const ccord_desktop *d) { return d ? d->last_body : ""; }
void ccord_desktop_set_notify_action_callback(ccord_desktop *d, ccord_desktop_notify_action_fn fn, void *ud)
{
  if (!d) return;
  d->notify_fn = fn;
  d->notify_ud = ud;
}
uint32_t ccord_desktop_last_notify_id(const ccord_desktop *d) { return d ? d->last_id : 0; }
const char *ccord_desktop_last_notify_actions(const ccord_desktop *d) { return d ? d->last_actions : ""; }

bool ccord_desktop_notify_target_for(const ccord_desktop *d, uint32_t id, ccord_notify_target *out)
{
  if (!d || id == 0) return false;
  for (size_t i = 0; i < ANDROID_NOTIFY_MAP_CAP; i++) {
    if (d->map[i].used && d->map[i].id == id) {
      if (out) *out = d->map[i].target;
      return true;
    }
  }
  return false;
}

size_t ccord_desktop_notify_map_count(const ccord_desktop *d)
{
  if (!d) return 0;
  size_t n = 0;
  for (size_t i = 0; i < ANDROID_NOTIFY_MAP_CAP; i++) {
    if (d->map[i].used) n++;
  }
  return n;
}

void ccord_desktop_inject_notify_action(ccord_desktop *d, uint32_t id, const char *action_key)
{
  if (!d || !action_key || strcmp(action_key, "default") != 0) return;
  ccord_notify_target target;
  if (ccord_desktop_notify_target_for(d, id, &target) && d->notify_fn) {
    d->notify_fn(d->notify_ud, &target);
  }
}

void ccord_desktop_inject_notify_closed(ccord_desktop *d, uint32_t id)
{
  if (!d || id == 0) return;
  for (size_t i = 0; i < ANDROID_NOTIFY_MAP_CAP; i++) {
    if (d->map[i].used && d->map[i].id == id) {
      d->map[i].used = false;
      break;
    }
  }
}

bool ccord_desktop_tray_available(const ccord_desktop *d) { (void)d; return false; }
void ccord_desktop_set_badge(ccord_desktop *d, uint32_t unread, uint32_t mentions, uint32_t count)
{
  (void)d; (void)unread; (void)mentions; (void)count;
}
bool ccord_desktop_tray_pixmap(const ccord_desktop *d, size_t index, int *w, int *h, const uint8_t **argb)
{
  (void)d; (void)index; (void)w; (void)h; (void)argb;
  return false;
}
uint32_t ccord_desktop_new_icon_count(const ccord_desktop *d) { (void)d; return 0; }
void ccord_desktop_set_tray_callback(ccord_desktop *d, ccord_desktop_tray_fn fn, void *ud)
{
  if (!d) return;
  d->tray_fn = fn;
  d->tray_ud = ud;
}
void ccord_desktop_set_voice_state(ccord_desktop *d, bool muted, bool deafened)
{
  if (!d) return;
  d->voice_muted = muted;
  d->voice_deafened = deafened;
}
bool ccord_desktop_global_shortcuts_available(const ccord_desktop *d) { (void)d; return false; }
bool ccord_desktop_bind_shortcuts(ccord_desktop *d, const ccord_global_shortcut *defs, size_t n)
{
  (void)d; (void)defs; (void)n;
  return false;
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
