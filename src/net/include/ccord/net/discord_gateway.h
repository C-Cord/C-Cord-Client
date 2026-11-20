// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/core/link_event.h>
#include <ccord/net/discord_json.h>
#include <ccord/net/discord_rest.h>
#include <ccord/net/gateway_pace.h>

#ifndef CCORD_LIVE_GATEWAY_URL
#define CCORD_LIVE_GATEWAY_URL "wss://gateway.discord.gg/?v=9&encoding=json"
#endif
#ifndef CCORD_LIVE_CAPABILITIES
#define CCORD_LIVE_CAPABILITIES 1734653u
#endif

typedef struct ccord_gateway ccord_gateway;
struct ccord_live_config;
[[nodiscard]] ccord_gateway *ccord_gateway_create(const struct ccord_live_config *cfg, const char *token,
                                                  ccord_error *err);
void ccord_gateway_destroy(ccord_gateway *g);
[[nodiscard]] ccord_status ccord_gateway_start(ccord_gateway *g, ccord_error *err);
void ccord_gateway_stop(ccord_gateway *g);
// The machine woke from sleep or the network changed: drops the socket and reconnects at once (RESUME when a session
// exists), unless the pace says it is too soon. IGNORED unless the gateway is connected or connecting. Never blocks.
ccord_gwpace_wake_verdict ccord_gateway_wake(ccord_gateway *g, ccord_link_event kind);
// Frees the previous returned event, drains the socket without blocking, and
// returns the next queued event. Pointers are valid until the next call.
bool ccord_gateway_poll(ccord_gateway *g, ccord_net_event *out);
ccord_connection_state ccord_gateway_state(const ccord_gateway *g);
const char *ccord_gateway_session_id(const ccord_gateway *g);
const char *ccord_gateway_voice_session_id(const ccord_gateway *g);
[[nodiscard]] ccord_status ccord_gateway_select_guild(ccord_gateway *g, ccord_id guild_id, ccord_error *err);

// Gateway payload builders, exposed so the wire format is unit-testable.
// `out` must be initialised with ccord_buf_init and is never wiped here.
[[nodiscard]] ccord_status ccord_gateway_build_identify(const char *token, ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_build_resume(const char *token, const char *session_id, int64_t seq,
                                                      ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_build_lazy_request(ccord_id guild_id, ccord_buf *out, ccord_error *err);
// Opcode 4 VOICE_STATE_UPDATE. channel_id 0 means "disconnect from voice";
// guild_id CCORD_DM_GUILD_ID sends guild_id:null (join/leave a DM / group call).
[[nodiscard]] ccord_status ccord_gateway_build_voice_state(ccord_id guild_id, ccord_id channel_id, bool self_mute,
                                                           bool self_deaf, bool self_video, bool self_stream,
                                                           ccord_buf *out, ccord_error *err);
// Opcode 3 PRESENCE_UPDATE. `status_text` is an optional custom activity (type 4).
[[nodiscard]] ccord_status ccord_gateway_build_presence(ccord_presence presence, const char *status_text,
                                                        ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_build_presence_ex(ccord_presence presence, const char *emoji_name,
                                                           ccord_id emoji_id, const char *status_text,
                                                           ccord_buf *out, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_presence(ccord_gateway *g, ccord_presence presence,
                                                       const char *status_text, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_presence_ex(ccord_gateway *g, ccord_presence presence,
                                                          const char *emoji_name, ccord_id emoji_id,
                                                          const char *status_text, ccord_error *err);
// Rich Presence (Discord RPC SET_ACTIVITY): a JSON array of up to CCORD_GATEWAY_RICH_MAX_ACTIVITIES activity
// objects (each with a string "name" and an integral "type"), at most CCORD_GATEWAY_RICH_MAX_BYTES bytes.
// ccord_gateway_set_rich_activities validates and stores it; every op 3 from ccord_gateway_send_presence_ex
// splices it raw after the custom-status activity. Invisible presence omits it. nullptr/0 clears.
[[nodiscard]] ccord_status ccord_gateway_set_rich_activities(ccord_gateway *g, const char *json, size_t len,
                                                             ccord_error *err);
// Opcode 3 builder with the rich-activities array spliced after the custom status (same validation as the setter).
[[nodiscard]] ccord_status ccord_gateway_build_presence_rich(ccord_presence presence, const char *emoji_name,
                                                             ccord_id emoji_id, const char *status_text,
                                                             const char *rich_json, size_t rich_len,
                                                             ccord_buf *out, ccord_error *err);
// Opcode 18 Stream Create with the chosen encoder/codec/framework/resolution/fps.
[[nodiscard]] ccord_status ccord_gateway_build_stream_create(ccord_id guild_id, ccord_id channel_id,
                                                             const ccord_stream_config *config, ccord_buf *out,
                                                             ccord_error *err);
// Opcode 19 Stream Delete for the guild's active stream.
[[nodiscard]] ccord_status ccord_gateway_build_stream_delete(ccord_id guild_id, ccord_id channel_id, ccord_buf *out,
                                                             ccord_error *err);
// Formats/parses a Discord stream_key ("guild:<gid>:<cid>:<uid>" or "call:<cid>:<uid>").
void ccord_stream_key_format(ccord_id guild_id, ccord_id channel_id, ccord_id user_id, char out[96]);
bool ccord_stream_key_parse(const char *stream_key, ccord_id *out_guild_id, ccord_id *out_channel_id,
                            ccord_id *out_user_id);
// Opcode 19 Stream Delete by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_delete_key(const char *stream_key, ccord_buf *out,
                                                                 ccord_error *err);
// Opcode 20 Stream Watch by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_watch(const char *stream_key, ccord_buf *out,
                                                            ccord_error *err);
// Opcode 21 Stream Ping by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_ping(const char *stream_key, ccord_buf *out, ccord_error *err);
// Opcode 22 Stream Set Paused by stream_key.
[[nodiscard]] ccord_status ccord_gateway_build_stream_set_paused(const char *stream_key, bool paused,
                                                                 ccord_buf *out, ccord_error *err);
// Send helpers used by the live transport action hooks. They require an open gateway.
[[nodiscard]] ccord_status ccord_gateway_send_voice_state(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                          bool self_mute, bool self_deaf, bool self_video,
                                                          bool self_stream, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_create(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                            const ccord_stream_config *config, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_delete(ccord_gateway *g, ccord_id guild_id, ccord_id channel_id,
                                                            ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_delete_key(ccord_gateway *g, const char *stream_key,
                                                                ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_watch(ccord_gateway *g, const char *stream_key,
                                                           ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_ping(ccord_gateway *g, const char *stream_key, ccord_error *err);
[[nodiscard]] ccord_status ccord_gateway_send_stream_set_paused(ccord_gateway *g, const char *stream_key, bool paused,
                                                                ccord_error *err);

