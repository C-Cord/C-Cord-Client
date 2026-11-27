// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 desktop host glue: startup INI prefs, ccord_ui_host callbacks, tray badge, notifications and
// GlobalShortcuts. See app_desktop.h for the contract. Nothing here mutates Discord state: theme,
// zoom, badge and notifications are local; mute/deafen go through ccord_session intents.
#define _GNU_SOURCE
#include "app_desktop.h"

#include <ccord/assets/embedded_assets.h>
#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/ini.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>

#include <SDL3/SDL_hints.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------- startup prefs (settings.ini)

typedef struct startup_visit_state {
  ccord_app_startup *out;
} startup_visit_state;

static bool ini_bool_value(const char *value, bool *out)
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

static bool ini_float_value(const char *value, float *out)
{
  if (!value || !value[0]) return false;
  char *end = nullptr;
  float v = strtof(value, &end);
  if (end == value || (end && *end != '\0')) return false;
  if (!isfinite(v)) return false;
  *out = v;
  return true;
}

// Theme ids are builtin names: lowercase letters, digits, '-' and '_' (keeps path/INI input safe).
static bool theme_id_ok(const char *id)
{
  if (!id || !id[0]) return false;
  size_t n = strlen(id);
  if (n >= CCORD_APP_THEME_ID_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

static bool startup_visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  (void)line;
  (void)err;
  startup_visit_state *st = ud;
  if (strcmp(section, "ui") != 0) return true;
  if (strcmp(key, "theme") == 0) {
    if (value) {
      if (strcmp(value, "discord-dark") == 0 || strcmp(value, "ccord-dark") == 0) {
        snprintf(st->out->theme_id, sizeof st->out->theme_id, "c-cord-dark");
      } else if (strcmp(value, "discord-light") == 0 || strcmp(value, "ccord-light") == 0) {
        snprintf(st->out->theme_id, sizeof st->out->theme_id, "c-cord-light");
      } else if (strcmp(value, "discord-amoled") == 0 || strcmp(value, "ccord-amoled") == 0) {
        snprintf(st->out->theme_id, sizeof st->out->theme_id, "c-cord-amoled");
      } else if (theme_id_ok(value)) {
        snprintf(st->out->theme_id, sizeof st->out->theme_id, "%s", value);
      }
    }
    return true;
  }
  if (strcmp(key, "zoom") == 0) {
    float z = 0.0f;
    if (ini_float_value(value, &z)) {
      if (z < CCORD_ZOOM_MIN) z = CCORD_ZOOM_MIN;
      if (z > CCORD_ZOOM_MAX) z = CCORD_ZOOM_MAX;
      st->out->zoom = z;
    }
    return true;
  }
  if (strcmp(key, "hardware_accel") == 0) {
    bool on = true;
    if (ini_bool_value(value, &on)) st->out->hardware_accel = on;
    return true;
  }
  if (strcmp(key, "close_to_tray") == 0) {
    bool on = true;
    if (ini_bool_value(value, &on)) st->out->close_to_tray = on;
    return true;
  }
  if (strcmp(key, "quickcss_enabled") == 0) {
    bool on = false;
    if (ini_bool_value(value, &on)) st->out->quickcss_enabled = on;
    return true;
  }
  if (strcmp(key, "quickcss") == 0 && value) {
    snprintf(st->out->quickcss, sizeof st->out->quickcss, "%s", value);
    return true;
  }
  return true;
}

void ccord_app_desktop_startup_defaults(ccord_app_startup *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  snprintf(out->theme_id, sizeof out->theme_id, "%s", CCORD_APP_THEME_FALLBACK);
  out->zoom = 1.0f;
  out->hardware_accel = true;
  out->close_to_tray = true;   // run in the background on close by default
}

bool ccord_app_desktop_should_draw(const ccord_platform *p)
{
  return !p || (!ccord_platform_in_background(p) && ccord_platform_window_visible(p) && !ccord_platform_window_minimized(p));
}

void ccord_app_desktop_handle_show(ccord_platform *p)
{
  if (!p) return;
  // Unconditionally show, unminimize (if minimized), and raise the window without ever toggling hide,
  // even when p->focused was optimistically true before FocusIn/FocusOut or across virtual desktops.
  ccord_platform_show_window(p);
}

void ccord_app_desktop_startup_read(ccord_app_startup *out, const char *ini_path)
{
  if (!out) return;
  ccord_app_desktop_startup_defaults(out);
  char path[CCORD_PATH_MAX];
  if (ini_path && ini_path[0]) {
    snprintf(path, sizeof path, "%s", ini_path);
  } else {
    const char *dir = ccord_paths_data_dir();
    ccord_error err;
    if (!dir || !dir[0] || ccord_paths_join(path, sizeof path, dir, "settings.ini", &err) != CCORD_OK) return;
  }
  FILE *f = fopen(path, "rb");
  if (!f) return;
  char text[8192];
  size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = '\0';
  startup_visit_state st = {.out = out};
  ccord_error err;
  if (ccord_ini_parse(text, n, startup_visit, &st, &err) != CCORD_OK) ccord_error_clear(&err);
}

[[nodiscard]] ccord_status ccord_app_desktop_load_theme(ccord_theme *theme, const ccord_app_startup *startup,
                                                        const char *cli_path, ccord_error *err)
{
  ccord_error_clear(err);
  if (!theme) return ccord_error_set(err, CCORD_ERR_INVALID, "app: null theme");
  if (cli_path && cli_path[0]) {
    // --theme wins over settings.ini; an unreadable explicit path is an error as before.
    return ccord_theme_load(theme, cli_path, err);
  }
  const char *id = startup && theme_id_ok(startup->theme_id) ? startup->theme_id : CCORD_APP_THEME_FALLBACK;
  if (ccord_theme_load_builtin(theme, id, err) == CCORD_OK) return CCORD_OK;
  if (strcmp(id, CCORD_APP_THEME_FALLBACK) != 0 &&
      ccord_theme_load_builtin(theme, CCORD_APP_THEME_FALLBACK, err) == CCORD_OK) {
    return CCORD_OK;
  }
  size_t tlen = 0;
  const char *emb = ccord_embedded_theme(&tlen);
  if (emb && ccord_theme_parse(theme, emb, tlen, err) == CCORD_OK) return CCORD_OK;
  return err ? err->code : CCORD_ERR_PARSE;
}

void ccord_app_desktop_prepare_renderer(const ccord_app_startup *startup)
{
  if (startup && !startup->hardware_accel) (void)SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
}

// ---------------------------------------------------------------- object

#define APP_TRACKED_NOTIFY_MAX 32u

typedef struct app_tracked_notify {
  ccord_id channel_id;
  ccord_id message_id;
  ccord_id initial_last_read_id;   // channel's last_read_id when the notification was delivered
  uint64_t seq;
} app_tracked_notify;

struct ccord_app_desktop {
  ccord_store *store;
  ccord_session *session;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_theme *theme;
  ccord_contrib *contrib;
  ccord_ui *ui;
  char ptt_trigger[48];           // push-to-talk trigger last offered to the portal ("" = none)
  char asset_dir[CCORD_PATH_MAX];
  ccord_theme base_theme;                  // pristine base snapshot; QuickCSS restores it before overlaying
  bool quickcss_enabled;
  char quickcss[256];
  char active_preset_owner[65];
  char active_preset_id[65];
  uint64_t applied_theme_rev;
  char applied_font_reg[CCORD_PATH_MAX];
  char applied_font_bold[CCORD_PATH_MAX];
  ccord_audio *playback;          // lazy notification beep stream (closed after idle silence)
  int64_t playback_busy_until_ms;
  bool audio_cap_probed;
  bool audio_cap_avail;
  ccord_app_desktop_sound_fn sound_hook;   // optional notification sound (earcons); null = legacy tone
  ccord_app_desktop_wake_fn wake_hook;     // optional Desktop Companion Wake emitter (bare ping, zero content)
  void *wake_ud;
  int16_t *beep_pcm;
  size_t beep_frames;
  uint64_t badge_revision;
  bool badge_valid;
  uint32_t badge_last_unread;
  uint32_t badge_last_mentions;
  uint32_t badge_last_tray;
  bool badge_pushed;
  uint64_t voice_sync_rev;
  bool voice_sync_valid;          // tray voice state: push only when mute/deafen actually changed
  bool redraw;
  app_tracked_notify tracked_notifs[APP_TRACKED_NOTIFY_MAX];
  size_t tracked_notif_count;
  uint64_t tracked_notif_seq;
  uint64_t notif_sync_rev;
  bool notif_sync_valid;
  ccord_app_desktop_debug dbg;
};

ccord_app_desktop *ccord_app_desktop_create(const ccord_app_desktop_deps *deps, ccord_error *err)
{
  ccord_error_clear(err);
  if (!deps || !deps->theme) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "app desktop: null deps");
    return nullptr;
  }
  ccord_app_desktop *d = ccord_calloc(1, sizeof *d);
  if (!d) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "app desktop: out of memory");
    return nullptr;
  }
  d->store = deps->store;
  d->session = deps->session;
  d->platform = deps->platform;
  d->renderer = deps->renderer;
  d->theme = deps->theme;
  d->contrib = deps->contrib;
  d->base_theme = *deps->theme;   // pristine base for QuickCSS restore (also covers --theme files)
  const char *assets = deps->asset_dir ? deps->asset_dir : ccord_paths_asset_dir();
  if (assets) snprintf(d->asset_dir, sizeof d->asset_dir, "%s", assets);
  return d;
}

void ccord_app_desktop_destroy(ccord_app_desktop *d)
{
  if (!d) return;
  // The platform owns the SDL audio subsystem; close our stream before it is destroyed.
  ccord_audio_destroy(d->playback);
  ccord_free(d->beep_pcm);
  ccord_free(d);
}

void ccord_app_desktop_attach_ui(ccord_app_desktop *d, ccord_ui *ui)
{
  if (!d) return;
  d->ui = ui;
  d->dbg.ui_attached = ui != nullptr;
}

const ccord_theme *ccord_app_desktop_base_theme(const ccord_app_desktop *d)
{
  return d ? &d->base_theme : nullptr;
}

static void desktop_load_theme_into(const ccord_app_desktop *d, ccord_theme *dst, const char *builtin_id)
{
  const char *id = theme_id_ok(builtin_id) ? builtin_id : CCORD_APP_THEME_FALLBACK;
  ccord_error err;
  bool loaded = false;
  if (d->asset_dir[0]) {
    char rel[128], path[CCORD_PATH_MAX];
    snprintf(rel, sizeof rel, "themes/%s.theme", id);
    if (ccord_paths_join(path, sizeof path, d->asset_dir, rel, &err) == CCORD_OK &&
        ccord_theme_load(dst, path, &err) == CCORD_OK) {
      loaded = true;
    }
  }
  if (!loaded && ccord_theme_load_builtin(dst, id, &err) == CCORD_OK) loaded = true;
  if (!loaded && strcmp(id, CCORD_APP_THEME_FALLBACK) != 0) {
    (void)ccord_theme_load_builtin(dst, CCORD_APP_THEME_FALLBACK, &err);
  }
}

// Loads a builtin/asset base theme in place and snapshots it for QuickCSS restore.
static void desktop_load_base(ccord_app_desktop *d, const char *builtin_id)
{
  desktop_load_theme_into(d, d->theme, builtin_id);
  d->base_theme = *d->theme;
}

// Overlays the remembered QuickCSS snippet onto the current base (no-op when disabled/empty).
static void desktop_quick_css_overlay(ccord_app_desktop *d)
{
  if (!d->quickcss_enabled || !d->quickcss[0]) return;
  ccord_error err;
  size_t applied = 0;
  if (ccord_theme_apply_quick_css(d->theme, d->quickcss, strlen(d->quickcss), &applied, &err) == CCORD_OK) {
    d->dbg.quick_css_applies++;
  } else {
    ccord_error_clear(&err);   // malformed snippets never break the base theme
  }
}

static void desktop_rebuild_theme_stack(ccord_app_desktop *d)
{
  if (!d || !d->theme) return;
  *d->theme = d->base_theme;
  const char *font_reg = "";
  const char *font_bold = "";
  ccord_error err = {0};

  // 1. Selected Class 1 theme preset (if active and still registered)
  if (d->contrib && d->active_preset_owner[0] && d->active_preset_id[0]) {
    const ccord_theme_preset *pr =
      ccord_contrib_find_theme_preset(d->contrib, d->active_preset_owner, d->active_preset_id);
    if (pr) {
      if (pr->base_id && pr->base_id[0]) desktop_load_theme_into(d, d->theme, pr->base_id);
      if (pr->colors_json || pr->metrics_json) {
        (void)ccord_theme_apply_json(d->theme, pr->colors_json, pr->metrics_json, nullptr, &err);
      }
      if (pr->css && pr->css[0]) {
        (void)ccord_theme_apply_plugin_css(d->theme, pr->css, strlen(pr->css), nullptr, &err);
      }
      if (pr->font_regular && pr->font_regular[0]) font_reg = pr->font_regular;
      if (pr->font_bold && pr->font_bold[0]) font_bold = pr->font_bold;
    }
  }

  // 2. User QuickCSS
  desktop_quick_css_overlay(d);

  // 3. Class 2 live theme layers sorted by priority ascending
  if (d->contrib) {
    size_t n = ccord_contrib_theme_layer_count(d->contrib);
    const ccord_theme_layer *ordered[32];
    size_t count = 0;
    for (size_t i = 0; i < n && count < 32; i++) {
      const ccord_theme_layer *l = ccord_contrib_theme_layer_at(d->contrib, i);
      if (l) ordered[count++] = l;
    }
    for (size_t i = 1; i < count; i++) {
      const ccord_theme_layer *key = ordered[i];
      size_t j = i;
      while (j > 0 && ordered[j - 1]->priority > key->priority) {
        ordered[j] = ordered[j - 1];
        j--;
      }
      ordered[j] = key;
    }
    for (size_t i = 0; i < count; i++) {
      const ccord_theme_layer *l = ordered[i];
      if (l->base_id && l->base_id[0]) desktop_load_theme_into(d, d->theme, l->base_id);
      if (l->colors_json || l->metrics_json) {
        (void)ccord_theme_apply_json(d->theme, l->colors_json, l->metrics_json, nullptr, &err);
      }
      if (l->css && l->css[0]) {
        (void)ccord_theme_apply_plugin_css(d->theme, l->css, strlen(l->css), nullptr, &err);
      }
      if (l->font_regular && l->font_regular[0]) font_reg = l->font_regular;
      if (l->font_bold && l->font_bold[0]) font_bold = l->font_bold;
    }
    d->applied_theme_rev = ccord_contrib_theme_revision(d->contrib);
  }

  if (d->renderer &&
      (strcmp(d->applied_font_reg, font_reg) != 0 || strcmp(d->applied_font_bold, font_bold) != 0)) {
    (void)ccord_render_set_custom_fonts(d->renderer, font_reg[0] ? font_reg : nullptr,
                                        font_bold[0] ? font_bold : nullptr);
    snprintf(d->applied_font_reg, sizeof d->applied_font_reg, "%s", font_reg);
    snprintf(d->applied_font_bold, sizeof d->applied_font_bold, "%s", font_bold);
  }
  if (d->platform && d->theme) {
    ccord_color bg = d->theme->c.bg_primary;
    uint32_t argb = 0xFF000000u | ((uint32_t)bg.r << 16) | ((uint32_t)bg.g << 8) | (uint32_t)bg.b;
    uint32_t luma = ((uint32_t)bg.r * 299u + (uint32_t)bg.g * 587u + (uint32_t)bg.b * 114u) / 1000u;
    ccord_platform_set_theme_bg(d->platform, argb, luma >= 140u);
  }
  d->redraw = true;
}

void ccord_app_desktop_apply_theme(void *ud, const char *builtin_id)
{
  ccord_app_desktop *d = ud;
  if (!d || !d->theme) return;
  desktop_load_base(d, builtin_id);
  desktop_rebuild_theme_stack(d);
}

void ccord_app_desktop_select_theme_preset(void *ud, const char *owner, const char *preset_id)
{
  ccord_app_desktop *d = ud;
  if (!d || !d->theme) return;
  snprintf(d->active_preset_owner, sizeof d->active_preset_owner, "%s", owner ? owner : "");
  snprintf(d->active_preset_id, sizeof d->active_preset_id, "%s", preset_id ? preset_id : "");
  desktop_rebuild_theme_stack(d);
}

void ccord_app_desktop_apply_quick_css(void *ud, bool enabled, const char *css)
{
  ccord_app_desktop *d = ud;
  if (!d || !d->theme) return;
  d->quickcss_enabled = enabled;
  snprintf(d->quickcss, sizeof d->quickcss, "%s", css ? css : "");
  desktop_rebuild_theme_stack(d);
}

bool ccord_app_desktop_sync_plugin_theme(ccord_app_desktop *d)
{
  if (!d || !d->contrib || !d->theme) return false;
  uint64_t rev = ccord_contrib_theme_revision(d->contrib);
  if (rev == d->applied_theme_rev) return false;
  desktop_rebuild_theme_stack(d);
  return true;
}

void ccord_app_desktop_set_zoom(void *ud, float zoom)
{
  ccord_app_desktop *d = ud;
  if (!d || !d->renderer) return;
  if (!(zoom >= CCORD_ZOOM_MIN)) zoom = CCORD_ZOOM_MIN;   // also catches NaN
  if (zoom > CCORD_ZOOM_MAX) zoom = CCORD_ZOOM_MAX;
  ccord_render_set_zoom(d->renderer, zoom);
  d->redraw = true;
}

void ccord_app_desktop_scale_event_for_zoom(ccord_input_event *ev, float zoom)
{
  if (!ev || !(zoom > 0.0f) || zoom == 1.0f) return;
  if (ev->type == CCORD_EV_DROP_POSITION || ev->type == CCORD_EV_DROP_FILE) {
    ev->x /= zoom;
    ev->y /= zoom;
    return;
  }
  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE:
    case CCORD_IN_MOUSE_DOWN:
    case CCORD_IN_MOUSE_UP:
    case CCORD_IN_WHEEL:
    case CCORD_IN_TOUCH_DOWN:
    case CCORD_IN_TOUCH_MOVE:
    case CCORD_IN_TOUCH_UP:
    case CCORD_IN_TOUCH_CANCEL:
    case CCORD_IN_RESIZE:
      ev->x /= zoom;
      ev->y /= zoom;
      break;
    case CCORD_IN_SAFE_AREA:
      ev->inset_top /= zoom;
      ev->inset_right /= zoom;
      ev->inset_bottom /= zoom;
      ev->inset_left /= zoom;
      break;
    case CCORD_IN_KEYBOARD_INSET:
      ev->inset_bottom /= zoom;
      break;
    default:
      break;
  }
}

#define APP_PLAYBACK_IDLE_MS 2000

static ccord_audio *app_playback(ccord_app_desktop *d)
{
  if (d->playback) return d->playback;
  ccord_audio_config cfg = {.sample_rate = 48000, .channels = 1, .frame_samples = 960, .capture = false};
  ccord_error err;
  d->playback = ccord_audio_create(&cfg, &err);
  if (d->playback && !ccord_audio_available(d->playback)) {
    ccord_audio_destroy(d->playback);
    d->playback = nullptr;
  }
  d->audio_cap_probed = true;
  d->audio_cap_avail = d->playback != nullptr;
  return d->playback;
}

uint32_t ccord_app_desktop_capabilities(void *ud)
{
  ccord_app_desktop *d = ud;
  if (!d) return 0;
  uint32_t caps = 0;
  bool audio_ok = false;
  if (d->playback) {
    audio_ok = ccord_audio_available(d->playback);
  } else if (d->audio_cap_probed) {
    audio_ok = d->audio_cap_avail;
  } else {
    // Probe without keeping an idle 48 kHz audio stream open permanently.
    ccord_audio_config cfg = {.sample_rate = 48000, .channels = 1, .frame_samples = 960, .capture = false};
    ccord_error err;
    ccord_audio *probe = ccord_audio_create(&cfg, &err);
    audio_ok = ccord_audio_available(probe);
    ccord_audio_destroy(probe);
    d->audio_cap_probed = true;
    d->audio_cap_avail = audio_ok;
  }
  if (audio_ok) caps |= CCORD_UI_CAP_AUDIO_PLAYBACK;
  if (d->platform) {
    if (ccord_platform_notifications_available(d->platform)) caps |= CCORD_UI_CAP_NOTIFICATIONS;
    if (ccord_platform_tray_available(d->platform)) caps |= CCORD_UI_CAP_TRAY;
    if (ccord_platform_global_shortcuts_available(d->platform)) caps |= CCORD_UI_CAP_GLOBAL_SHORTCUTS;
  }
  d->dbg.capabilities = caps;
  return caps;
}

static const ccord_global_shortcut k_shortcuts[] = {
  {"toggle-mute", "Toggle microphone mute", "CTRL+SHIFT+M"},
  {"toggle-deafen", "Toggle deafen", "CTRL+SHIFT+D"},
  {CCORD_SHORTCUT_PUSH_TO_TALK, "Push to talk", nullptr},
};

// The push-to-talk trigger worth offering the portal: the bound key, only while PTT mode is on (a bare key
// is never grabbed globally while voice activity mode is selected).
static void ptt_wanted_trigger(const ccord_app_desktop *d, char *out, size_t cap)
{
  out[0] = '\0';
  if (!d->ui) return;
  ccord_ui_voice_prefs vp;
  ccord_ui_voice_settings(d->ui, &vp);
  if (!vp.ptt) return;
  ccord_ui_keybind kb[CCORD_UI_KEYBIND_COUNT];
  ccord_ui_get_keybinds(d->ui, kb);
  ccord_ui_keybind_trigger(&kb[CCORD_UI_KEYBIND_PTT], out, cap);
}

bool ccord_app_desktop_bind_shortcuts(ccord_app_desktop *d)
{
  if (!d) return false;
  // Preferred triggers follow the user's rebindable keybinds (Settings > Keybinds); the portal
  // lets the user override them anyway. Without an attached UI the defaults above are used.
  ccord_global_shortcut binds[19];
  size_t nbinds = 3;
  binds[0] = k_shortcuts[0];
  binds[1] = k_shortcuts[1];
  binds[2] = k_shortcuts[2];
  char mute_trigger[48], deafen_trigger[48], ptt_trigger[48];
  char plugin_ids[16][128];
  char plugin_descs[16][128];
  if (d->ui) {
    ccord_ui_keybind kb[CCORD_UI_KEYBIND_COUNT];
    ccord_ui_get_keybinds(d->ui, kb);
    ccord_ui_keybind_trigger(&kb[CCORD_UI_KEYBIND_MUTE], mute_trigger, sizeof mute_trigger);
    ccord_ui_keybind_trigger(&kb[CCORD_UI_KEYBIND_DEAFEN], deafen_trigger, sizeof deafen_trigger);
    if (mute_trigger[0]) binds[0].preferred_trigger = mute_trigger;
    if (deafen_trigger[0]) binds[1].preferred_trigger = deafen_trigger;
  }
  ptt_wanted_trigger(d, ptt_trigger, sizeof ptt_trigger);
  if (ptt_trigger[0]) binds[2].preferred_trigger = ptt_trigger;
  memcpy(d->ptt_trigger, ptt_trigger, sizeof d->ptt_trigger);
  if (d->contrib) {
    size_t np = ccord_contrib_keybind_count(d->contrib);
    size_t pi = 0;
    for (size_t i = 0; i < np && pi < 16u; i++) {
      const ccord_keybind_contribution *kb = ccord_contrib_keybind_at(d->contrib, i);
      if (!kb || !kb->is_global || !kb->owner || !kb->bind_id) continue;
      snprintf(plugin_ids[pi], sizeof plugin_ids[0], "plugin:%s:%s", kb->owner, kb->bind_id);
      snprintf(plugin_descs[pi], sizeof plugin_descs[0], "%s (%s)",
               (kb->label && kb->label[0]) ? kb->label : kb->bind_id, kb->owner);
      binds[nbinds++] = (ccord_global_shortcut){
        .id = plugin_ids[pi],
        .description = plugin_descs[pi],
        .preferred_trigger = (kb->current_combo && kb->current_combo[0]) ? kb->current_combo : nullptr,
      };
      pi++;
    }
  }
  bool ok = d->platform && ccord_platform_bind_global_shortcuts(d->platform, binds, nbinds);
  d->dbg.shortcuts_bound = ok;
  return ok;
}

void ccord_app_desktop_sync_ptt_shortcut(ccord_app_desktop *d)
{
  if (!d || !d->dbg.shortcuts_bound) return;
  char want[48];
  ptt_wanted_trigger(d, want, sizeof want);
  if (strcmp(want, d->ptt_trigger) != 0) (void)ccord_app_desktop_bind_shortcuts(d);
}

// ---------------------------------------------------------------- tray badge

static uint32_t badge_sum(uint32_t a, uint32_t b)
{
  uint64_t sum = (uint64_t)a + b;
  return sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

static int64_t now_unix_ms_wall(void)
{
  return ccord_clock_unix_ms();
}

static void badge_add_guild(const ccord_store *s, ccord_id guild_id, int64_t now_unix_ms, uint32_t *unread,
                            uint32_t *mentions)
{
  ccord_id self = ccord_store_self_id(s);
  bool base_computed = false;
  bool have_base = false;
  ccord_store_guild_perm_base perm_base = {0};
  bool guild_muted = ccord_store_guild_muted(s, guild_id, now_unix_ms);
  size_t channels = ccord_store_channel_count(s, guild_id);
  for (size_t i = 0; i < channels; i++) {
    const ccord_channel *ch = ccord_store_channel_at(s, guild_id, i);
    if (!ch || ch->is_message_request || ch->is_spam) continue;
    if (ch->unread_count == 0 && ch->mention_count == 0) continue;
    if (guild_id != CCORD_DM_GUILD_ID) {
      if (!base_computed) {
        base_computed = true;
        have_base = ccord_store_guild_base_permissions(s, guild_id, self, now_unix_ms, &perm_base);
      }
      if (have_base && perm_base.exact &&
          (ccord_store_channel_permissions_with_base(s, &perm_base, ch) & CCORD_PERM_VIEW_CHANNEL) == 0) {
        continue;
      }
    }
    if (ch->unread_count > 0 && !guild_muted && !ccord_store_channel_is_muted(s, ch, now_unix_ms)) {
      *unread = badge_sum(*unread, ch->unread_count);
    }
    *mentions = badge_sum(*mentions, ch->mention_count);
  }
}

// Totals over every guild's channels plus the synthetic DM guild (not part of guild_count). tray_count is the number
// the tray icon shows: every mention plus the unread DM messages that are not already counted as DM mentions.
static void badge_totals(const ccord_store *s, int64_t now_unix_ms, uint32_t *out_unread, uint32_t *out_mentions,
                         uint32_t *out_tray)
{
  uint32_t unread = 0, mentions = 0;
  size_t guilds = ccord_store_guild_count(s);
  for (size_t i = 0; i < guilds; i++) {
    const ccord_guild *g = ccord_store_guild_at(s, i);
    if (g) badge_add_guild(s, g->id, now_unix_ms, &unread, &mentions);
  }
  uint32_t dm_unread_extra = 0;   // unread DM messages beyond the mentions already counted for that DM
  bool dm_guild_muted = ccord_store_guild_muted(s, CCORD_DM_GUILD_ID, now_unix_ms);
  size_t dms = ccord_store_channel_count(s, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < dms; i++) {
    const ccord_channel *ch = ccord_store_channel_at(s, CCORD_DM_GUILD_ID, i);
    if (!ch || ch->is_message_request || ch->is_spam) continue;
    if (ch->unread_count == 0 && ch->mention_count == 0) continue;
    if (!ccord_store_channel_is_muted(s, ch, now_unix_ms)) {
      if (ch->unread_count > ch->mention_count) {
        dm_unread_extra = badge_sum(dm_unread_extra, ch->unread_count - ch->mention_count);
      }
      if (!dm_guild_muted && ch->unread_count > 0) {
        unread = badge_sum(unread, ch->unread_count);
      }
    }
    mentions = badge_sum(mentions, ch->mention_count);
  }
  *out_unread = unread;
  *out_mentions = mentions;
  *out_tray = badge_sum(mentions, dm_unread_extra);
}

void ccord_app_desktop_sync_badge(ccord_app_desktop *d)
{
  if (!d || !d->store || !d->platform) return;
  if (ccord_platform_is_mobile(d->platform)) return;   // mobile has no desktop title bar or SNI tray badge
  uint64_t revision = ccord_store_badge_revision(d->store);
  if (d->badge_valid && revision == d->badge_revision) return;
  d->badge_revision = revision;
  d->badge_valid = true;
  uint32_t unread = 0, mentions = 0, tray = 0;
  badge_totals(d->store, now_unix_ms_wall(), &unread, &mentions, &tray);
  d->dbg.badge_computations++;
  if (d->badge_pushed && unread == d->badge_last_unread && mentions == d->badge_last_mentions &&
      tray == d->badge_last_tray) {
    return;
  }
  d->badge_pushed = true;
  d->badge_last_unread = unread;
  d->badge_last_mentions = mentions;
  d->badge_last_tray = tray;
  ccord_platform_set_badge_counts(d->platform, unread, mentions, tray);
}

// ---------------------------------------------------------------- notifications

[[nodiscard]] bool ccord_app_desktop_notify_eligible(const ccord_notification *n,
                                                     const ccord_app_notify_prefs *prefs,
                                                     const ccord_store *store, int64_t now_unix_ms)
{
  if (!n || !prefs) return false;
  if (!prefs->notify_desktop) return false;                       // Settings > Notifications off
  if (prefs->focused) return false;                               // the user is already looking at it
  if (prefs->self_id != CCORD_ID_NONE && n->author_id == prefs->self_id) return false;
  if (prefs->streamer_mode && !prefs->stream_notifications_enabled) return false;
  if (store && n->author_id != CCORD_ID_NONE &&
      ccord_store_relationship(store, n->author_id) == CCORD_REL_BLOCKED) {
    return false;                                                 // a blocked author never notifies
  }
  if (store) {
    if (n->guild_id != CCORD_ID_NONE && n->guild_id != CCORD_DM_GUILD_ID &&
        ccord_store_guild_muted(store, n->guild_id, now_unix_ms)) {
      return false;
    }
    if (n->channel_id != CCORD_ID_NONE && ccord_store_channel_muted(store, n->channel_id, now_unix_ms)) return false;
  }
  return true;
}

// 150 ms 880 Hz sine with a 25 ms fade-out, two interleaved channels, from a fixed phase table.
#define APP_BEEP_RATE 48000
#define APP_BEEP_FRAMES (APP_BEEP_RATE * 150 / 1000)
#define APP_BEEP_FADE 1200

static const int16_t k_sine[64] = {
  0, 803, 1598, 2378, 3135, 3862, 4551, 5197, 5793, 6333, 6811, 7225, 7568, 7839, 8035, 8153,
  8192, 8153, 8035, 7839, 7568, 7225, 6811, 6333, 5793, 5197, 4551, 3862, 3135, 2378, 1598, 803,
  0, -803, -1598, -2378, -3135, -3862, -4551, -5197, -5793, -6333, -6811, -7225, -7568, -7839, -8035, -8153,
  -8192, -8153, -8035, -7839, -7568, -7225, -6811, -6333, -5793, -5197, -4551, -3862, -3135, -2378, -1598, -803,
};

static bool app_beep_tone(ccord_app_desktop *d)
{
  if (d->beep_pcm) return true;
  int16_t *pcm = ccord_calloc(APP_BEEP_FRAMES, sizeof *pcm);
  if (!pcm) return false;
  uint32_t phase = 0;                              // 16.16 fixed point over the 64-entry table
  const uint32_t step = (uint32_t)((880u * 64u * 65536u) / APP_BEEP_RATE);
  for (size_t i = 0; i < APP_BEEP_FRAMES; i++) {
    int32_t sample = k_sine[(phase >> 16) & 63u];
    size_t left = APP_BEEP_FRAMES - i;
    if (left < APP_BEEP_FADE) sample = sample * (int32_t)left / APP_BEEP_FADE;
    pcm[i] = (int16_t)sample;
    phase += step;
  }
  d->beep_pcm = pcm;
  d->beep_frames = APP_BEEP_FRAMES;
  return true;
}

static void app_beep(ccord_app_desktop *d, bool sounds_on, const ccord_notification *n, int64_t now_unix_ms)
{
  if (!d || !sounds_on) return;
  d->dbg.beep_requests++;
  if (d->sound_hook && d->sound_hook(n, d->ui, d->store, now_unix_ms)) return;   // the message/mention earcon
  d->dbg.tone_requests++;
  ccord_audio *a = app_playback(d);
  if (!ccord_audio_available(a) || !app_beep_tone(d)) return;   // honest silent no-op without a device
  (void)ccord_audio_write(a, d->beep_pcm, d->beep_frames);
  int64_t base = d->playback_busy_until_ms > now_unix_ms ? d->playback_busy_until_ms : now_unix_ms;
  d->playback_busy_until_ms = base + 150;
}

static void notify_prefs_now(const ccord_app_desktop *d, ccord_app_notify_prefs *out, int64_t now_unix_ms)
{
  (void)now_unix_ms;
  memset(out, 0, sizeof *out);
  ccord_ui_settings_debug sd;
  memset(&sd, 0, sizeof sd);
  if (d->ui) {
    ccord_ui_get_settings_debug(d->ui, &sd);
    out->notify_desktop = sd.notify_desktop;
    out->notify_sounds = sd.notify_sounds;
    out->streamer_mode = sd.streamer_mode;
  } else {
    out->notify_desktop = true;   // no UI yet (startup): don't invent a suppression
  }
  const ccord_user_settings *us = d->store ? ccord_store_user_settings(d->store) : nullptr;
  out->stream_notifications_enabled = us ? us->stream_notifications_enabled : false;
  // Streamer Mode per-option toggles: Disable Sounds silences the notification beep; turning
  // Disable Notifications off lifts the streamer suppression (the governed "notify while
  // streaming" override keeps working when it is on).
  if (out->streamer_mode) {
    if (sd.streamer_disable_sounds) out->notify_sounds = false;
    if (!sd.streamer_disable_notifications) out->streamer_mode = false;
  }
  out->focused = d->platform ? ccord_platform_window_focused(d->platform) : false;
  out->self_id = d->store ? ccord_store_self_id(d->store) : CCORD_ID_NONE;
}

ccord_platform *ccord_app_desktop_platform(const ccord_app_desktop *d) { return d ? d->platform : nullptr; }

void ccord_app_desktop_set_sound_hook(ccord_app_desktop *d, ccord_app_desktop_sound_fn fn)
{
  if (d) d->sound_hook = fn;
}

void ccord_app_desktop_set_wake_hook(ccord_app_desktop *d, ccord_app_desktop_wake_fn fn, void *ud)
{
  if (!d) return;
  d->wake_hook = fn;
  d->wake_ud = ud;
}

static void desktop_track_notification(ccord_app_desktop *d, ccord_id channel_id, ccord_id message_id)
{
  if (!d || channel_id == CCORD_ID_NONE) return;
  const ccord_channel *ch = d->store ? ccord_store_channel(d->store, channel_id) : nullptr;
  app_tracked_notify entry = {
    .channel_id = channel_id,
    .message_id = message_id,
    .initial_last_read_id = ch ? ch->last_read_id : CCORD_ID_NONE,
    .seq = ++d->tracked_notif_seq,
  };
  if (d->tracked_notif_count < APP_TRACKED_NOTIFY_MAX) {
    d->tracked_notifs[d->tracked_notif_count++] = entry;
  } else {
    size_t oldest = 0;
    for (size_t i = 1; i < APP_TRACKED_NOTIFY_MAX; i++) {
      if (d->tracked_notifs[i].seq < d->tracked_notifs[oldest].seq) oldest = i;
    }
    d->tracked_notifs[oldest] = entry;
  }
  d->notif_sync_valid = false;
}

void ccord_app_desktop_sync_notifications(ccord_app_desktop *d)
{
  if (!d || !d->platform || !d->store) return;
  if (d->tracked_notif_count == 0 && ccord_platform_notify_map_count(d->platform) == 0) return;

  // Rule 1: while the window is focused and actively reading the selected channel, dismiss all
  // active OS notifications belonging to that channel (e.g. restored from minimized/unfocused).
  bool focused = ccord_platform_window_focused(d->platform);
  bool reading = focused && (!d->ui || ccord_ui_is_reading(d->ui));
  if (reading) {
    ccord_id sel = ccord_store_selected_channel(d->store);
    if (sel != CCORD_ID_NONE) {
      size_t closed = ccord_platform_close_channel_notifications(d->platform, sel, 0);
      if (closed > 0) d->dbg.notifications_closed += closed;
      size_t w = 0;
      for (size_t i = 0; i < d->tracked_notif_count; i++) {
        if (d->tracked_notifs[i].channel_id != sel) d->tracked_notifs[w++] = d->tracked_notifs[i];
      }
      d->tracked_notif_count = w;
    }
  }

  // Rule 2: if a tracked channel's read position advanced past the notified message (Mark as Read
  // or read on another client) or the channel disappeared, dismiss those notifications even while unfocused.
  if (d->tracked_notif_count > 0) {
    uint64_t rev = ccord_store_revision(d->store);
    if (!d->notif_sync_valid || rev != d->notif_sync_rev) {
      d->notif_sync_rev = rev;
      d->notif_sync_valid = true;
      size_t w = 0;
      for (size_t i = 0; i < d->tracked_notif_count; i++) {
        app_tracked_notify e = d->tracked_notifs[i];
        const ccord_channel *ch = ccord_store_channel(d->store, e.channel_id);
        if (!ch) {
          size_t closed = ccord_platform_close_channel_notifications(d->platform, e.channel_id, 0);
          if (closed > 0) d->dbg.notifications_closed += closed;
          continue;
        }
        if (ch->last_read_id != CCORD_ID_NONE && ch->last_read_id > e.initial_last_read_id &&
            (e.message_id == CCORD_ID_NONE || ch->last_read_id >= e.message_id)) {
          size_t closed = ccord_platform_close_channel_notifications(d->platform, e.channel_id, ch->last_read_id);
          if (closed > 0) d->dbg.notifications_closed += closed;
          continue;
        }
        d->tracked_notifs[w++] = e;
      }
      d->tracked_notif_count = w;
    }
  }
}

bool ccord_app_desktop_deliver(ccord_app_desktop *d, const ccord_notification *n, int64_t now_unix_ms)
{
  if (!d || !n) return false;
  ccord_app_notify_prefs prefs;
  notify_prefs_now(d, &prefs, now_unix_ms);
  if (!ccord_app_desktop_notify_eligible(n, &prefs, d->store, now_unix_ms)) {
    // Even if local desktop popups are disabled on the relay PC, still emit Companion Wake
    // when the desktop window is unfocused and the message is otherwise eligible.
    if (d->wake_hook && !prefs.notify_desktop && !prefs.focused && d->platform &&
        !ccord_platform_is_mobile(d->platform)) {
      ccord_app_notify_prefs wp = prefs;
      wp.notify_desktop = true;
      if (ccord_app_desktop_notify_eligible(n, &wp, d->store, now_unix_ms)) {
        d->wake_hook(d->wake_ud, false);
      }
    }
    d->dbg.notifications_gated++;
    d->dbg.notifications_suppressed++;
    return false;
  }
  char title[128], body[256];
  if (!d->ui || !ccord_ui_prepare_notification(d->ui, n, title, sizeof title, body, sizeof body)) {
    d->dbg.notifications_suppressed++;
    return false;
  }
  bool hook_silent = false;
  if (d->contrib) {
    char htitle[128] = {0}, hbody[256] = {0};
    if (!ccord_contrib_run_notification_hook(d->contrib, n->guild_id, n->channel_id, n->message_id, n->author_id,
                                             body, n->is_mention, n->guild_id == CCORD_DM_GUILD_ID,
                                             htitle, sizeof htitle, hbody, sizeof hbody, &hook_silent)) {
      d->dbg.notifications_suppressed++;
      return false;
    }
    if (htitle[0]) snprintf(title, sizeof title, "%s", htitle);
    if (hbody[0]) snprintf(body, sizeof body, "%s", hbody);
  }
  // Emit Companion Wake before local OS notification delivery so missing desktop notification daemons
  // still wake the user's mobile companion.
  if (d->wake_hook && !prefs.focused && d->platform && !ccord_platform_is_mobile(d->platform)) {
    d->wake_hook(d->wake_ud, false);
  }
  // The notification's default action (a click) opens this message, see ccord_app_desktop_notify_click.
  ccord_notify_target target = {.guild_id = n->guild_id, .channel_id = n->channel_id, .message_id = n->message_id};
  if (!d->platform || !ccord_platform_notify_message(d->platform, title, body, &target)) {
    d->dbg.notifications_suppressed++;
    return false;
  }
  desktop_track_notification(d, n->channel_id, n->message_id);
  d->dbg.notifications_delivered++;
  // On mobile while backgrounded, the OS notification channel plays the native alert sound/vibration
  // and respects system DND/vibrate mode without opening an SDL audio playback stream.
  bool os_handles_sound = d->platform && ccord_platform_is_mobile(d->platform) &&
                          ccord_platform_in_background(d->platform);
  app_beep(d, prefs.notify_sounds && !hook_silent && !os_handles_sound, n, now_unix_ms);
  return true;
}

void ccord_app_desktop_notify_click(void *ud, const ccord_notify_target *target)
{
  ccord_app_desktop *d = ud;
  if (!d || !target) return;
  ccord_app_desktop_handle_show(d->platform);   // restore from the tray / minimize and raise
  if (d->ui) ccord_ui_open_message(d->ui, target->guild_id, target->channel_id, target->message_id);
  d->redraw = true;
}

// Pushes the local mute/deafen state to the tray (menu checkmarks + icon overlay) when it changed.
static void desktop_sync_voice_state(ccord_app_desktop *d)
{
  if (!d->platform || !d->store) return;
  if (ccord_platform_is_mobile(d->platform)) return;   // mobile has no desktop tray voice indicator
  uint64_t rev = ccord_store_lists_revision(d->store);
  if (d->voice_sync_valid && rev == d->voice_sync_rev) return;
  d->voice_sync_rev = rev;
  bool muted = false, deafened = false;
  ccord_id vch = ccord_store_active_voice_channel(d->store);
  ccord_id self_id = ccord_store_self_id(d->store);
  if (vch != CCORD_ID_NONE && self_id != CCORD_ID_NONE) {
    const ccord_channel *ch = ccord_store_channel(d->store, vch);
    ccord_id guild = ch ? ch->guild_id : ccord_store_selected_guild(d->store);
    const ccord_voice_state *vs = ccord_store_voice_user(d->store, guild, self_id);
    if (vs) {
      muted = vs->self_mute || vs->self_deaf;   // deafen implies mute on the tray checkmark
      deafened = vs->self_deaf;
    }
  }
  if (d->voice_sync_valid && muted == d->dbg.last_voice_muted && deafened == d->dbg.last_voice_deafened) return;
  d->voice_sync_valid = true;
  d->dbg.last_voice_muted = muted;
  d->dbg.last_voice_deafened = deafened;
  d->dbg.voice_state_pushes++;
  ccord_platform_set_voice_state(d->platform, muted, deafened);
}

void ccord_app_desktop_pump(ccord_app_desktop *d, bool virtual_clock, int64_t now_unix_ms)
{
  if (!d) return;
  if (d->playback && now_unix_ms >= d->playback_busy_until_ms + APP_PLAYBACK_IDLE_MS) {
    ccord_audio_destroy(d->playback);
    d->playback = nullptr;
    d->playback_busy_until_ms = 0;
    ccord_free(d->beep_pcm);
    d->beep_pcm = nullptr;
    d->beep_frames = 0;
  }
  if (d->session) {
    ccord_notification notif;
    while (ccord_session_pop_notification(d->session, &notif)) {
      if (!virtual_clock) (void)ccord_app_desktop_deliver(d, &notif, now_unix_ms);
    }
  }
  ccord_app_desktop_sync_badge(d);
  ccord_app_desktop_sync_notifications(d);
  desktop_sync_voice_state(d);
}

uint32_t ccord_app_desktop_next_wait_ms(const ccord_app_desktop *d, int64_t now_unix_ms)
{
  if (!d || !d->playback) return UINT32_MAX;
  int64_t close_at = d->playback_busy_until_ms + APP_PLAYBACK_IDLE_MS;
  if (now_unix_ms >= close_at) return 0u;
  int64_t rem = close_at - now_unix_ms;
  return rem > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)rem;
}

// ---------------------------------------------------------------- global shortcuts

static ccord_id self_voice_guild(const ccord_app_desktop *d, ccord_id channel_id)
{
  const ccord_channel *ch = d->store ? ccord_store_channel(d->store, channel_id) : nullptr;
  return ch ? ch->guild_id : (d->store ? ccord_store_selected_guild(d->store) : CCORD_ID_NONE);
}

bool ccord_app_desktop_handle_hotkey(ccord_app_desktop *d, const char *shortcut_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!d || !shortcut_id) return false;
  if (strncmp(shortcut_id, "plugin:", 7) == 0 && d->contrib) {
    const char *rest = shortcut_id + 7;
    const char *col = strchr(rest, ':');
    if (col && col > rest && col[1]) {
      char owner[65], bind_id[65];
      size_t olen = (size_t)(col - rest);
      if (olen >= sizeof owner) olen = sizeof owner - 1u;
      memcpy(owner, rest, olen);
      owner[olen] = '\0';
      snprintf(bind_id, sizeof bind_id, "%s", col + 1);
      return ccord_contrib_trigger_keybind(d->contrib, owner, bind_id);
    }
    return false;
  }
  if (!d->session || !d->store) return false;
  bool mute = strcmp(shortcut_id, "toggle-mute") == 0;
  bool deafen = strcmp(shortcut_id, "toggle-deafen") == 0;
  if (!mute && !deafen) return false;
  ccord_id vch = ccord_store_active_voice_channel(d->store);
  if (vch == CCORD_ID_NONE) return false;   // not in voice: an honest no-op
  ccord_id self_id = ccord_store_self_id(d->store);
  if (self_id == CCORD_ID_NONE) return false;
  const ccord_voice_state *vs = ccord_store_voice_user(d->store, self_voice_guild(d, vch), self_id);
  bool on = vs && (mute ? vs->self_mute : vs->self_deaf);
  ccord_status st = mute ? ccord_session_set_voice_mute(d->session, !on, err)
                         : ccord_session_set_voice_deafen(d->session, !on, err);
  return st == CCORD_OK;
}

bool ccord_app_desktop_take_redraw(ccord_app_desktop *d)
{
  if (!d || !d->redraw) return false;
  d->redraw = false;
  return true;
}

void ccord_app_desktop_get_debug(const ccord_app_desktop *d, ccord_app_desktop_debug *out)
{
  if (!out) return;
  *out = d ? d->dbg : (ccord_app_desktop_debug){0};
}

// ---------------------------------------------------------------- link events (resume / network change)

// ud is the session. A transport without the hook answers UNSUPPORTED (idle, signed-out): that is expected, not an error.
static void app_link_cb(void *ud, ccord_link_event ev)
{
  ccord_session *s = ud;
  if (!s) return;
  ccord_error err;
  ccord_status st = ccord_session_link_event(s, ev, &err);
  if (st != CCORD_OK && st != CCORD_ERR_UNSUPPORTED) {
    const char *ename = ev == CCORD_LINK_RESUMED ? "resume"
                        : ev == CCORD_LINK_OFFLINE ? "offline"
                        : ev == CCORD_LINK_ONLINE ? "online"
                        : ev == CCORD_LINK_PROBE ? "probe"
                                                 : "network-change";
    CCORD_LOG_D("link: %s event not applied: %s", ename, err.message);
  }
}

void ccord_app_desktop_bind_link(ccord_platform *p, ccord_session *s)
{
  if (!p) return;
  ccord_platform_set_link_callback(p, s ? app_link_cb : nullptr, s);
}
