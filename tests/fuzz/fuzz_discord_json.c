// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Fuzz harness for the live Discord JSON decoders (discord_json.c): the Gateway v9 mapper, the REST
// object/array parsers and the Voice Gateway v8 decoder. One stack ccord_live_event is reused across the
// parsers; each parser re-initialises it internally and it is freed after every call, so there is no
// arena leak. Only built when CCORD_LIVE_ACCOUNT is ON (discord_json.c is gated on it).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/transport.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Run one event parser, then release whatever it allocated before the next parser re-initialises ev.
#define PARSE(call)                     \
  do {                                  \
    ccord_error_clear(&err);            \
    (void)(call);                       \
    ccord_live_event_free(&ev);         \
  } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  static const uint8_t empty = 0;
  if (data == nullptr) { data = &empty; size = 0; }

  char json[8193];
  if (size > 8192u) { size = 8192u; }
  memcpy(json, data, size);
  json[size] = '\0';
  size_t len = size;

  ccord_live_event ev;
  ccord_error err;
  ccord_error_clear(&err);
  ccord_live_event_init(&ev);

  const ccord_id gid = 123456789012345678ULL;
  const ccord_id cid = 234567890123456789ULL;
  const ccord_id uid = 345678901234567890ULL;

  PARSE(ccord_live_gateway_decode(json, len, &ev, &err));
  PARSE(ccord_live_json_message(json, len, CCORD_NET_MESSAGE_CREATE, &ev, &err));
  PARSE(ccord_live_json_history(json, len, cid, &ev, &err));
  PARSE(ccord_live_json_search(json, len, &ev, &err));
  PARSE(ccord_live_json_guild(json, len, &ev, &err));
  PARSE(ccord_live_json_member(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_role(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_channel(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_bans(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_invites(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_audit_log(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_automod_rules(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_prune(json, len, gid, (uint8_t)7, false, &ev, &err));
  PARSE(ccord_live_json_user_settings(json, len, &ev, &err));
  PARSE(ccord_live_json_custom_status(json, len, &ev, &err));
  PARSE(ccord_live_json_forum_posts(json, len, cid, 0u, &ev, &err));
  PARSE(ccord_live_json_forum_post_created(json, len, cid, &ev, &err));
  PARSE(ccord_live_json_slash_commands(json, len, gid, cid, &ev, &err));
  PARSE(ccord_live_json_pins(json, len, cid, false, &ev, &err));
  PARSE(ccord_live_json_guild_emojis(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_guild_stickers(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_guild_soundboard(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_notify_settings(json, len, &ev, &err));
  PARSE(ccord_live_json_user_note(json, len, uid, &ev, &err));
  PARSE(ccord_live_json_user_profile(json, len, uid, &ev, &err));
  PARSE(ccord_live_json_invite_preview(json, len, false, &ev, &err));
  PARSE(ccord_live_json_vanity_url(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_incident_actions(json, len, gid, &ev, &err));
  PARSE(ccord_live_json_discoverable_guilds(json, len, "q", 0u, 0u, &ev, &err));
  PARSE(ccord_live_json_reaction_users(json, len, cid, uid, "emoji", &ev, &err));

  // Parsers that write fixed buffers instead of an event.
  char token[CCORD_LIVE_TOKEN_MAX];
  char ticket[CCORD_LIVE_TOKEN_MAX];
  memset(token, 0, sizeof token);
  memset(ticket, 0, sizeof ticket);
  ccord_error_clear(&err);
  (void)ccord_live_json_auth_response(json, len, 200, token, sizeof token, ticket, sizeof ticket, &err);

  char url[2048];
  char filename[256];
  ccord_error_clear(&err);
  (void)ccord_live_json_upload_slot(json, len, url, sizeof url, filename, sizeof filename, &err);

  char urls[2][2048];
  char filenames[2][256];
  ccord_error_clear(&err);
  (void)ccord_live_json_upload_slots(json, len, 2u, urls, filenames, &err);

  char msg[256];
  (void)ccord_live_json_error_message(json, len, msg, sizeof msg);

  ccord_voice_event ve;
  memset(&ve, 0, sizeof ve);
  ccord_error_clear(&err);
  (void)ccord_voice_gateway_decode(json, len, &ve, &err);

  return 0;
}
