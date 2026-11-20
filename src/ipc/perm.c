// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ipc/perm.h>

#include <string.h>

static const struct {
  ccord_perm perm;
  ccord_perm_class cls;
  const char *name;
  const char *desc;
} k_perms[] = {
  // Class 1 (Safe) — kept for manifest compatibility; always granted
  {CCORD_PERM_EVENTS_NAVIGATION, CCORD_PERM_CLASS_SAFE,   "events.navigation", "Subscribe to guild/channel navigation events"},
  {CCORD_PERM_UI_CONTEXTMENU,    CCORD_PERM_CLASS_SAFE,   "ui.contextMenu",    "Add items to context menus"},
  {CCORD_PERM_CLIPBOARD_WRITE,   CCORD_PERM_CLASS_SAFE,   "clipboard.write",   "Copy text to the clipboard"},
  {CCORD_PERM_STORAGE,           CCORD_PERM_CLASS_SAFE,   "storage",           "Store plugin-scoped preferences"},
  {CCORD_PERM_COMMANDS,          CCORD_PERM_CLASS_SAFE,   "commands",          "Register local slash commands"},

  // Class 2 (Unsafe) — C-Cord internal state, mutations, hooks, UI
  {CCORD_PERM_EVENTS_MESSAGES,   CCORD_PERM_CLASS_UNSAFE, "events.messages",   "Read incoming, edited, and deleted messages"},
  {CCORD_PERM_EVENTS_PRESENCE,   CCORD_PERM_CLASS_UNSAFE, "events.presence",   "Observe user online status and presence changes"},
  {CCORD_PERM_EVENTS_VOICE,      CCORD_PERM_CLASS_UNSAFE, "events.voice",      "Observe voice channel joins, leaves, and streams"},
  {CCORD_PERM_EVENTS_CHANNELS,   CCORD_PERM_CLASS_UNSAFE, "events.channels",   "Observe guild and channel updates"},
  {CCORD_PERM_STORE_READ,        CCORD_PERM_CLASS_UNSAFE, "store.read",        "Read cached guilds, channels, DMs, members, and messages"},
  {CCORD_PERM_MESSAGES_SEND,     CCORD_PERM_CLASS_UNSAFE, "messages.send",     "Send messages as your account"},
  {CCORD_PERM_MESSAGES_MANAGE,   CCORD_PERM_CLASS_UNSAFE, "messages.manage",   "Edit, delete, and react to messages"},
  {CCORD_PERM_MESSAGES_HOOK,     CCORD_PERM_CLASS_UNSAFE, "messages.hook",     "Intercept and modify outgoing messages before send"},
  {CCORD_PERM_PRESENCE_WRITE,    CCORD_PERM_CLASS_UNSAFE, "presence.write",    "Change your online status and Rich Presence"},
  {CCORD_PERM_NAVIGATION_WRITE,  CCORD_PERM_CLASS_UNSAFE, "navigation.write",  "Switch active guilds and channels"},
  {CCORD_PERM_VOICE_CONTROL,     CCORD_PERM_CLASS_UNSAFE, "voice.control",     "Join/leave voice channels and toggle mute/deafen"},
  {CCORD_PERM_UI_PANEL,          CCORD_PERM_CLASS_UNSAFE, "ui.panel",          "Render custom UI panels inside C-Cord"},
  {CCORD_PERM_UI_COMPOSER,       CCORD_PERM_CLASS_UNSAFE, "ui.composer",       "Read and modify the message composer draft and autocomplete"},
  {CCORD_PERM_CLIPBOARD_READ,    CCORD_PERM_CLASS_UNSAFE, "clipboard.read",    "Read text from your clipboard"},
  {CCORD_PERM_GATEWAY_RAW,       CCORD_PERM_CLASS_UNSAFE, "gateway.raw",       "Inspect raw Discord gateway events"},
  {CCORD_PERM_UI_DECORATE,       CCORD_PERM_CLASS_UNSAFE, "ui.decorate",       "Decorate messages, members, profiles, and channels"},
  {CCORD_PERM_THEME_WRITE,       CCORD_PERM_CLASS_UNSAFE, "theme.write",       "Dynamically modify active theme colors, metrics, CSS, fonts, icons, and backgrounds"},
  {CCORD_PERM_AUDIO_EARCONS,     CCORD_PERM_CLASS_UNSAFE, "audio.earcons",     "Override notification and voice earcon sounds"},
  {CCORD_PERM_NOTIFICATIONS_HOOK,CCORD_PERM_CLASS_UNSAFE, "notifications.hook","Intercept or filter incoming notifications and link opens"},
  {CCORD_PERM_SOCIAL_WRITE,      CCORD_PERM_CLASS_UNSAFE, "social.write",      "Open DMs, update user notes, vote on polls, RSVP events, and use threads/forums"},
  {CCORD_PERM_UI_WINDOW,         CCORD_PERM_CLASS_UNSAFE, "ui.window",         "Modify window title, badge, attention flash, and tray menu items"},
  {CCORD_PERM_VOICE_LEVELS,      CCORD_PERM_CLASS_UNSAFE, "voice.levels",      "Read live voice speaking and audio level meters"},
  {CCORD_PERM_VOICE_PCM,         CCORD_PERM_CLASS_UNSAFE, "voice.pcm",         "Tap or inject raw voice call PCM audio"},

  // Class 3 (System) — OS filesystem, network, notifications, processes
  {CCORD_PERM_FS_READ,           CCORD_PERM_CLASS_SYSTEM, "fs.read",           "Read files and directories on your computer"},
  {CCORD_PERM_FS_WRITE,          CCORD_PERM_CLASS_SYSTEM, "fs.write",          "Create, modify, and delete files on your computer"},
  {CCORD_PERM_FS_WATCH,          CCORD_PERM_CLASS_SYSTEM, "fs.watch",          "Watch local files and directories for changes"},
  {CCORD_PERM_NETWORK_HTTP,      CCORD_PERM_CLASS_SYSTEM, "network.http",      "Make external HTTP/HTTPS requests"},
  {CCORD_PERM_NETWORK_WS,        CCORD_PERM_CLASS_SYSTEM, "network.ws",        "Open external WebSocket connections"},
  {CCORD_PERM_NETWORK_LOCAL,     CCORD_PERM_CLASS_SYSTEM, "network.local",     "Connect to localhost services and local Unix domain sockets"},
  {CCORD_PERM_SYSTEM_NOTIFY,     CCORD_PERM_CLASS_SYSTEM, "system.notify",     "Send native OS desktop notifications"},
  {CCORD_PERM_SYSTEM_OPEN,       CCORD_PERM_CLASS_SYSTEM, "system.open",       "Open URLs in your system web browser"},
  {CCORD_PERM_SYSTEM_INFO,       CCORD_PERM_CLASS_SYSTEM, "system.info",       "Read OS platform details"},
  {CCORD_PERM_SYSTEM_ENV,        CCORD_PERM_CLASS_SYSTEM, "system.env",        "Read OS environment variables and system paths"},
  {CCORD_PERM_SYSTEM_EXEC,       CCORD_PERM_CLASS_SYSTEM, "system.exec",       "Run external programs on your computer"},
  {CCORD_PERM_SYSTEM_SHORTCUTS,  CCORD_PERM_CLASS_SYSTEM, "system.shortcuts",  "Register global OS keyboard shortcuts"},
  {CCORD_PERM_SYSTEM_MEDIA,      CCORD_PERM_CLASS_SYSTEM, "system.media",      "Read and control OS media playback (Now Playing)"},
  {CCORD_PERM_SYSTEM_PROCESSES,  CCORD_PERM_CLASS_SYSTEM, "system.processes",  "Inspect running processes and active foreground window"},
  {CCORD_PERM_CLIPBOARD_IMAGE,   CCORD_PERM_CLASS_SYSTEM, "clipboard.image",   "Read and write images on the system clipboard"},
};

const char *ccord_perm_name(ccord_perm p)
{
  for (size_t i = 0; i < sizeof k_perms / sizeof k_perms[0]; i++) {
    if (k_perms[i].perm == p) return k_perms[i].name;
  }
  return "?";
}

ccord_perm_class ccord_perm_class_of(ccord_perm p)
{
  for (size_t i = 0; i < sizeof k_perms / sizeof k_perms[0]; i++) {
    if (k_perms[i].perm == p) return k_perms[i].cls;
  }
  return CCORD_PERM_CLASS_SAFE;
}

const char *ccord_perm_description(ccord_perm p)
{
  for (size_t i = 0; i < sizeof k_perms / sizeof k_perms[0]; i++) {
    if (k_perms[i].perm == p) return k_perms[i].desc;
  }
  return "";
}

ccord_perm_class ccord_perm_max_class(ccord_perm_set s)
{
  ccord_perm_class mx = CCORD_PERM_CLASS_SAFE;
  for (size_t i = 0; i < sizeof k_perms / sizeof k_perms[0]; i++) {
    if ((s & (uint64_t)k_perms[i].perm) && k_perms[i].cls > mx) mx = k_perms[i].cls;
  }
  return mx;
}

bool ccord_perm_from_name(const char *name, ccord_perm *out)
{
  if (!name) return false;
  for (size_t i = 0; i < sizeof k_perms / sizeof k_perms[0]; i++) {
    if (strcmp(k_perms[i].name, name) == 0) {
      if (out) *out = k_perms[i].perm;
      return true;
    }
  }
  return false;
}

size_t ccord_perm_list(ccord_perm_set s, char *out, size_t cap)
{
  if (!out || cap == 0) return 0;
  size_t len = 0;
  out[0] = '\0';
  for (size_t i = 0; i < sizeof k_perms / sizeof k_perms[0]; i++) {
    if (!(s & (uint64_t)k_perms[i].perm)) continue;
    size_t nl = strlen(k_perms[i].name);
    size_t sep = len ? 1 : 0;
    if (len + sep + nl + 1 > cap) break;
    if (sep) out[len++] = ',';
    memcpy(out + len, k_perms[i].name, nl);
    len += nl;
    out[len] = '\0';
  }
  return len;
}

size_t ccord_perm_count(void)
{
  return sizeof k_perms / sizeof k_perms[0];
}

ccord_perm ccord_perm_at(size_t index)
{
  return index < (sizeof k_perms / sizeof k_perms[0]) ? k_perms[index].perm : (ccord_perm)0;
}
