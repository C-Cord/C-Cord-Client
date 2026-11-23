// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "settings_internal.h"
#include <limits.h>

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
  if (strcmp(section, "reminders") == 0) {
    ui_reminders_load_ini(ui, key, value);
    return true;
  }
  if (strcmp(section, "pinned_dms") == 0 || strcmp(section, "favorite_dms") == 0 ||
      strcmp(section, "friend_nicknames") == 0 || strcmp(section, "ignored_users") == 0 ||
      strcmp(section, "hide_muted_guilds") == 0 || strcmp(section, "pinned_channels") == 0 ||
      strcmp(section, "hidden_voice_names") == 0 || strcmp(section, "hidden_channels") == 0 ||
      strcmp(section, "show_all_channels") == 0) {
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
    clamp_int(&iv, 0, (int)CCORD_STREAM_CODEC_AV1);
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
  ui->nreminders = 0;    // rebuild the reminder list from this file
  ccord_error err;
  if (ccord_ini_parse(text, n, settings_visit, ui, &err) != CCORD_OK) ccord_error_clear(&err);
  ccord_free(text);
  ui_saved_messages_apply_loaded(ui);   // push the loaded bookmarks into the store (newest first)
}

void ui_settings_init(ccord_ui *ui)
{
  st_s_ptt_armed = false;
  st_s_kb_armed = -1;
  st_s_voice_scroll = 0.0f;
  ccord_scroller_cancel(&st_s_voice_glide);
  st_s_dev.valid = false;
  st_s_save_pending = false;
  st_s_save_ui = nullptr;
  st_s_save_due_ms = 0;
  st_s_hover_key = -1;
  st_s_last_drawn_page = -1;
  ui_settings_m6_reset();
  ui_settings_m6_prefs_reset();
  st_settings_init_defaults(ui);
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
  ui_reminders_save_ini(ui, f);           // [reminders] message reminders
  ui_dm_prefs_save_ini(ui, f);            // [pinned_dms], [favorite_dms], [friend_nicknames], [ignored_users]
}

void st_settings_save(ccord_ui *ui)
{
  st_s_save_pending = false;
  st_s_save_ui = nullptr;
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

void st_settings_save_flush(ccord_ui *ui)
{
  if (!st_s_save_pending || st_s_save_ui != ui) return;
  st_settings_save(ui);
}

void ui_settings_save(ccord_ui *ui)
{
  if (!ui) return;
  st_settings_init_defaults(ui);
  st_settings_save(ui);
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
  st_settings_save(ui);
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

