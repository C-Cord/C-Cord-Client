// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// End-to-end headless parity verification for Results 1-3
// (`ccord --headless --verify-slice messaging|settings-proto|events-stage`).
//
// Each slice drives the REAL app pipeline (live_transport + session + store + ui + renderer) against
// forked loopback Discord HTTP + Main Gateway servers (tests/support/fake_discord.h + fake_gateway.h),
// exactly like verify_expressions.c, and asserts what actually reached the servers plus the resulting
// UI/store state before printing its `...=1` result lines. The settings-proto slice additionally runs
// the same favorite-GIF writeback flow over the offline mock transport (private bus/store/session) to
// prove live/mock parity. All scratch files live in a mkdtemp dir under $TMPDIR.
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/core/input.h>
#include <ccord/net/transport.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/ui.h>

typedef struct ccord_verify_parity_ctx {
  ccord_session *session;
  ccord_store *store;
  ccord_ui *ui;
  ccord_renderer *renderer;
  ccord_platform *platform;         // clipboard readback for Copy Message Link
  void *ud;
  void (*frames)(void *ud, int n);        // advance n virtual frames (pump + ui update + draw)
  void (*advance_ms)(void *ud, uint64_t ms);
  uint64_t (*now_ms)(void *ud);           // virtual clock, ms
  void (*input)(void *ud, const ccord_input_event *ev);   // raw UI input event
  ccord_status (*set_transport)(void *ud, ccord_transport *tr, ccord_error *err);
} ccord_verify_parity_ctx;

// Result 1: outgoing forward (message_reference type 1), announcement crosspost, Copy Message Link,
// Mark Unread (POST .../ack + unread state), /tts, Saved Messages persistence + popover, and slash
// SUB_COMMAND_GROUP/SUB_COMMAND + USER/MESSAGE context-menu interaction POSTs.
// Prints forward_msg_ref_type1 / crosspost_published / copy_message_link / mark_unread_ack /
// tts_message_sent / saved_messages_persisted / slash_subcommand_and_ctx_cmd (each =1 on pass).
[[nodiscard]] int ccord_verify_messaging_slice(const ccord_verify_parity_ctx *ctx);

// Result 2: lossless FrecencyUserSettings favorite-GIF writeback. Seeds settings-proto/2 with an
// unknown wire field + one favorite, toggles add/remove through the session, and asserts the store
// state plus every PATCH /users/@me/settings-proto/2 preserves the unknown field and round-trips
// favorites, on both the live and the mock transport.
// Prints settings_proto_patch_sent / settings_proto_unknown_fields_preserved /
// settings_proto_favorites_roundtrip / settings_proto_mock_parity (each =1 on pass).
[[nodiscard]] int ccord_verify_settings_proto_slice(const ccord_verify_parity_ctx *ctx);

// Result 3: Scheduled Event list/create/modify/delete + RSVP PUT/DELETE, Gateway
// GUILD_SCHEDULED_EVENT_* sync, and Stage POST/PATCH/DELETE /stage-instances (Events modal + stage
// prompts drive the UI path). Prints scheduled_event_crud / scheduled_event_rsvp /
// scheduled_event_gateway_sync / stage_instance_create_patch_delete (each =1 on pass).
[[nodiscard]] int ccord_verify_events_stage_slice(const ccord_verify_parity_ctx *ctx);

// Membership screening (Result 4): joining a guild with member verification leaves self pending; the composer
// locks and the session refuses sends; the rules modal shows the GET /guilds/{g}/member-verification form;
// Submit is gated on the checkbox and PUTs /guilds/{g}/requests/@me with response:true and the served version;
// the gateway's pending=false update unlocks sending. Also runs the mock transport's 'mv-test' flow (parity).
// Prints `membership-screening: PASS checks=7` on success.
[[nodiscard]] int ccord_verify_membership_screening_slice(const ccord_verify_parity_ctx *ctx);

// Components v2 (message flag 32768): the gateway delivers a v2 tree (Container, Section, TextDisplay, Thumbnail,
// MediaGallery, File, Separator + nested button/select) and a legacy v1 message; asserts the parsed tree, what the
// renderer drew (no duplicate message-level attachments/embeds), and the POST /interactions each button/select sends.
// Prints cv2_parse / cv2_render / cv2_no_dup_attachments / cv2_button_interaction / cv2_select_interaction /
// cv2_legacy_unchanged (each =1 on pass) and `components-v2: PASS checks=6`; exit 0 only when all six pass.
[[nodiscard]] int ccord_verify_components_v2_slice(const ccord_verify_parity_ctx *ctx);

// Embedded Activities: Activities chip -> GET /activities/shelf?guild_id= listed in shelf_rank order -> clicking an
// entry POSTs /activities/{channel}/{app} with the READY session_id -> gateway EMBEDDED_ACTIVITY_UPDATE_V2 shows the
// instance and its participants (self, then a second user) -> Leave POSTs .../instances/{id}/leave -> an update with
// no participants removes the instance -> a voice disconnect leaves the instance the user is still in. Launch is
// refused outside the voice channel; "Open in browser" hands https://discord.com/channels/{g}/{c} to the host
// opener; the same launch/leave/disconnect flow runs over the mock transport. Prints
// `activities: PASS checks=10` and exits 0 only when every check passes.
[[nodiscard]] int ccord_verify_activities_slice(const ccord_verify_parity_ctx *ctx);

// Voice messages: the real UI recorder (PCM test seam) and Ogg/Opus encoder produce a clip; the UI Send reaches
// ccord_session_send_voice_message and, over forked loopback HTTP, the 3-step upload (slot with file_size, bytes PUT
// to upload_url, message POST with flags 8192 + numeric duration_secs + base64 waveform); a gateway flag-8192 audio/ogg
// message with a waveform is drawn by the waveform player (not the generic card); the mock transport yields the same
// flag/duration/waveform; without ATTACH_FILES nothing is sent. Prints one `voice-messages: check N <name> ok` line
// per check, then `voice-messages: PASS checks=N`.
[[nodiscard]] int ccord_verify_voice_messages_slice(const ccord_verify_parity_ctx *ctx);
