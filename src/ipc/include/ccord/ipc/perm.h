// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
// Permission vocabulary shared by main (enforces) and host (early check + debug log).
// Class 1 (SAFE) APIs require no declaration; legacy Safe names remain accepted in manifests.
// Class 2 (UNSAFE) and Class 3 (SYSTEM) APIs MUST be declared in plugin.json; undeclared calls are ignored and logged.
typedef enum ccord_perm_class : uint8_t {
  CCORD_PERM_CLASS_SAFE   = 0,
  CCORD_PERM_CLASS_UNSAFE = 1,
  CCORD_PERM_CLASS_SYSTEM = 2,
} ccord_perm_class;

typedef enum ccord_perm : uint64_t {
  // Legacy / Safe-tier identifiers (never require user consent, always available to plugins)
  CCORD_PERM_EVENTS_NAVIGATION = 1ull << 1,   // "events.navigation"
  CCORD_PERM_UI_CONTEXTMENU    = 1ull << 2,   // "ui.contextMenu"
  CCORD_PERM_CLIPBOARD_WRITE   = 1ull << 3,   // "clipboard.write"
  CCORD_PERM_STORAGE           = 1ull << 4,   // "storage"
  CCORD_PERM_COMMANDS          = 1ull << 5,   // "commands"

  // Class 2: Unsafe APIs (C-Cord internal state, mutations, hooks, UI)
  CCORD_PERM_EVENTS_MESSAGES   = 1ull << 0,   // "events.messages"
  CCORD_PERM_EVENTS_PRESENCE   = 1ull << 6,   // "events.presence"
  CCORD_PERM_EVENTS_VOICE      = 1ull << 7,   // "events.voice"
  CCORD_PERM_EVENTS_CHANNELS   = 1ull << 8,   // "events.channels"
  CCORD_PERM_STORE_READ        = 1ull << 9,   // "store.read"
  CCORD_PERM_MESSAGES_SEND     = 1ull << 10,  // "messages.send"
  CCORD_PERM_MESSAGES_MANAGE   = 1ull << 11,  // "messages.manage"
  CCORD_PERM_MESSAGES_HOOK     = 1ull << 12,  // "messages.hook"
  CCORD_PERM_PRESENCE_WRITE    = 1ull << 13,  // "presence.write"
  CCORD_PERM_NAVIGATION_WRITE  = 1ull << 14,  // "navigation.write"
  CCORD_PERM_VOICE_CONTROL     = 1ull << 15,  // "voice.control"
  CCORD_PERM_UI_PANEL          = 1ull << 16,  // "ui.panel"
  CCORD_PERM_UI_COMPOSER       = 1ull << 17,  // "ui.composer"
  CCORD_PERM_CLIPBOARD_READ    = 1ull << 18,  // "clipboard.read"
  CCORD_PERM_GATEWAY_RAW       = 1ull << 19,  // "gateway.raw"
  CCORD_PERM_UI_DECORATE       = 1ull << 20,  // "ui.decorate"
  CCORD_PERM_THEME_WRITE       = 1ull << 21,  // "theme.write"
  CCORD_PERM_AUDIO_EARCONS     = 1ull << 22,  // "audio.earcons"
  CCORD_PERM_NOTIFICATIONS_HOOK = 1ull << 23, // "notifications.hook"
  CCORD_PERM_SOCIAL_WRITE      = 1ull << 32,  // "social.write"
  CCORD_PERM_UI_WINDOW         = 1ull << 33,  // "ui.window"
  CCORD_PERM_VOICE_LEVELS      = 1ull << 34,  // "voice.levels"
  CCORD_PERM_VOICE_PCM         = 1ull << 35,  // "voice.pcm"

  // Class 3: System-level APIs (OS filesystem, network, notifications, processes)
  CCORD_PERM_FS_READ           = 1ull << 24,  // "fs.read"
  CCORD_PERM_FS_WRITE          = 1ull << 25,  // "fs.write"
  CCORD_PERM_FS_WATCH          = 1ull << 26,  // "fs.watch"
  CCORD_PERM_NETWORK_HTTP      = 1ull << 27,  // "network.http"
  CCORD_PERM_SYSTEM_NOTIFY     = 1ull << 28,  // "system.notify"
  CCORD_PERM_SYSTEM_OPEN       = 1ull << 29,  // "system.open"
  CCORD_PERM_SYSTEM_INFO       = 1ull << 30,  // "system.info"
  CCORD_PERM_SYSTEM_EXEC       = 1ull << 31,  // "system.exec"
  CCORD_PERM_NETWORK_WS        = 1ull << 40,  // "network.ws"
  CCORD_PERM_NETWORK_LOCAL     = 1ull << 41,  // "network.local"
  CCORD_PERM_SYSTEM_ENV        = 1ull << 42,  // "system.env"
  CCORD_PERM_SYSTEM_SHORTCUTS  = 1ull << 43,  // "system.shortcuts"
  CCORD_PERM_SYSTEM_MEDIA      = 1ull << 44,  // "system.media"
  CCORD_PERM_SYSTEM_PROCESSES  = 1ull << 45,  // "system.processes"
  CCORD_PERM_CLIPBOARD_IMAGE   = 1ull << 46,  // "clipboard.image"
  CCORD_PERM_ACCOUNT_READ      = 1ull << 47,  // "account.read"
  CCORD_PERM_PROFILE_WRITE     = 1ull << 48,  // "account.profile"
  CCORD_PERM_ACCOUNT_WRITE     = 1ull << 49,  // "account.write"
} ccord_perm;

typedef uint64_t ccord_perm_set;

const char *ccord_perm_name(ccord_perm p);
ccord_perm_class ccord_perm_class_of(ccord_perm p);
const char *ccord_perm_description(ccord_perm p);
ccord_perm_class ccord_perm_max_class(ccord_perm_set s);
bool ccord_perm_from_name(const char *name, ccord_perm *out);
size_t ccord_perm_list(ccord_perm_set s, char *out, size_t cap);   // "a,b,c" for display, always NUL-terminated
size_t ccord_perm_count(void);
ccord_perm ccord_perm_at(size_t index);
