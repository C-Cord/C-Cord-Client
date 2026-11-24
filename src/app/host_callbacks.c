// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/paths.h>
#include <ccord/core/atomic_file.h>
#include <ccord/platform/platform.h>
#include <SDL3/SDL_video.h>

#include <string.h>

// ---------------------------------------------------------------- host callbacks (ui -> os)

static bool cstr_opt_eq(const char *x, const char *y)
{
  if (x == y) return true;
  if (!x || !y) return false;
  return strcmp(x, y) == 0;
}

// Pushes ccord_account_view to the UI only when any field actually changed (ccord_ui_set_account_view marks ui->dirty).
void app_sync_account_view(app *a)
{
  if (!a || !a->ui || !a->account) return;
  const ccord_account_view *v = ccord_account_view_of(a->account);
  if (!v) return;
  const char *disp = v->display_name ? v->display_name : "";
  const char *user = v->username ? v->username : "";
  const char *stat = v->status ? v->status : "";
  const char *det = v->detail ? v->detail : "";
  if (a->last_av_valid &&
      v->mode == a->last_av.mode &&
      v->error == a->last_av.error &&
      v->configured == a->last_av.configured &&
      v->signed_in == a->last_av.signed_in &&
      v->busy == a->last_av.busy &&
      v->can_sign_in == a->last_av.can_sign_in &&
      v->can_sign_out == a->last_av.can_sign_out &&
      v->can_refresh == a->last_av.can_refresh &&
      v->demo_chip_visible == a->last_av.demo_chip_visible &&
      v->show_non_goals == a->last_av.show_non_goals &&
      cstr_opt_eq(v->heading, a->last_av.heading) &&
      strcmp(stat, a->last_av_status) == 0 &&
      strcmp(det, a->last_av_detail) == 0 &&
      strcmp(disp, a->last_av_display) == 0 &&
      strcmp(user, a->last_av_username) == 0 &&
      strcmp(v->user_id_text, a->last_av.user_id_text) == 0 &&
      v->guild_count == a->last_av.guild_count &&
      v->guilds_truncated == a->last_av.guilds_truncated &&
      v->non_goals == a->last_av.non_goals &&
      v->non_goal_count == a->last_av.non_goal_count &&
      v->identity == a->last_av.identity) {
    return;
  }
  a->last_av = *v;
  snprintf(a->last_av_display, sizeof a->last_av_display, "%s", disp);
  snprintf(a->last_av_username, sizeof a->last_av_username, "%s", user);
  snprintf(a->last_av_status, sizeof a->last_av_status, "%s", stat);
  snprintf(a->last_av_detail, sizeof a->last_av_detail, "%s", det);
  a->last_av_valid = true;
  ccord_ui_set_account_view(a->ui, v);
}

#if CCORD_LIVE_ACCOUNT
bool app_ensure_auth_q(app *a, ccord_error *err)
{
  if (a->auth_q) {
    a->auth_q_retire = false;
    return true;
  }
  ccord_rest_config auth_cfg = {.timeout_ms = 10000, .max_response_bytes = 65536};
  a->auth_q = ccord_rest_queue_create(&auth_cfg, err);
  a->auth_q_retire = false;
  return a->auth_q != nullptr;
}

void app_destroy_auth_q(app *a)
{
  if (a->auth_q) {
    ccord_rest_queue_destroy(a->auth_q);
    a->auth_q = nullptr;
  }
  a->auth_q_retire = false;
}
#endif

void app_host_clipboard_set(void *ud, const char *utf8)
{
  app *a = ud;
  ccord_error err;
  if (ccord_platform_clipboard_set(a->platform, utf8, &err) != CCORD_OK) CCORD_LOG_W("clipboard: %s", err.message);
}

static char *host_clipboard_get(void *ud)
{
  return ccord_platform_clipboard_get(((app *)ud)->platform);
}

static void host_set_cursor(void *ud, ccord_cursor_kind kind)
{
  ccord_platform_set_cursor(((app *)ud)->platform, kind);
}

static void host_set_fullscreen(void *ud, bool fullscreen)
{
  (void)ccord_platform_set_fullscreen(((app *)ud)->platform, fullscreen);
}

static void host_text_input_kind(void *ud, int kind)
{
  app *a = ud;
  if (!a) return;
  if (kind < (int)CCORD_INPUT_TEXT || kind > (int)CCORD_INPUT_SINGLE_LINE) kind = (int)CCORD_INPUT_TEXT;
  a->ime_kind = (ccord_text_input_kind)kind;
}

static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  app *a = ud;
  float z = (a && a->renderer) ? ccord_render_get_zoom(a->renderer) : 1.0f;
  ccord_platform_text_input_ex(a->platform, active, a->ime_kind, x * z, y * z, w * z, h * z);
}

static void host_auth_autofill(void *ud, int mode, int active_field, bool commit,
                               ccord_rect email_r, ccord_rect pw_r, ccord_rect otp_r,
                               const char *email_text, const char *pw_text, const char *otp_text)
{
  app *a = ud;
  if (!a || !a->platform) return;
  float z = a->renderer ? ccord_render_get_zoom(a->renderer) : 1.0f;
  ccord_auth_autofill_state st = {
    .mode = (mode >= 0 && mode <= 2) ? (ccord_auth_autofill_mode)mode : CCORD_AUTOFILL_NONE,
    .active_field = (active_field >= 0 && active_field <= 3) ? (ccord_auth_autofill_field)active_field : CCORD_AUTOFILL_FIELD_NONE,
    .commit = commit,
    .email_x = email_r.x * z, .email_y = email_r.y * z, .email_w = email_r.w * z, .email_h = email_r.h * z,
    .pw_x = pw_r.x * z, .pw_y = pw_r.y * z, .pw_w = pw_r.w * z, .pw_h = pw_r.h * z,
    .otp_x = otp_r.x * z, .otp_y = otp_r.y * z, .otp_w = otp_r.w * z, .otp_h = otp_r.h * z,
    .email_text = email_text,
    .pw_text = pw_text,
    .otp_text = otp_text,
  };
  ccord_platform_set_auth_autofill(a->platform, &st);
}

static void host_request_quit(void *ud)
{
  ((app *)ud)->quit = true;
}


// M6 desktop glue callbacks (Settings > Appearance theme/zoom, capability badges). The desktop object
// keeps the app-owned theme and renderer; the app keeps the ui viewport in sync with the zoom.
static void host_apply_theme(void *ud, const char *builtin_id)
{
  app *a = ud;
  if (a && a->desktop) ccord_app_desktop_apply_theme(a->desktop, builtin_id);
#if CCORD_WITH_PLUGINS
  if (a && a->plugins) ccord_plugins_dispatch_theme_change(a->plugins, builtin_id ? builtin_id : "");
#endif
}

static void host_select_theme_preset(void *ud, const char *owner, const char *preset_id)
{
  app *a = ud;
  if (a && a->desktop) ccord_app_desktop_select_theme_preset(a->desktop, owner, preset_id);
#if CCORD_WITH_PLUGINS
  if (a && a->plugins && a->theme) ccord_plugins_dispatch_theme_change(a->plugins, a->theme->id);
#endif
}

static void host_window_set_title(void *ud, const char *title)
{
  app *a = ud;
  if (a && a->platform) ccord_platform_set_title(a->platform, title ? title : "C-Cord");
}

static void host_window_set_badge(void *ud, int count)
{
  app *a = ud;
  if (!a || !a->platform) return;
  uint32_t c = count > 0 ? (uint32_t)count : 0u;
  ccord_platform_set_badge_counts(a->platform, c, c, c);
}

static void host_window_flash(void *ud)
{
  app *a = ud;
  if (!a || !a->platform) return;
  SDL_Window *sw = ccord_platform_sdl_window(a->platform);
  if (sw) (void)SDL_FlashWindow(sw, SDL_FLASH_BRIEFLY);
}

static bool host_window_is_focused(void *ud)
{
  app *a = ud;
  return a && a->platform && ccord_platform_window_focused(a->platform);
}

static bool host_play_earcon(void *ud, const char *kind_name)
{
  app *a = ud;
  return a && a->earcons && ccord_earcons_play_kind(a->earcons, kind_name, (int64_t)app_now_ms(a));
}

static bool host_play_audio_bytes(void *ud, const uint8_t *data, size_t len)
{
  app *a = ud;
  return a && a->earcons && ccord_earcons_play_bytes(a->earcons, data, len, (int64_t)app_now_ms(a));
}

static bool host_save_file_dialog(void *ud, const char *suggested_name, char *out_path, size_t cap)
{
  (void)ud;
  if (!out_path || cap == 0) return false;
  out_path[0] = '\0';
  const char *ddir = ccord_paths_data_dir();
  if (!ddir || !ddir[0]) return false;
  char exp_dir[CCORD_PATH_MAX];
  ccord_error err = {0};
  if (ccord_paths_join(exp_dir, sizeof exp_dir, ddir, "exports", &err) != CCORD_OK) return false;
  (void)ccord_mkdir_p(exp_dir, 0700, &err);
  char safe[128];
  size_t si = 0;
  const char *src = (suggested_name && suggested_name[0]) ? suggested_name : "export.bin";
  for (const char *p = src; *p && si + 1 < sizeof safe; p++) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '.' || c == '-' || c == '_') {
      safe[si++] = (char)c;
    } else {
      safe[si++] = '_';
    }
  }
  if (si == 0 || safe[0] == '.') {
    snprintf(safe, sizeof safe, "export.bin");
  } else {
    safe[si] = '\0';
  }
  return ccord_paths_join(out_path, cap, exp_dir, safe, &err) == CCORD_OK;
}

static void host_composer_changed(void *ud, ccord_id channel_id, const char *text)
{
  app *a = ud;
#if CCORD_WITH_PLUGINS
  if (a && a->plugins) ccord_plugins_dispatch_composer_change(a->plugins, channel_id, text);
#else
  (void)a; (void)channel_id; (void)text;
#endif
}

static void host_set_zoom(void *ud, float zoom)
{
  app *a = ud;
  if (!a || !a->desktop) return;
  ccord_app_desktop_set_zoom(a->desktop, zoom);
  if (a->ui && a->renderer) {
    float eff = ccord_render_get_zoom(a->renderer);
    if (eff <= 0.0f) eff = 1.0f;
    ccord_vec2 size = ccord_render_size(a->renderer);
    a->viewport_w = size.x;
    a->viewport_h = size.y;
    ccord_ui_set_viewport(a->ui, size.x, size.y);
    if (a->platform) {
      ccord_safe_insets raw_safe = ccord_platform_safe_area(a->platform);
      a->last_safe = (ccord_safe_insets){
        .top = raw_safe.top / eff,
        .right = raw_safe.right / eff,
        .bottom = raw_safe.bottom / eff,
        .left = raw_safe.left / eff,
      };
      ccord_ui_set_safe_insets(a->ui, a->last_safe.top, a->last_safe.right, a->last_safe.bottom, a->last_safe.left);
    }
    a->last_kb_inset = a->raw_kb_inset / eff;
    ccord_ui_set_keyboard_inset(a->ui, a->last_kb_inset);
#if CCORD_WITH_PLUGINS
    app_plugins_push_platform_state(a);
#endif
  }
}

static uint32_t host_desktop_capabilities(void *ud)
{
  app *a = ud;
  return a && a->desktop ? ccord_app_desktop_capabilities(a->desktop) : 0;
}

static void host_set_close_to_tray(void *ud, bool on)
{
  app *a = ud;
  if (a && a->platform) ccord_platform_set_close_to_tray(a->platform, on);
}

static void host_apply_quick_css(void *ud, bool enabled, const char *css)
{
  app *a = ud;
  if (a && a->desktop) ccord_app_desktop_apply_quick_css(a->desktop, enabled, css);
}

void app_host_keybinds_changed(void *ud)
{
  app *a = ud;
  if (a && a->desktop) (void)ccord_app_desktop_bind_shortcuts(a->desktop);
}

static void host_ensure_updater(app *a)
{
  if (!a || a->updater) return;
  a->updater_deferred = false;
  a->updater = ccord_updater_create(CCORD_VERSION_STRING, ccord_paths_data_dir(), false);
}

static void host_update_check(void *ud)
{
  app *a = ud;
  host_ensure_updater(a);
  if (a && a->updater) ccord_updater_check_now(a->updater);
}

static void host_update_install(void *ud)
{
  app *a = ud;
  host_ensure_updater(a);
  if (a && a->updater) ccord_updater_install_now(a->updater);
}

// SNI tray menu / click actions. CCORD_TRAY_ACTIVATE already toggled window visibility in the
// platform layer; the rest are applied here (voice through the session, quit through the loop).
void app_host_tray_action(void *ud, ccord_tray_action action)
{
  app *a = ud;
  if (!a) return;
  ccord_error err;
  switch (action) {
    case CCORD_TRAY_ACTIVATE:
      if (a->ui && a->platform && !ccord_platform_window_focused(a->platform)) {
        ccord_input_event unfocus = {.type = CCORD_IN_FOCUS, .focused = false};
        (void)ccord_ui_handle_input(a->ui, &unfocus);
      }
      break;   // visibility toggled by the platform; the next frame paints the restored window
    case CCORD_TRAY_TOGGLE_MUTE:
      (void)ccord_app_desktop_handle_hotkey(a->desktop, "toggle-mute", &err);
      break;
    case CCORD_TRAY_TOGGLE_DEAFEN:
      (void)ccord_app_desktop_handle_hotkey(a->desktop, "toggle-deafen", &err);
      break;
    case CCORD_TRAY_QUIT:
      a->quit = true;
      break;
    case CCORD_TRAY_DISCONNECT_VOICE: {
      if (a->store && a->session) {
        ccord_id vch = ccord_store_active_voice_channel(a->store);
        if (vch != CCORD_ID_NONE) {
          const ccord_channel *vchan = ccord_store_channel(a->store, vch);
          ccord_id g = vchan ? vchan->guild_id : ccord_store_selected_guild(a->store);
          (void)ccord_session_leave_voice(a->session, g, &err);
        }
      }
      break;
    }
  }
}

static bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  if (!url || !url[0]) return false;
  return ccord_platform_open_url(url);
}

static bool host_open_file_dialog(void *ud, char *out_path, size_t cap)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (out_path && cap > 0) out_path[0] = '\0';
  a->file_dialog_pending = true;
  ccord_platform_open_file_dialog(a->platform);
  if (out_path && cap > 0 && ccord_platform_pop_drop_file(a->platform, out_path, cap)) {
    a->file_dialog_pending = false;
    return true;
  }
  return false;
}

static bool host_open_dir_dialog(void *ud, char *out_path, size_t cap)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (out_path && cap > 0) out_path[0] = '\0';
  a->file_dialog_pending = true;
  ccord_platform_open_folder_dialog(a->platform);
  if (out_path && cap > 0 && ccord_platform_pop_drop_file(a->platform, out_path, cap)) {
    a->file_dialog_pending = false;
    return true;
  }
  return false;
}

// Upload a File (+ menu): starts the multi-select picker and returns at once. It must not pop the drop queue:
// every chosen path arrives as CCORD_EV_DROP_FILE and run_interactive attaches each one, so none is attached twice.
static void host_open_file_dialog_async(void *ud)
{
  app *a = ud;
  if (!a || !a->platform) return;
  a->file_dialog_pending = true;
  ccord_platform_open_file_dialog(a->platform);
}

// Schedule Message: the UI reaches the on-disk queue (scheduled_messages.c) through these.
bool app_host_scheduled_add(void *ud, ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                               int64_t now_unix_ms, const char *const *paths, const bool *spoilers, size_t npaths,
                               char *err_out, size_t err_cap)
{
  (void)ud;
  ccord_error err;
  if (ccord_scheduled_messages_add_files(channel_id, utf8, len, due_unix_ms, now_unix_ms, paths, spoilers, npaths, nullptr, &err) == CCORD_OK) return true;
  if (err_out && err_cap > 0) snprintf(err_out, err_cap, "%s", err.message);
  return false;
}

static size_t host_scheduled_list(void *ud, ccord_ui_scheduled_item *out, size_t cap)
{
  (void)ud;
  return ccord_scheduled_messages_list(out, cap);
}

bool app_host_scheduled_cancel(void *ud, uint64_t id)
{
  (void)ud;
  return ccord_scheduled_messages_cancel(id);
}

static bool host_scheduled_send_now(void *ud, uint64_t id, char *err_out, size_t err_cap)
{
  (void)ud;
  ccord_error err;
  if (ccord_scheduled_messages_send_now(id, &err) == CCORD_OK) return true;
  if (err_out && err_cap > 0) snprintf(err_out, err_cap, "%s", err.message);
  return false;
}

bool app_host_get_clipboard_image(void *ud, uint8_t **out_png, size_t *out_len)
{
  app *a = ud;
  if (!a || !a->platform || !out_png || !out_len) return false;
  *out_len = 0;
  *out_png = ccord_platform_get_clipboard_image(a->platform, out_len);
  return *out_png != nullptr && *out_len > 0;
}

static bool host_haptic_impact(void *ud, int style)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (style < 0 || style > CCORD_HAPTIC_SELECTION) style = CCORD_HAPTIC_LIGHT;
  return ccord_platform_haptic_impact(a->platform, (ccord_haptic_style)style);
}

static bool host_share_text(void *ud, const char *title, const char *text)
{
  app *a = ud;
  return a && a->platform && ccord_platform_share_text(a->platform, title, text);
}

#if defined(__ANDROID__)
static bool host_save_image(void *ud, const char *filename, const uint8_t *data, size_t len,
                            const char *fallback_url)
{
  app *a = ud;
  return a && a->platform && ccord_platform_save_image(a->platform, filename, data, len, fallback_url);
}

static bool host_share_image(void *ud, const char *filename, const uint8_t *data, size_t len,
                             const char *fallback_url)
{
  app *a = ud;
  return a && a->platform && ccord_platform_share_image(a->platform, filename, data, len, fallback_url);
}
#endif

static void host_dismiss_keyboard(void *ud)
{
  app *a = ud;
  if (a && a->platform) ccord_platform_dismiss_keyboard(a->platform);
}

uint64_t app_clock(void *ud)
{
  return ((app *)ud)->virtual_ms;
}


void app_fill_ui_host(app *a, ccord_ui_host *out)
{
  *out = (ccord_ui_host){
    .ud = a,
    .clipboard_set = app_host_clipboard_set,
    .clipboard_get = host_clipboard_get,
    .set_cursor = host_set_cursor,
    .text_input = host_text_input,
    .request_quit = host_request_quit,
    .account_action = app_host_account_action,
    .open_url = host_open_url,
    .open_file_dialog = host_open_file_dialog,
    .open_dir_dialog = host_open_dir_dialog,
    .open_file_dialog_async = host_open_file_dialog_async,
    .scheduled_add = app_host_scheduled_add,
    .scheduled_list = host_scheduled_list,
    .scheduled_cancel = app_host_scheduled_cancel,
    .scheduled_send_now = host_scheduled_send_now,
    .get_clipboard_image = app_host_get_clipboard_image,
    .apply_theme = host_apply_theme,
    .set_zoom = host_set_zoom,
    .desktop_capabilities = host_desktop_capabilities,
    .set_close_to_tray = host_set_close_to_tray,
    .apply_quick_css = host_apply_quick_css,
    .keybinds_changed = app_host_keybinds_changed,
    .account_switch = app_host_account_switch,
    .account_remove = app_host_account_remove,
    .update_check = host_update_check,
    .update_install = host_update_install,
    .haptic_impact = host_haptic_impact,
    .share_text = host_share_text,
#if defined(__ANDROID__)
    .save_image = host_save_image,
    .share_image = host_share_image,
#endif
    .dismiss_keyboard = host_dismiss_keyboard,
    .text_input_kind = host_text_input_kind,
    .auth_autofill = host_auth_autofill,
    .set_fullscreen = host_set_fullscreen,
    .window_set_title = host_window_set_title,
    .window_set_badge = host_window_set_badge,
    .window_flash = host_window_flash,
    .window_is_focused = host_window_is_focused,
    .play_earcon = host_play_earcon,
    .play_audio_bytes = host_play_audio_bytes,
    .save_file_dialog = host_save_file_dialog,
    .select_theme_preset = host_select_theme_preset,
    .composer_changed = host_composer_changed,
  };
#if CCORD_WITH_PLUGINS
  out->set_extensions_enabled = app_host_set_extensions;
#endif
}
