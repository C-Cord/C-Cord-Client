// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// M6 account settings pages: Privacy & Safety, Devices and Connections
//. The page bodies live here; settings.c owns the sidebar,
// the page enum and the dispatch. Every control is backed by a governed session intent and
// renders Discord state from the store / session accessors ONLY after the answer arrives
// (design D4: no optimistic mutation, no fake local values). Adding a new OAuth connection
// and all account-credential/billing edits stay discord.com browser redirects (design D6).
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/ui/ui.h>

// Module-local page ids; settings.c maps its SP_PRIVACY / SP_DEVICES / SP_CONNECTIONS onto these.
enum {
  CCORD_M6_PAGE_PRIVACY = 0,
  CCORD_M6_PAGE_DEVICES,
  CCORD_M6_PAGE_CONNECTIONS,
  CCORD_M6_PAGE_COUNT,
};

// Resets the transient page state (scroll offsets, pending confirmations, visit tracking).
// Called from ui_settings_init.
void ui_settings_m6_reset(void);
// Draws one page body. `content` / `panel` are the settings-modal rects; the Devices and
// Connections pages scroll themselves (fixed scrollbar in the right margin, like Voice & Video).
// Visiting Devices / Connections triggers their TTL-guarded lazy fetch.
void ui_settings_m6_draw(ccord_ui *ui, ccord_renderer *r, int page, ccord_rect content, ccord_rect panel);
// Hit-tests a left click; true when the page consumed it. Destructive actions (logout,
// disconnect) need two clicks: the first arms the button, the second sends the request.
bool ui_settings_m6_click(ccord_ui *ui, int page, ccord_rect content, ccord_rect panel, float x, float y);
// Wheel over a list page (Devices / Connections) queues one eased notch; true when the page moved.
bool ui_settings_m6_wheel(ccord_ui *ui, int page, ccord_rect content, float wheel, bool direct);
// Ticks the list-page glides by fdt_ms; true while one is still moving. Called from ui_settings_scroll_tick.
bool ui_settings_m6_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced);

// Test/introspection seam: the control rects as last drawn (current scroll included), the list
// sizes, fetch states and the pending confirmation. Zeroed before the first draw. Not part of
// the fixed-size ccord_ui_settings_debug snapshot, which predates these pages.
typedef struct ccord_settings_m6_debug {
  int drawn_page;                     // module-local id of the last drawn page, -1 before the first draw
  ccord_rect content;                 // content rect the rects below were computed against
  // Privacy & Safety
  ccord_rect dm_toggle;               // allow DMs from server members (inverted default_guilds_restricted)
  ccord_rect filter_chips[3];         // DM spam filter: 0 off, 1 non-friends, 2 everyone
  ccord_rect friend_all_toggle, friend_mutual_toggle, friend_guild_toggle, age_toggle;
  ccord_rect profile_privacy_chips[3]; // 0 all servers, 1 small servers, 2 friends only
  ccord_rect share_updates_toggle;    // share profile updates with friends
  ccord_rect harvest_data_btn;        // Request My Data button
  // Devices
  int devices_state;                  // ccord_fetch_state of the auth-session list
  size_t device_count;
  ccord_rect device_logout[CCORD_SESSION_MAX_AUTH_SESSIONS];   // zero-sized for the current session
  ccord_rect logout_all_btn, devices_retry_btn;
  // Connections
  int connections_state;              // ccord_fetch_state of the connections list
  size_t connection_count;
  ccord_rect conn_visibility[CCORD_SESSION_MAX_CONNECTIONS];
  ccord_rect conn_activity[CCORD_SESSION_MAX_CONNECTIONS];
  ccord_rect conn_sync[CCORD_SESSION_MAX_CONNECTIONS];
  ccord_rect conn_disconnect[CCORD_SESSION_MAX_CONNECTIONS];
  ccord_rect conn_add_btn, connections_retry_btn;
  // Pending two-click confirmation: 0 none, 1 log-out-all, 2 one device, 3 disconnect.
  int confirm_kind;
  size_t confirm_index;
  float scroll;                       // scroll offset of the drawn list page (0 for Privacy)
} ccord_settings_m6_debug;
void ui_settings_m6_get_debug(const ccord_ui *ui, ccord_settings_m6_debug *out);

// Implemented in settings.c (the sidebar wiring): the three M6 sidebar tab rects (Privacy,
// Devices, Connections order) and the Account-page web button that opens discord.com for the
// excluded credential edits (email / password / username / avatar / 2FA).
void ui_settings_m6_host_rects(const ccord_ui *ui, ccord_rect out_tabs[CCORD_M6_PAGE_COUNT],
                               ccord_rect *out_account_web_btn);
