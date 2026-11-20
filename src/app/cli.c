// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/utf8.h>
#include <ccord/core/os.h>
#include <ccord/net/governor.h>
#include <ccord/net/mock_transport.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- argument parsing

void app_usage(FILE *out)
{
  fputs("usage: ccord [options]\n"
        "  --version, -v           print C-Cord version and exit\n"
        "  --help, -h              print this help message and exit\n"
        "  --headless              use the dummy video driver (no display needed)\n"
        "  --mobile                force responsive mobile UI layout\n"
        "  --screenshot PATH       render one frame to PNG and exit\n"
        "  --verify-slice NAME     run end-to-end headless verification for slice NAME (m0, m4c, m4d, m5, m6, live-stream, stream-audio, expressions, messaging, settings-proto, events-stage, rich-presence, membership-screening, components-v2, activities, call-bar, search-view, picker-layout, voice-text-chat, earcons, reply-style, ui-style, panel-toggles, smooth-scroll, motion, media-load, hover-edit, status-card, jump-present, ccord-settings, game-detect, profile-edit, reaction-once, rpc-app-name)\n"
        "  --net-stats             print request-governor totals and per-route counters at exit\n"
        "  --verify-slice streamer-mode\n"
        "                          Streamer Mode masking end to end (needs --mock)\n"
        "  --verify-slice voice-messages\n"
        "                          voice messages end to end (record, Ogg/Opus, upload, flag 8192, waveform player)\n"
        "  --live                  connect to Discord with the saved C-Cord user token\n"
        "  --mock                  force the offline mock transport (default for screenshots)\n"
        "  --login-screen          open the unauthenticated Discord Login Screen over an empty store\n"
        "  --token-file PATH       standalone token file (default $XDG_CONFIG_HOME/ccord/token)\n"
        "  --size WxH              window size in logical px (default 1280x800)\n"
        "  --channel NAME          select the first text channel called NAME after loading\n"
        "  --frames N              frames pumped before the screenshot (default 3)\n"
        "  --hover X,Y             move the pointer\n"
        "  --right-click X,Y       right-click at X,Y (opens a context menu)\n"
        "  --click X,Y             left-click at X,Y after --right-click (activates a menu item)\n"
        "  --print-clipboard       print the clipboard text after the screenshot run\n"
        "  --scroll N              mouse wheel notches over the messages (positive = older)\n"
        "  --type TEXT [--send]    type TEXT into the composer, optionally press Enter\n"
        "  --open-settings         open the settings overlay\n"
        "  --settings-plugins      with --open-settings: select the Plugins tab\n"
        "  --settings-account      with --open-settings: select the Account tab\n"
        "  --toggle-extensions     with --open-settings: click the Extensions switch\n"
        "  --theme PATH            theme file (overrides the theme saved in settings.ini)\n"
        "  --start-minimized       start hidden in the tray (use the tray icon to show the window)\n"
        "  --control-socket PATH   single-instance control socket (default $XDG_RUNTIME_DIR/ccord/control.sock)\n"
        "  --toggle-mic            toggle microphone mute on the running instance and exit\n"
        "  --toggle-deafen         toggle deafen on the running instance and exit\n"
        "  --show                  show the running instance's window and exit\n"
        "  --quit                  quit the running instance and exit\n"
        "  --plugin-dir DIR        extra extension folder (repeatable)\n"
        "  --no-plugins            do not start extensions\n"
        "  --plugin-dev            auto-reload plugins when their files change on disk\n"
        "  --no-rpc                do not open the local Discord RPC socket (Rich Presence)\n"
        "  --no-warm-cache         do not read or write the warm-start cache (settings.ini [ui] warm_cache = 0 does too)\n"
        "  --data-dir DIR          data directory (extension storage)\n"
        "  --fixed-time UNIX_MS    pin \"now\" for timestamps (UTC)\n"
        "  --mock-latency-ms N     delay mock replies\n"
        "  --mock-ambient-ms N     a bot posts into #general every N ms\n"
        "  --log-level LEVEL       debug|info|warn|error\n",
        out);
}

static bool parse_long(const char *s, long lo, long hi, long *out)
{
  if (!s || !*s) return false;
  char *end = nullptr;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno != 0 || *end != '\0' || v < lo || v > hi) return false;
  *out = v;
  return true;
}

static bool parse_pair(const char *s, char sep, long lo, long hi, long *a, long *b)
{
  const char *p = s ? strchr(s, sep) : nullptr;
  if (!p) return false;
  char first[24];
  size_t n = (size_t)(p - s);
  if (n == 0 || n >= sizeof first) return false;
  memcpy(first, s, n);
  first[n] = '\0';
  return parse_long(first, lo, hi, a) && parse_long(p + 1, lo, hi, b);
}

int app_parse_args(int argc, char **argv, options *o)
{
  *o = (options){.width = 1280, .height = 800, .frames = DEFAULT_FRAMES, .log_level = CCORD_LOG_WARN};
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *val = i + 1 < argc ? argv[i + 1] : nullptr;
    long x, y;
#define NEED_VALUE()                                          \
  do {                                                        \
    if (!val) {                                               \
      fprintf(stderr, "ccord: %s needs a value\n", a);        \
      return 2;                                               \
    }                                                         \
    i++;                                                      \
  } while (0)
#define BAD(msg)                                              \
  do {                                                        \
    fprintf(stderr, "ccord: %s: %s\n", a, msg);               \
    return 2;                                                 \
  } while (0)
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      app_usage(stdout);
      exit(0);
    } else if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
      printf("ccord %s\n", CCORD_VERSION_STRING);
      exit(0);
    } else if (strcmp(a, "--headless") == 0) {
      o->headless = true;
    } else if (strcmp(a, "--mobile") == 0) {
      o->mobile = true;
    } else if (strcmp(a, "--live") == 0) {
      o->live = true;
    } else if (strcmp(a, "--mock") == 0) {
      o->mock = true;
    } else if (strcmp(a, "--login-screen") == 0) {
      o->login_screen = true;
    } else if (strcmp(a, "--token-file") == 0) {
      NEED_VALUE();
      o->token_file = val;
    } else if (strcmp(a, "--screenshot") == 0) {
      NEED_VALUE();
      o->screenshot = val;
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "streamer-mode") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "voice-messages") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "gif-favorites") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "embed-video") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0 && i + 1 < argc && strcmp(argv[i + 1], "video-stream") == 0) {
      o->verify_slice = argv[++i];
    } else if (strcmp(a, "--verify-slice") == 0) {
      NEED_VALUE();
      if (strcmp(val, "m0") != 0 && strcmp(val, "m4c") != 0 && strcmp(val, "m4d") != 0 && strcmp(val, "m5") != 0 &&
          strcmp(val, "m6") != 0 && strcmp(val, "live-stream") != 0 && strcmp(val, "stream-audio") != 0 &&
          strcmp(val, "expressions") != 0 &&
          strcmp(val, "messaging") != 0 && strcmp(val, "settings-proto") != 0 && strcmp(val, "events-stage") != 0 && strcmp(val, "rich-presence") != 0 &&
          strcmp(val, "membership-screening") != 0 && strcmp(val, "components-v2") != 0 && strcmp(val, "activities") != 0 &&
          strcmp(val, "call-bar") != 0 && strcmp(val, "search-view") != 0 && strcmp(val, "picker-layout") != 0 &&
          strcmp(val, "voice-text-chat") != 0 && strcmp(val, "earcons") != 0 && strcmp(val, "reply-style") != 0 &&
          strcmp(val, "ui-style") != 0 && strcmp(val, "panel-toggles") != 0 && strcmp(val, "smooth-scroll") != 0 &&
          strcmp(val, "motion") != 0 && strcmp(val, "media-load") != 0 && strcmp(val, "hover-edit") != 0 && strcmp(val, "reaction-once") != 0 &&
          strcmp(val, "status-card") != 0 && strcmp(val, "jump-present") != 0 && strcmp(val, "ccord-settings") != 0 &&
          strcmp(val, "game-detect") != 0 && strcmp(val, "profile-edit") != 0 && strcmp(val, "rpc-app-name") != 0 &&
          strcmp(val, "warm-start") != 0 &&
          strcmp(val, "plus-menu") != 0 &&
          strcmp(val, "poll-create") != 0 &&
          strcmp(val, "schedule-message") != 0 &&
          strcmp(val, "fav-gif-order") != 0 &&
          strcmp(val, "tray-unread") != 0 &&
          strcmp(val, "close-background") != 0 &&
          strcmp(val, "app-icon") != 0 &&
          strcmp(val, "status-sync") != 0 && strcmp(val, "blocked-hide") != 0 && strcmp(val, "ctx-react") != 0 &&
          strcmp(val, "new-messages-bar") != 0 && strcmp(val, "report-flow") != 0 && strcmp(val, "call-ring") != 0 &&
          strcmp(val, "global-ptt") != 0 && strcmp(val, "voice-reconnect") != 0 && strcmp(val, "multi-camera") != 0 &&
          strcmp(val, "notify-jump") != 0 && strcmp(val, "resume-reconnect") != 0 &&
          strcmp(val, "profile-view") != 0 && strcmp(val, "profile-roles") != 0 && strcmp(val, "rail-scroll") != 0 &&
          strcmp(val, "composer-format") != 0 && strcmp(val, "settings-profile") != 0 && strcmp(val, "glyph-fallback") != 0 &&
          strcmp(val, "ansi-block") != 0)
        BAD("unknown slice (supported:m0, m4c, m4d, m5, m6, live-stream, stream-audio, expressions, messaging, settings-proto, events-stage, rich-presence, membership-screening, components-v2, activities, call-bar, search-view, picker-layout, voice-text-chat, earcons, reply-style, ui-style, panel-toggles, smooth-scroll, motion, media-load, hover-edit, status-card, jump-present, ccord-settings, game-detect, profile-edit, reaction-once, rpc-app-name, warm-start, plus-menu, poll-create, schedule-message, fav-gif-order, tray-unread, close-background, app-icon, status-sync, blocked-hide, ctx-react, new-messages-bar, report-flow, call-ring, global-ptt, voice-reconnect, multi-camera, notify-jump, resume-reconnect, profile-view, profile-roles, rail-scroll, composer-format, settings-profile, glyph-fallback, ansi-block)");
      o->verify_slice = val;
    } else if (strcmp(a, "--net-stats") == 0) {
      o->net_stats = true;
    } else if (strcmp(a, "--size") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, 'x', 200, 8192, &x, &y)) BAD("expected WxH between 200 and 8192");
      o->width = (int)x;
      o->height = (int)y;
    } else if (strcmp(a, "--channel") == 0) {
      NEED_VALUE();
      o->channel = val;
    } else if (strcmp(a, "--frames") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 1, 1000, &x)) BAD("expected 1..1000");
      o->frames = (int)x;
    } else if (strcmp(a, "--hover") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, ',', 0, 20000, &x, &y)) BAD("expected X,Y");
      o->has_hover = true;
      o->hover_x = (float)x;
      o->hover_y = (float)y;
    } else if (strcmp(a, "--right-click") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, ',', 0, 20000, &x, &y)) BAD("expected X,Y");
      o->has_right_click = true;
      o->rc_x = (float)x;
      o->rc_y = (float)y;
    } else if (strcmp(a, "--click") == 0) {
      NEED_VALUE();
      if (!parse_pair(val, ',', 0, 20000, &x, &y)) BAD("expected X,Y");
      o->has_click = true;
      o->click_x = (float)x;
      o->click_y = (float)y;
    } else if (strcmp(a, "--print-clipboard") == 0) {
      o->print_clipboard = true;
    } else if (strcmp(a, "--scroll") == 0) {
      NEED_VALUE();
      if (!parse_long(val, -100000, 100000, &x)) BAD("expected an integer");
      o->scroll = (int)x;
    } else if (strcmp(a, "--type") == 0) {
      NEED_VALUE();
      if (!ccord_utf8_valid(val, strlen(val))) BAD("text is not valid UTF-8");
      o->type_text = val;
    } else if (strcmp(a, "--send") == 0) {
      o->send = true;
    } else if (strcmp(a, "--open-settings") == 0) {
      o->open_settings = true;
    } else if (strcmp(a, "--settings-plugins") == 0) {
      o->settings_plugins = true;
    } else if (strcmp(a, "--settings-account") == 0) {
      o->settings_account = true;
    } else if (strcmp(a, "--toggle-extensions") == 0) {
      o->toggle_extensions = true;
    } else if (strcmp(a, "--theme") == 0) {
      NEED_VALUE();
      o->theme_path = val;
    } else if (strcmp(a, "--start-minimized") == 0) {
      o->start_minimized = true;
    } else if (strcmp(a, "--control-socket") == 0) {
      NEED_VALUE();
      o->control_socket = val;
    } else if (strcmp(a, "--toggle-mic") == 0 || strcmp(a, "--toggle-deafen") == 0 ||
               strcmp(a, "--show") == 0 || strcmp(a, "--quit") == 0) {
      if (o->ipc_cmd) BAD("only one instance command (--toggle-mic/--toggle-deafen/--show/--quit) is allowed");
      o->ipc_cmd = strcmp(a, "--toggle-mic") == 0 ? "toggle-mic"
                 : strcmp(a, "--toggle-deafen") == 0 ? "toggle-deafen"
                 : strcmp(a, "--show") == 0 ? "show" : "quit";
    } else if (strcmp(a, "--plugin-dir") == 0) {
      NEED_VALUE();
      if (o->plugin_dir_count >= MAX_PLUGIN_DIRS) BAD("too many --plugin-dir options");
      o->plugin_dirs[o->plugin_dir_count++] = val;
    } else if (strcmp(a, "--no-plugins") == 0) {
      o->no_plugins = true;
    } else if (strcmp(a, "--plugin-dev") == 0) {
      o->plugin_dev = true;
    } else if (strcmp(a, "--no-rpc") == 0) {
      o->no_rpc = true;
    } else if (strcmp(a, "--no-warm-cache") == 0) {
      o->no_warm_cache = true;
    } else if (strcmp(a, "--warm-phase") == 0) {
      NEED_VALUE();
      o->warm_phase = val;
    } else if (strcmp(a, "--data-dir") == 0) {
      NEED_VALUE();
      o->data_dir = val;
    } else if (strcmp(a, "--fixed-time") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 1, LONG_MAX, &x)) BAD("expected a positive integer (unix ms)");
      o->fixed_time = x;
    } else if (strcmp(a, "--mock-latency-ms") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 0, 60000, &x)) BAD("expected 0..60000");
      o->mock_latency_ms = (uint32_t)x;
    } else if (strcmp(a, "--mock-ambient-ms") == 0) {
      NEED_VALUE();
      if (!parse_long(val, 0, 3600000, &x)) BAD("expected 0..3600000");
      o->mock_ambient_ms = (uint32_t)x;
    } else if (strcmp(a, "--log-level") == 0) {
      NEED_VALUE();
      if (strcmp(val, "debug") == 0) o->log_level = CCORD_LOG_DEBUG;
      else if (strcmp(val, "info") == 0) o->log_level = CCORD_LOG_INFO;
      else if (strcmp(val, "warn") == 0) o->log_level = CCORD_LOG_WARN;
      else if (strcmp(val, "error") == 0) o->log_level = CCORD_LOG_ERROR;
      else BAD("expected debug, info, warn or error");
    } else if (strcmp(a, "--open-url") == 0) {
      NEED_VALUE();
      o->open_url = val;
    } else if (a[0] != '-' &&
               (strncmp(a, "https://", 8) == 0 || strncmp(a, "http://", 7) == 0 ||
                strncmp(a, "discord://", 10) == 0 || strncmp(a, "ccord://", 8) == 0 ||
                strncmp(a, "discord.gg/", 11) == 0 || strncmp(a, "discord.com/invite/", 19) == 0)) {
      o->open_url = a;
    } else {
      fprintf(stderr, "ccord: unknown option %s\n", a);
      app_usage(stderr);
      return 2;
    }
#undef NEED_VALUE
#undef BAD
  }
  if (o->send && !o->type_text) {
    fputs("ccord: --send needs --type\n", stderr);
    return 2;
  }
  if (o->warm_phase && (!o->verify_slice || strcmp(o->verify_slice, "warm-start") != 0)) {
    fputs("ccord: --warm-phase only applies to --verify-slice warm-start\n", stderr);
    return 2;
  }
  if (o->live && o->mock) {
    fputs("ccord: --live and --mock are mutually exclusive\n", stderr);
    return 2;
  }
#if !CCORD_LIVE_ACCOUNT
  if (o->live) {
    fputs("ccord: --live: this build has CCORD_LIVE_ACCOUNT=OFF\n", stderr);
    return 2;
  }
#endif
  return 0;
}


// ---------------------------------------------------------------- screenshot mode

static bool plugins_settled(const app *a)
{
  size_t n = ccord_contrib_plugin_count(a->contrib);
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(a->contrib, i);
    if (p && p->state == CCORD_PLUGIN_STARTING) return false;
  }
  return true;
}

#if CCORD_LIVE_ACCOUNT
// Live events arrive on real sockets, so screenshots must wait in real time instead of virtual frames.
static bool wait_live_history(app *a, uint64_t timeout_ms)
{
  uint64_t t0 = ccord_clock_ms();
  for (;;) {
    app_step(a, false);
    ccord_id ch = ccord_store_selected_channel(a->store);
    if (ccord_session_state(a->session) == CCORD_CONN_CONNECTED && ch != CCORD_ID_NONE &&
        ccord_store_history_loaded(a->store, ch)) {
      return true;
    }
    if (ccord_session_state(a->session) == CCORD_CONN_FAILED) return false;
    if (ccord_clock_ms() - t0 >= timeout_ms) return false;
    ccord_sleep_ms(10);
  }
}
#endif

static bool select_channel_by_name(app *a, const char *name)
{
  if (strcmp(name, "@me") == 0 || strcmp(name, "dms") == 0) {
    ccord_error err;
    if (ccord_session_select_guild(a->session, CCORD_DM_GUILD_ID, &err) != CCORD_OK) {
      fprintf(stderr, "ccord: --channel %s: %s\n", name, err.message);
      return false;
    }
    return true;
  }
  size_t ng = ccord_store_guild_count(a->store);
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(a->store, gi);
    size_t n = g ? ccord_store_channel_count(a->store, g->id) : 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(a->store, g->id, i);
      if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, name) == 0) {
        ccord_error err;
        if (ccord_session_select_channel(a->session, c->id, &err) != CCORD_OK) {
          fprintf(stderr, "ccord: --channel %s: %s\n", name, err.message);
          return false;
        }
        return true;
      }
    }
  }
  size_t ndm = ccord_store_channel_count(a->store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < ndm; i++) {
    const ccord_channel *c = ccord_store_channel_at(a->store, CCORD_DM_GUILD_ID, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && strcmp(c->name, name) == 0) {
      ccord_error err;
      if (ccord_session_select_channel(a->session, c->id, &err) != CCORD_OK) {
        fprintf(stderr, "ccord: --channel %s: %s\n", name, err.message);
        return false;
      }
      return true;
    }
  }
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(a->store, gi);
    size_t n = g ? ccord_store_channel_count(a->store, g->id) : 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(a->store, g->id, i);
      if (c && c->kind == CCORD_CHANNEL_VOICE && strcmp(c->name, name) == 0) {
        ccord_error err;
        (void)ccord_session_select_channel(a->session, c->id, &err);
        (void)ccord_session_join_voice(a->session, g->id, c->id, &err);
        return true;
      }
    }
  }
  fprintf(stderr, "ccord: --channel: no text channel named '%s'\n", name);
  return false;
}

int app_run_screenshot(app *a, const options *o)
{
  if (a->live) {
#if CCORD_LIVE_ACCOUNT
    if (!wait_live_history(a, CCORD_LIVE_TIMEOUT_DEFAULT_MS + 15000)) {
      fputs("ccord: live transport did not connect or load history\n", stderr);
      return 1;
    }
#endif
    if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
      fputs("ccord: transport did not connect\n", stderr);
      return 1;
    }
  } else if (a->mock) {
    // Let the mock connect and deliver the snapshot and the first history.
    for (int i = 0; i < 200; i++) {
      a->virtual_ms += VIRTUAL_FRAME_MS;
      app_step(a, false);
      ccord_id ch = ccord_store_selected_channel(a->store);
      if (ch != CCORD_ID_NONE && ccord_store_history_loaded(a->store, ch)) break;
    }
    if (ccord_session_state(a->session) != CCORD_CONN_CONNECTED) {
      fputs("ccord: transport did not connect\n", stderr);
      return 1;
    }
  }
  // Plugins start asynchronously; wait until they are ready or failed (max 2 s of real time).
  uint64_t t0 = ccord_clock_ms();
  while (!plugins_settled(a) && ccord_clock_ms() - t0 < 2000) {
    app_pump(a);
    ccord_sleep_ms(5);
  }
  app_run_frames(a, o->frames);

  if (o->channel) {
    if (!select_channel_by_name(a, o->channel)) return 1;
#if CCORD_LIVE_ACCOUNT
    if (a->live && !wait_live_history(a, CCORD_LIVE_TIMEOUT_DEFAULT_MS + 5000)) {
      fputs("ccord: live transport did not load the requested channel\n", stderr);
      return 1;
    }
#endif
    app_run_frames(a, 3);
  }
  const ccord_layout *lay = ccord_ui_get_layout(a->ui);
  if (o->scroll != 0) {
    ccord_id ch = ccord_store_selected_channel(a->store);
    size_t before_count = ccord_store_message_count(a->store, ch);
    ccord_input_event ev = {.type = CCORD_IN_WHEEL, .wheel = (float)o->scroll,
                            .x = lay->message_area.x + lay->message_area.w * 0.5f, .y = lay->message_area.y + lay->message_area.h * 0.5f};
    app_inject(a, &ev);
    app_run_frames(a, 4);
#if CCORD_LIVE_ACCOUNT
    if (a->live && o->scroll > 0 && ch != CCORD_ID_NONE && ccord_session_history_has_more(a->session, ch)) {
      uint64_t st0 = ccord_clock_ms();
      while (ccord_store_message_count(a->store, ch) == before_count && ccord_clock_ms() - st0 < 5000) {
        app_step(a, false);
        ccord_sleep_ms(10);
      }
      app_run_frames(a, 2);
    }
#else
    (void)before_count;
#endif
  }
  if (o->type_text) {
    app_inject_text(a, o->type_text);
    app_run_frames(a, 1);
    if (o->send) {
      ccord_input_event ev = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_RETURN};
      app_inject(a, &ev);
      app_run_frames(a, 3);
    }
  }
  if (o->open_settings) {
    if (ccord_ui_is_mobile_layout(a->ui)) {
      ccord_ui_set_left_drawer(a->ui, true);
      app_run_frames(a, 4);
      lay = ccord_ui_get_layout(a->ui);
    }
    app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, lay->user_panel.x + lay->user_panel.w - 24.0f, lay->user_panel.y + lay->user_panel.h * 0.5f);
    app_run_frames(a, 2);
  }
  if (o->settings_plugins || o->settings_account || o->toggle_extensions) {
    ccord_ui_debug dbg;
    ccord_ui_get_debug(a->ui, &dbg);
    if (o->settings_account) {
      app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, dbg.settings_account_tab.x + 10.0f, dbg.settings_account_tab.y + 10.0f);
      app_run_frames(a, 2);
    }
    if (o->settings_plugins) {
      app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, dbg.settings_plugins_tab.x + 10.0f, dbg.settings_plugins_tab.y + 10.0f);
      app_run_frames(a, 2);
    }
    if (o->toggle_extensions) {
      ccord_ui_get_debug(a->ui, &dbg);
      app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, dbg.extensions_toggle.x + dbg.extensions_toggle.w - 20.0f,
                   dbg.extensions_toggle.y + dbg.extensions_toggle.h * 0.5f);
      app_run_frames(a, 2);
      ccord_ui_get_debug(a->ui, &dbg);
      printf("extensions: %s\n", dbg.extensions_on ? "on" : "off");
    }
  }
  if (o->has_hover) {
    app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, o->hover_x, o->hover_y);
    app_run_frames(a, 3);
  }
  if (o->has_right_click) {
    app_inject_mouse(a, CCORD_IN_MOUSE_MOVE, 0, o->rc_x, o->rc_y);
    app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 3, o->rc_x, o->rc_y);
    app_run_frames(a, 3);
  }
  if (o->has_click) {
    // Plugin actions run in the host process: pump until the clipboard write arrives or 2 s pass.
    uint64_t click_t0 = ccord_clock_ms();
    app_inject_mouse(a, CCORD_IN_MOUSE_DOWN, 1, o->click_x, o->click_y);
    app_inject_mouse(a, CCORD_IN_MOUSE_UP, 1, o->click_x, o->click_y);
    while (ccord_clock_ms() - click_t0 < 2000) {
      app_run_frames(a, 1);
      char *text = ccord_platform_clipboard_get(a->platform);
      bool got = text && *text;
      ccord_free(text);
      if (got) break;
      ccord_sleep_ms(5);
    }
  }
  app_run_frames(a, 2);

  // Final frame: draw, capture before present, present.
  a->virtual_ms += VIRTUAL_FRAME_MS;
  app_pump(a);
  (void)ccord_ui_update(a->ui, app_now_ms(a));
  ccord_render_begin(a->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(a->ui, a->renderer);
  ccord_error err;
  ccord_status st = ccord_render_screenshot(a->renderer, o->screenshot, &err);
  ccord_render_end(a->renderer);
  if (st != CCORD_OK) {
    fprintf(stderr, "ccord: %s\n", err.message);
    return 1;
  }
  ccord_ui_debug dbg;
  ccord_ui_get_debug(a->ui, &dbg);
  const ccord_account_view *av = ccord_account_view_of(a->account);
  size_t guilds = av ? av->guild_count : 0;
  if (guilds == 0) guilds = ccord_store_guild_count(a->store);   // account view guild count is 0 until the first live sync
  printf("screenshot: %s (%d x %d logical); messages=%zu visible=%zu..%zu measured=%zu account=%s demo_chip=%d guilds=%zu\n",
         o->screenshot, o->width, o->height, dbg.message_count, dbg.first_visible, dbg.last_visible, dbg.rows_measured,
         av ? ccord_account_mode_name(av->mode) : "unavailable", dbg.demo_chip_visible ? 1 : 0, guilds);
  if (o->print_clipboard) {
    char *text = ccord_platform_clipboard_get(a->platform);
    printf("clipboard: %s\n", text ? text : "");
    ccord_free(text);
  }
  return 0;
}

static void print_net_stats_route(void *ud, const char *route, uint32_t requests, uint32_t rate_limited,
                                  int last_status)
{
  (void)ud;
  printf("net-stats: route=%s requests=%u rate_limited=%u last_status=%d\n", route, requests, rate_limited,
         last_status);
}

static void print_net_stats_gov(const char *name, const ccord_gov *g)
{
  if (!g) return;
  ccord_gov_stats s = ccord_gov_get_stats(g);
  printf("net-stats: governor=%s started_user=%llu started_visible=%llu started_background=%llu waits=%llu "
         "refusals=%llu responses_2xx=%llu responses_4xx=%llu responses_5xx=%llu rate_limited=%llu "
         "network_errors=%llu edge_trips=%llu inflight=%u\n",
         name, (unsigned long long)s.started[CCORD_GOV_USER], (unsigned long long)s.started[CCORD_GOV_VISIBLE],
         (unsigned long long)s.started[CCORD_GOV_BACKGROUND], (unsigned long long)s.waits,
         (unsigned long long)s.refusals, (unsigned long long)s.responses_2xx,
         (unsigned long long)s.responses_4xx, (unsigned long long)s.responses_5xx,
         (unsigned long long)s.rate_limited, (unsigned long long)s.network_errors,
         (unsigned long long)s.edge_trips, s.inflight);
  ccord_gov_foreach_route(g, print_net_stats_route, nullptr);
}

// Prints totals then per-route counters for every governor this run actually exercised.
void app_print_net_stats(const app *a)
{
#if CCORD_LIVE_ACCOUNT
  if (a->live && a->transport && a->session) print_net_stats_gov("live", ccord_live_transport_governor(a->transport));
  if (a->auth_q) print_net_stats_gov("pre-auth", ccord_rest_queue_governor(a->auth_q));
#endif
  print_net_stats_gov("verify", a->verify_gov);
}

