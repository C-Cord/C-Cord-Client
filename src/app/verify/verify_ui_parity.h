// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Headless UI-parity verification (`ccord --headless --mock --verify-slice NAME`).
//
// The slices below drive the real app pipeline (mock transport + session + store + ui + renderer), like
// verify_gif_favorites.c / verify_embed_video.c. Each entry point returns 0 only after printing its
// `<name>: PASS checks=N` line, and returns non-zero after printing `<name>: FAIL ...` otherwise.
#include <stdint.h>

#include <ccord/core/input.h>
#include <ccord/net/transport.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_ui_parity_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_renderer *renderer;
  ccord_transport *transport;               // the offline mock transport
  void *ud;
  void (*frames)(void *ud, int n);          // advance n virtual frames (pump + ui update + draw)
  // Run one real pump + ui update + draw frame, advancing virtual time by ms (e.g. 8 or 16). The `frames` step
  // above keeps its fixed 100 ms per frame; animation checks use this one.
  void (*frame_ms)(void *ud, uint32_t ms);
  void (*input)(void *ud, const ccord_input_event *ev);
  ccord_platform *platform;                 // the window platform: its drop-file queue and event pump (plus-menu slice)
} ccord_verify_ui_parity_ctx;

// DM call bar must not cover the top of the message timeline. Prints `call-bar: PASS checks=N` (exit 0)
// or `call-bar: FAIL <check>` (exit non-zero).
[[nodiscard]] int ccord_verify_call_bar_slice(const ccord_verify_ui_parity_ctx *ctx);

// Search results belong in their own panel while the main timeline stays unfiltered; a result click jumps
// to the message. Prints `search-view: PASS checks=N` (exit 0) or `search-view: FAIL <check>`.
[[nodiscard]] int ccord_verify_search_view_slice(const ccord_verify_ui_parity_ctx *ctx);

// Emoji/GIF picker columns derive from the popover width (no clipped cell) and the popover clamps to small
// windows. Prints `picker-layout: PASS checks=N` (exit 0) or `picker-layout: FAIL <check>`.
[[nodiscard]] int ccord_verify_picker_layout_slice(const ccord_verify_ui_parity_ctx *ctx);

// The voice "Text Chat" button opens the voice channel's text chat while staying connected.
// Prints `voice-text-chat: PASS checks=N` (exit 0) or `voice-text-chat: FAIL <check>`.
[[nodiscard]] int ccord_verify_voice_text_chat_slice(const ccord_verify_ui_parity_ctx *ctx);

// Message/mention/voice join-leave/mute-deafen/incoming-call earcons play and obey the setting that turns
// them off. Prints `earcons: PASS checks=N` (exit 0) or `earcons: FAIL <check>`.
[[nodiscard]] int ccord_verify_earcons_slice(const ccord_verify_ui_parity_ctx *ctx);

// Wave-2 slices. Until each is implemented, its stub prints `<name>: FAIL not implemented` and returns 1.
// Reply connector line and composer reply bar. Prints `reply-style: PASS checks=N` or `reply-style: FAIL ...`.
[[nodiscard]] int ccord_verify_reply_style_slice(const ccord_verify_ui_parity_ctx *ctx);
// One shared style token set for buttons, menus, popovers, modals, tooltips and inputs.
// Prints `ui-style: PASS checks=N` or `ui-style: FAIL ...`.
[[nodiscard]] int ccord_verify_ui_style_slice(const ccord_verify_ui_parity_ctx *ctx);
// Member list and user profile panel toggle independently and survive a restart.
// Prints `panel-toggles: PASS checks=N` or `panel-toggles: FAIL ...`.
[[nodiscard]] int ccord_verify_panel_toggles_slice(const ccord_verify_ui_parity_ctx *ctx);
// Eased, frame-rate independent scrolling on every scrollable surface; instant under reduced motion.
// Prints `smooth-scroll surfaces: ok` per surface, then `smooth-scroll: PASS checks=N` or `smooth-scroll: FAIL ...`.
[[nodiscard]] int ccord_verify_smooth_scroll_slice(const ccord_verify_ui_parity_ctx *ctx);

// --verify-slice motion runs the four area checks below and prints `motion <area>: ok` or `motion <area>: FAIL`
// for each, then `motion: PASS checks=N` only when all four return 0. Each area adds its check count to *checks.
[[nodiscard]] int ccord_verify_motion_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_motion_overlays(const ccord_verify_ui_parity_ctx *ctx, int *checks);
[[nodiscard]] int ccord_verify_motion_nav(const ccord_verify_ui_parity_ctx *ctx, int *checks);
[[nodiscard]] int ccord_verify_motion_chat(const ccord_verify_ui_parity_ctx *ctx, int *checks);
[[nodiscard]] int ccord_verify_motion_controls(const ccord_verify_ui_parity_ctx *ctx, int *checks);

// Wave-3 slices. Until each is implemented, its stub prints `<name>: FAIL not implemented` and returns 1.
// Each implemented slice prints `<name>: PASS checks=N` (exit 0) or `<name>: FAIL ...`.
// GIFs and embeds hosted on Discord's CDN load and animate like Tenor/Giphy ones.
[[nodiscard]] int ccord_verify_media_load_slice(const ccord_verify_ui_parity_ctx *ctx);
// Hovering your own message shows an Edit button that starts editing it.
[[nodiscard]] int ccord_verify_hover_edit_slice(const ccord_verify_ui_parity_ctx *ctx);
// Status card anchored above the avatar; status choice applies at once.
[[nodiscard]] int ccord_verify_status_card_slice(const ccord_verify_ui_parity_ctx *ctx);
// "Jump to present" appears only after N messages sit below the viewport (settings, default 10).
[[nodiscard]] int ccord_verify_jump_present_slice(const ccord_verify_ui_parity_ctx *ctx);
// Settings has a separate C-Cord section for C-Cord-specific settings.
[[nodiscard]] int ccord_verify_ccord_settings_slice(const ccord_verify_ui_parity_ctx *ctx);
// Running games are detected and shown as Playing presence; a setting turns detection off.
[[nodiscard]] int ccord_verify_game_detect_slice(const ccord_verify_ui_parity_ctx *ctx);
// Profile edit inside C-Cord (display name, avatar, banner, bio, pronouns, accent); credentials link out.
[[nodiscard]] int ccord_verify_profile_edit_slice(const ccord_verify_ui_parity_ctx *ctx);
// Adding your own reaction counts once whether the REST result or the gateway echo arrives first.
[[nodiscard]] int ccord_verify_reaction_once_slice(const ccord_verify_ui_parity_ctx *ctx);
// A local RPC client's activity is published under the real application name resolved from its client_id.
// Prints `rpc-app-name: PASS checks=N` or `rpc-app-name: FAIL ...`.
[[nodiscard]] int ccord_verify_rpc_app_name_slice(const ccord_verify_ui_parity_ctx *ctx);

// Planned feature slices (stubs until implemented): each prints `<name>: PASS checks=N` (exit 0) or `<name>: FAIL ...`.
[[nodiscard]] int ccord_verify_warm_start_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_plus_menu_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_poll_create_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_schedule_message_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_fav_gif_order_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_tray_unread_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_close_background_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_app_icon_slice(const ccord_verify_ui_parity_ctx *ctx);

// Stub slices (each prints `<name>: FAIL stub` and returns 1 until its feature lands; replace only its own file).
[[nodiscard]] int ccord_verify_status_sync_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_blocked_hide_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_ctx_react_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_new_messages_bar_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_report_flow_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_call_ring_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_global_ptt_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_voice_reconnect_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_notify_jump_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_resume_reconnect_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_profile_view_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_profile_roles_slice(const ccord_verify_ui_parity_ctx *ctx);
// The server rail scrolls by pixels like the channel list. Prints `rail-scroll: PASS checks=N` or `rail-scroll: FAIL ...`.
[[nodiscard]] int ccord_verify_rail_scroll_slice(const ccord_verify_ui_parity_ctx *ctx);

// Stub slices for composer, settings and text rendering (each prints `<name>: FAIL stub` and returns 1 until its
// feature lands; each feature task replaces only its own file).
[[nodiscard]] int ccord_verify_composer_format_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_settings_profile_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_glyph_fallback_slice(const ccord_verify_ui_parity_ctx *ctx);
[[nodiscard]] int ccord_verify_ansi_block_slice(const ccord_verify_ui_parity_ctx *ctx);
