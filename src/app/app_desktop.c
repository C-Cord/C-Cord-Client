// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// M6 desktop host glue: startup INI prefs, ccord_ui_host callbacks, tray badge, notifications and
// GlobalShortcuts. See app_desktop.h for the contract. Nothing here mutates Discord state: theme,
// zoom, badge and notifications are local; mute/deafen go through ccord_session intents.
#define _GNU_SOURCE
#include "app_desktop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <ccord/core/alloc.h>
#include <ccord/core/ini.h>
#include <ccord/core/log.h>
#include <ccord/core/paths.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>

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
  return !p || (ccord_platform_window_visible(p) && !ccord_platform_window_minimized(p));
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
  char path[PATH_MAX];
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

struct ccord_app_desktop {
  ccord_store *store;
  ccord_session *session;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_theme *theme;
  ccord_ui *ui;
  char ptt_trigger[48];           // push-to-talk trigger last offered to the portal ("" = none)
  char asset_dir[PATH_MAX];
  ccord_theme base_theme;                  // pristine base snapshot; QuickCSS restores it before overlaying
  bool quickcss_enabled;
  char quickcss[256];
  ccord_audio *playback;          // lazy capability probe + notification beep stream (one device)
  ccord_app_desktop_sound_fn sound_hook;   // optional notification sound (earcons); null = legacy tone
  int16_t *beep_pcm;
  size_t beep_frames;
  uint64_t badge_revision;
  bool badge_valid;
  bool voice_sync_valid;          // tray voice state: push only when mute/deafen actually changed
  bool redraw;
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

// Loads a builtin/asset base theme in place and snapshots it for QuickCSS restore.
static void desktop_load_base(ccord_app_desktop *d, const char *builtin_id)
{
  const char *id = theme_id_ok(builtin_id) ? builtin_id : CCORD_APP_THEME_FALLBACK;
  ccord_error err;
  bool loaded = false;
  if (d->asset_dir[0]) {
    char rel[128], path[PATH_MAX];
    snprintf(rel, sizeof rel, "themes/%s.theme", id);
    if (ccord_paths_join(path, sizeof path, d->asset_dir, rel, &err) == CCORD_OK &&
        ccord_theme_load(d->theme, path, &err) == CCORD_OK) {
      loaded = true;
    }
  }
  if (!loaded && ccord_theme_load_builtin(d->theme, id, &err) == CCORD_OK) loaded = true;
  if (!loaded && strcmp(id, CCORD_APP_THEME_FALLBACK) != 0) {
    (void)ccord_theme_load_builtin(d->theme, CCORD_APP_THEME_FALLBACK, &err);
  }
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

void ccord_app_desktop_apply_theme(void *ud, const char *builtin_id)
{
  ccord_app_desktop *d = ud;
  if (!d || !d->theme) return;
  desktop_load_base(d, builtin_id);
  desktop_quick_css_overlay(d);   // overrides survive a base theme swap
  d->redraw = true;
}

void ccord_app_desktop_apply_quick_css(void *ud, bool enabled, const char *css)
{
  ccord_app_desktop *d = ud;
  if (!d || !d->theme) return;
  d->quickcss_enabled = enabled;
  snprintf(d->quickcss, sizeof d->quickcss, "%s", css ? css : "");
  *d->theme = d->base_theme;      // restore the pristine base, then overlay live
  desktop_quick_css_overlay(d);
  d->redraw = true;
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

static ccord_audio *app_playback(ccord_app_desktop *d)
{
  if (d->playback) return d->playback;
  ccord_audio_config cfg = {.sample_rate = 48000, .channels = 2, .frame_samples = 960, .capture = false};
  ccord_error err;
  d->playback = ccord_audio_create(&cfg, &err);
  return d->playback;
}

uint32_t ccord_app_desktop_capabilities(void *ud)
{
  ccord_app_desktop *d = ud;
  if (!d) return 0;
  uint32_t caps = 0;
  if (ccord_audio_available(app_playback(d))) caps |= CCORD_UI_CAP_AUDIO_PLAYBACK;
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
  ccord_global_shortcut binds[3] = {k_shortcuts[0], k_shortcuts[1], k_shortcuts[2]};
  char mute_trigger[48], deafen_trigger[48], ptt_trigger[48];
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
  bool ok = d->platform &&
            ccord_platform_bind_global_shortcuts(d->platform, binds, sizeof binds / sizeof binds[0]);
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
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000L);
}

static void badge_add_guild(const ccord_store *s, ccord_id guild_id, int64_t now_unix_ms, uint32_t *unread,
                            uint32_t *mentions)
{
  ccord_id self = ccord_store_self_id(s);
  bool exact = guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s, guild_id, self);
  size_t channels = ccord_store_channel_count(s, guild_id);
  for (size_t i = 0; i < channels; i++) {
    const ccord_channel *ch = ccord_store_channel_at(s, guild_id, i);
    if (!ch || ch->is_message_request || ch->is_spam) continue;
    if (exact && (ccord_store_permissions(s, guild_id, ch->id, self, now_unix_ms) & CCORD_PERM_VIEW_CHANNEL) == 0) {
      continue;
    }
    if (!ccord_store_guild_muted(s, guild_id, now_unix_ms) && !ccord_store_channel_is_muted(s, ch, now_unix_ms)) {
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
  size_t dms = ccord_store_channel_count(s, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < dms; i++) {
    const ccord_channel *ch = ccord_store_channel_at(s, CCORD_DM_GUILD_ID, i);
    if (!ch || ch->is_message_request || ch->is_spam) continue;
    if (ccord_store_channel_is_muted(s, ch, now_unix_ms)) continue;
    if (ch->unread_count > ch->mention_count) {
      dm_unread_extra = badge_sum(dm_unread_extra, ch->unread_count - ch->mention_count);
    }
  }
  badge_add_guild(s, CCORD_DM_GUILD_ID, now_unix_ms, &unread, &mentions);
  *out_unread = unread;
  *out_mentions = mentions;
  *out_tray = badge_sum(mentions, dm_unread_extra);
}

void ccord_app_desktop_sync_badge(ccord_app_desktop *d)
{
  if (!d || !d->store || !d->platform) return;
  uint64_t revision = ccord_store_revision(d->store);
  if (d->badge_valid && revision == d->badge_revision) return;
  d->badge_revision = revision;
  d->badge_valid = true;
  uint32_t unread = 0, mentions = 0, tray = 0;
  badge_totals(d->store, now_unix_ms_wall(), &unread, &mentions, &tray);
  d->dbg.badge_computations++;
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
  int16_t *pcm = ccord_calloc(APP_BEEP_FRAMES * 2u, sizeof *pcm);
  if (!pcm) return false;
  uint32_t phase = 0;                              // 16.16 fixed point over the 64-entry table
  const uint32_t step = (uint32_t)((880u * 64u * 65536u) / APP_BEEP_RATE);
  for (size_t i = 0; i < APP_BEEP_FRAMES; i++) {
    int32_t sample = k_sine[(phase >> 16) & 63u];
    size_t left = APP_BEEP_FRAMES - i;
    if (left < APP_BEEP_FADE) sample = sample * (int32_t)left / APP_BEEP_FADE;
    pcm[i * 2u] = (int16_t)sample;
    pcm[i * 2u + 1u] = (int16_t)sample;
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

bool ccord_app_desktop_deliver(ccord_app_desktop *d, const ccord_notification *n, int64_t now_unix_ms)
{
  if (!d || !n) return false;
  ccord_app_notify_prefs prefs;
  notify_prefs_now(d, &prefs, now_unix_ms);
  if (!ccord_app_desktop_notify_eligible(n, &prefs, d->store, now_unix_ms)) {
    d->dbg.notifications_gated++;
    d->dbg.notifications_suppressed++;
    return false;
  }
  char title[128], body[256];
  if (!d->ui || !ccord_ui_prepare_notification(d->ui, n, title, sizeof title, body, sizeof body)) {
    d->dbg.notifications_suppressed++;
    return false;
  }
  // The notification's default action (a click) opens this message, see ccord_app_desktop_notify_click.
  ccord_notify_target target = {.guild_id = n->guild_id, .channel_id = n->channel_id, .message_id = n->message_id};
  if (!d->platform || !ccord_platform_notify_message(d->platform, title, body, &target)) {
    d->dbg.notifications_suppressed++;
    return false;
  }
  d->dbg.notifications_delivered++;
  app_beep(d, prefs.notify_sounds, n, now_unix_ms);
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
  if (d->session) {
    ccord_notification notif;
    while (ccord_session_pop_notification(d->session, &notif)) {
      if (!virtual_clock) (void)ccord_app_desktop_deliver(d, &notif, now_unix_ms);
    }
  }
  ccord_app_desktop_sync_badge(d);
  desktop_sync_voice_state(d);
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
  if (!d || !d->session || !d->store || !shortcut_id) return false;
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

// ---------------------------------------------------------------- single-instance control socket

#define IPC_CMD_MAX 512
#define IPC_ACK_MAX 544

struct ccord_app_ipc {
  int fd;                        // non-blocking listener; -1 after destroy
  char path[PATH_MAX];
};

void ccord_app_ipc_resolve_path(char *out, size_t cap, const char *override_path)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (override_path && override_path[0]) {
    snprintf(out, cap, "%s", override_path);
    return;
  }
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime && runtime[0]) {
    snprintf(out, cap, "%s/ccord/control.sock", runtime);
    return;
  }
  const char *tmp = getenv("TMPDIR");
  if (tmp && tmp[0]) {
    struct stat st;
    if (lstat(tmp, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0077) == 0) {
      snprintf(out, cap, "%s/ccord-control-%ld.sock", tmp, (long)getuid());
    }
  }
  // No runtime dir and no TMPDIR: leave the path empty; the caller runs without single-instance IPC.
}

static bool ipc_cmd_valid(const char *cmd)
{
  if (!cmd || !cmd[0]) return false;
  size_t n = strlen(cmd);
  if (n >= IPC_CMD_MAX) return false;
  if (strncmp(cmd, "open-url ", 9) == 0) {
    if (n <= 9) return false;
    for (size_t i = 9; i < n; i++) {
      unsigned char c = (unsigned char)cmd[i];
      if (c <= 0x20u || c >= 0x7Fu) return false;
    }
    return true;
  }
  for (size_t i = 0; i < n; i++) {
    char c = cmd[i];
    if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
  }
  return true;
}

static bool ipc_sockaddr(const char *path, struct sockaddr_un *out)
{
  if (!path || !path[0] || strlen(path) >= sizeof out->sun_path) return false;
  memset(out, 0, sizeof *out);
  out->sun_family = AF_UNIX;
  snprintf(out->sun_path, sizeof out->sun_path, "%s", path);
  return true;
}

// A live listener answers connect(); a stale socket file refuses it.
static bool ipc_probe_alive(const char *path)
{
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr)) return false;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  bool alive = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
  close(fd);
  return alive;
}

ccord_app_ipc *ccord_app_ipc_listen(const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: socket path missing or too long");
    return nullptr;
  }
  // Create the parent directory (e.g. $XDG_RUNTIME_DIR/ccord) private to the user.
  char parent[PATH_MAX];
  snprintf(parent, sizeof parent, "%s", path);
  char *slash = strrchr(parent, '/');
  if (slash && slash != parent) {
    *slash = '\0';
    if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
      (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: cannot create socket directory");
      return nullptr;
    }
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: socket() failed");
    return nullptr;
  }
  (void)fchmod(fd, 0600);
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
  int bind_errno = errno;
  if (rc != 0) {
    if (bind_errno == EADDRINUSE) {
      if (ipc_probe_alive(path)) {
        close(fd);
        (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "another ccord instance is already running");
        return nullptr;
      }
      struct stat st;
      if (lstat(path, &st) == 0 && (S_ISSOCK(st.st_mode) || S_ISREG(st.st_mode)) && !S_ISLNK(st.st_mode) && st.st_uid == geteuid()) {
        (void)unlink(path);   // stale socket from a crashed instance: recover and re-bind
      }
      rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    }
    if (rc != 0) {
      close(fd);
      (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: bind() failed");
      return nullptr;
    }
  }
  (void)chmod(path, 0600);
  if (listen(fd, 4) != 0 || fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
    close(fd);
    (void)unlink(path);
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: listen() failed");
    return nullptr;
  }
  ccord_app_ipc *ipc = ccord_calloc(1, sizeof *ipc);
  if (!ipc) {
    close(fd);
    (void)unlink(path);
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "ipc: out of memory");
    return nullptr;
  }
  ipc->fd = fd;
  snprintf(ipc->path, sizeof ipc->path, "%s", path);
  return ipc;
}

void ccord_app_ipc_destroy(ccord_app_ipc *ipc)
{
  if (!ipc) return;
  if (ipc->fd >= 0) close(ipc->fd);
  if (ipc->path[0]) (void)unlink(ipc->path);
  ccord_free(ipc);
}

const char *ccord_app_ipc_path(const ccord_app_ipc *ipc)
{
  return ipc ? ipc->path : "";
}

bool ccord_app_ipc_poll(ccord_app_ipc *ipc, char *out, size_t cap)
{
  if (!ipc || ipc->fd < 0 || !out || cap == 0) return false;
  // Drain up to 8 pending connections so an empty liveness probe (ipc_probe_alive) never delays
  // a real CLI command ("show", "quit", ...) sitting right behind it in the accept backlog.
  for (int attempt = 0; attempt < 8; attempt++) {
    int cfd = accept(ipc->fd, nullptr, nullptr);   // listener is non-blocking
    if (cfd < 0) return false;
    struct ucred cred = {0};
    socklen_t clen = sizeof cred;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 || cred.uid != geteuid()) {
      close(cfd);
      continue;
    }
    bool ok = false;
    char buf[IPC_CMD_MAX + 2];
    struct pollfd pfd = {.fd = cfd, .events = POLLIN};
    if (poll(&pfd, 1, 15) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      ssize_t n = read(cfd, buf, sizeof buf - 1);
      if (n > 0) {
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        if (ipc_cmd_valid(buf)) {
          snprintf(out, cap, "%s", buf);
          char ack[IPC_ACK_MAX];
          int alen = snprintf(ack, sizeof ack, "ipc_ack=%s\n", buf);
          (void)fcntl(cfd, F_SETFL, O_NONBLOCK);
          ssize_t w = write(cfd, ack, (size_t)alen);   // best effort: the client waits for this ack
          (void)w;
          ok = true;
        }
      }
    }
    close(cfd);
    if (ok) return true;
  }
  return false;
}

static bool ipc_peer_is_ccord(pid_t pid)
{
  if (pid <= 1) return false;
  char path[64], comm[64];
  snprintf(path, sizeof path, "/proc/%ld/comm", (long)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = read(fd, comm, sizeof comm - 1);
  close(fd);
  if (n <= 0) return false;
  while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r')) n--;
  comm[n] = '\0';
  return strcmp(comm, "ccord") == 0 || strcmp(comm, "c-cord") == 0;
}

static bool ipc_peer_is_stale_exe(pid_t peer_pid)
{
  if (peer_pid <= 1 || peer_pid == getpid()) return false;
  char link_path[64], peer_exe[PATH_MAX], self_exe[PATH_MAX];
  snprintf(link_path, sizeof link_path, "/proc/%ld/exe", (long)peer_pid);
  ssize_t pn = readlink(link_path, peer_exe, sizeof peer_exe - 1);
  if (pn <= 0) return false;
  peer_exe[pn] = '\0';
  static const char k_del[] = " (deleted)";
  if ((size_t)pn >= sizeof k_del - 1 &&
      memcmp(peer_exe + pn - (sizeof k_del - 1), k_del, sizeof k_del - 1) == 0) {
    return true;
  }
  ssize_t sn = readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
  if (sn <= 0) return false;
  self_exe[sn] = '\0';
  struct stat peer_st, self_st;
  if (stat(peer_exe, &peer_st) == 0 && stat(self_exe, &self_st) == 0) {
    if (peer_st.st_size != self_st.st_size || peer_st.st_mtime < self_st.st_mtime) return true;
  }
  return false;
}

bool ccord_app_ipc_handoff(const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: invalid socket path");
    return false;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: socket() failed");
    return false;
  }
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    if (errno == ECONNREFUSED || errno == ENOENT) (void)unlink(path);
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_NOT_FOUND, "no running ccord instance on the control socket");
    return false;
  }

  pid_t peer_pid = 0;
  bool same_uid = false;
#ifdef SO_PEERCRED
  struct ucred cred = {0};
  socklen_t clen = sizeof cred;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 && cred.pid > 1) {
    peer_pid = cred.pid;
    same_uid = (cred.uid == getuid());
  }
#endif

  bool stale = same_uid && ipc_peer_is_stale_exe(peer_pid);
  const char *cmd = stale ? "quit" : "show";
  char line[IPC_CMD_MAX + 2];
  int len = snprintf(line, sizeof line, "%s\n", cmd);
  bool ok = false;
  if (write(fd, line, (size_t)len) == len) {
    char ack[IPC_ACK_MAX];
    char expect[IPC_ACK_MAX];
    snprintf(expect, sizeof expect, "ipc_ack=%s", cmd);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
      ssize_t n = read(fd, ack, sizeof ack - 1);
      if (n > 0) {
        ack[n] = '\0';
        ack[strcspn(ack, "\r\n")] = '\0';
        ok = strcmp(ack, expect) == 0;
      }
    }
  }
  close(fd);

  if (!stale && ok) return true;   // healthy current primary surfaced its window

  if (peer_pid > 1 && same_uid) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    for (int i = 0; i < 40 && kill(peer_pid, 0) == 0; i++) (void)nanosleep(&ts, nullptr);
    if (kill(peer_pid, 0) == 0 && ipc_peer_is_ccord(peer_pid)) {
      (void)kill(peer_pid, SIGTERM);
      for (int i = 0; i < 20 && kill(peer_pid, 0) == 0; i++) (void)nanosleep(&ts, nullptr);
    }
  }
  (void)unlink(path);
  (void)ccord_error_set(err, CCORD_ERR_PROTOCOL,
                        stale ? "replaced stale background ccord instance" : "evicted unresponsive ccord instance");
  return false;
}

bool ccord_app_ipc_send(const char *path, const char *cmd, ccord_error *err)
{
  ccord_error_clear(err);
  struct sockaddr_un addr;
  if (!ipc_sockaddr(path, &addr) || !ipc_cmd_valid(cmd)) {
    (void)ccord_error_set(err, CCORD_ERR_INVALID, "ipc: invalid socket path or command");
    return false;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: socket() failed");
    return false;
  }
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_NOT_FOUND, "no running ccord instance on the control socket");
    return false;
  }
  char line[IPC_CMD_MAX + 2];
  int len = snprintf(line, sizeof line, "%s\n", cmd);
  ssize_t w = write(fd, line, (size_t)len);
  if (w != len) {
    close(fd);
    (void)ccord_error_set(err, CCORD_ERR_IO, "ipc: command write failed");
    return false;
  }
  char ack[IPC_ACK_MAX];
  char expect[IPC_ACK_MAX];
  snprintf(expect, sizeof expect, "ipc_ack=%s", cmd);
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  bool ok = false;
  if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
    ssize_t n = read(fd, ack, sizeof ack - 1);
    if (n > 0) {
      ack[n] = '\0';
      ack[strcspn(ack, "\r\n")] = '\0';
      ok = strcmp(ack, expect) == 0;
    }
  }
  close(fd);
  if (!ok) (void)ccord_error_set(err, CCORD_ERR_PROTOCOL, "ipc: no valid ack from the running instance");
  return ok;
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
    CCORD_LOG_D("link: %s event not applied: %s", ev == CCORD_LINK_RESUMED ? "resume" : "network-change", err.message);
  }
}

void ccord_app_desktop_bind_link(ccord_platform *p, ccord_session *s)
{
  if (!p) return;
  ccord_platform_set_link_callback(p, s ? app_link_cb : nullptr, s);
}
