// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_test_harness.h"

#include <ccord/core/alloc.h>
#include <ccord/net/discord_proto.h>

typedef struct fake_parity_transport {
  ccord_transport base;
  ccord_net_event ev[32];
  size_t i, n;
  // settings-proto
  uint32_t patch_proto_count;
  bool fail_patch_proto;
  uint8_t *patched_proto;
  size_t patched_proto_len;
  // messaging
  uint32_t tts_count;
  ccord_id last_tts_ch;
  char last_tts_text[128];
  uint32_t forward_count;
  ccord_id last_fwd_dst, last_fwd_src, last_fwd_msg;
  uint32_t crosspost_count;
  ccord_id last_xpost_ch, last_xpost_msg;
  uint32_t mark_unread_count;
  bool fail_mark_unread;
  ccord_id last_unread_ch, last_unread_msg;
  // interactions
  uint32_t interaction_count;
  ccord_interaction_request last_interaction;
  char last_subcmd[64];
  char last_subcmd_group[64];
  // admin (stage & scheduled events)
  uint32_t admin_count;
  ccord_admin_request last_admin;
} fake_parity_transport;

static ccord_status fake_parity_noop_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)limit;
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool fake_parity_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  if (f->i >= f->n) return false;
  *out = f->ev[f->i++];
  return true;
}

static ccord_status fake_parity_send(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                     ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)utf8;
  (void)len;
  ccord_error_clear(err);
  return CCORD_OK;
}

static void fake_parity_destroy(ccord_transport *t)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  ccord_free(f->patched_proto);
  free(f);
}

static ccord_status fake_parity_patch_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len,
                                            ccord_error *err)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  if (f->fail_patch_proto) return ccord_error_set(err, CCORD_ERR_IO, "fake: patch settings proto refused");
  f->patch_proto_count++;
  ccord_free(f->patched_proto);
  f->patched_proto = ccord_malloc(proto_len ? proto_len : 1u);
  if (!f->patched_proto) return ccord_error_set(err, CCORD_ERR_NOMEM, "oom");
  if (proto_len > 0) memcpy(f->patched_proto, proto, proto_len);
  f->patched_proto_len = proto_len;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_parity_send_tts(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                         ccord_error *err)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  f->tts_count++;
  f->last_tts_ch = channel_id;
  size_t copy = len < sizeof f->last_tts_text - 1u ? len : sizeof f->last_tts_text - 1u;
  memcpy(f->last_tts_text, utf8, copy);
  f->last_tts_text[copy] = '\0';
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_parity_forward(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id,
                                        ccord_id message_id, ccord_error *err)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  f->forward_count++;
  f->last_fwd_dst = channel_id;
  f->last_fwd_src = source_channel_id;
  f->last_fwd_msg = message_id;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_parity_crosspost(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                          ccord_error *err)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  f->crosspost_count++;
  f->last_xpost_ch = channel_id;
  f->last_xpost_msg = message_id;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_parity_mark_unread(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                            ccord_error *err)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  if (f->fail_mark_unread) return ccord_error_set(err, CCORD_ERR_IO, "fake: mark unread failed");
  f->mark_unread_count++;
  f->last_unread_ch = channel_id;
  f->last_unread_msg = message_id;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_parity_interaction(ccord_transport *t, const ccord_interaction_request *req, ccord_error *err)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  f->interaction_count++;
  f->last_interaction = *req;
  f->last_subcmd[0] = '\0';
  f->last_subcmd_group[0] = '\0';
  if (req->subcommand) snprintf(f->last_subcmd, sizeof f->last_subcmd, "%s", req->subcommand);
  if (req->subcommand_group) snprintf(f->last_subcmd_group, sizeof f->last_subcmd_group, "%s", req->subcommand_group);
  f->last_interaction.subcommand = f->last_subcmd[0] ? f->last_subcmd : nullptr;
  f->last_interaction.subcommand_group = f->last_subcmd_group[0] ? f->last_subcmd_group : nullptr;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_parity_admin(ccord_transport *t, const ccord_admin_request *req, ccord_error *err)
{
  fake_parity_transport *f = (fake_parity_transport *)t;
  f->admin_count++;
  f->last_admin = *req;
  ccord_error_clear(err);
  return CCORD_OK;
}

static const ccord_transport_vtable fake_parity_vt = {
  .name = "fake-parity",
  .connect = fake_connect,
  .disconnect = fake_disconnect,
  .request_history = fake_parity_noop_history,
  .send_message = fake_parity_send,
  .poll = fake_parity_poll,
  .destroy = fake_parity_destroy,
  .admin_action = fake_parity_admin,
  .send_interaction = fake_parity_interaction,
  .send_tts_message = fake_parity_send_tts,
  .forward_message = fake_parity_forward,
  .crosspost_message = fake_parity_crosspost,
  .mark_unread = fake_parity_mark_unread,
  .patch_settings_proto = fake_parity_patch_proto,
};

static void test_session_favorite_gif_lossless_proto_roundtrip(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_parity_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) return;
  f->base.vt = &fake_parity_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  // Non-GIF root records in FrecencyUserSettings that MUST survive every favorite-GIF PATCH verbatim:
  //   field 1 (versions, bytes):   0a 04 08 2a 10 07
  //   field 5 (favorite_emojis):   2a 06 0a 04 f0 9f 94 a5  ("🔥")
  //   field 9 (unknown varint):    48 99 01
  //   field 10 (unknown fixed64):  51 44 33 22 11 de ad be ef
  //   field 11 (unknown fixed32):  5d 12 34 56 78
  //   field 12 (unknown bytes):    62 05 'h' 'e' 'l' 'l' 'o'
  static const uint8_t k_rec_versions[] = {0x0a, 0x04, 0x08, 0x2a, 0x10, 0x07};
  static const uint8_t k_rec_emojis[] = {0x2a, 0x06, 0x0a, 0x04, 0xf0, 0x9f, 0x94, 0xa5};
  static const uint8_t k_rec_varint[] = {0x48, 0x99, 0x01};
  static const uint8_t k_rec_fixed64[] = {0x51, 0x44, 0x33, 0x22, 0x11, 0xde, 0xad, 0xbe, 0xef};
  static const uint8_t k_rec_fixed32[] = {0x5d, 0x12, 0x34, 0x56, 0x78};
  static const uint8_t k_rec_bytes[] = {0x62, 0x05, 'h', 'e', 'l', 'l', 'o'};
  uint8_t base_proto[64];
  size_t base_len = 0;
  memcpy(base_proto + base_len, k_rec_versions, sizeof k_rec_versions); base_len += sizeof k_rec_versions;
  memcpy(base_proto + base_len, k_rec_emojis, sizeof k_rec_emojis);     base_len += sizeof k_rec_emojis;
  memcpy(base_proto + base_len, k_rec_varint, sizeof k_rec_varint);     base_len += sizeof k_rec_varint;
  memcpy(base_proto + base_len, k_rec_fixed64, sizeof k_rec_fixed64);   base_len += sizeof k_rec_fixed64;
  memcpy(base_proto + base_len, k_rec_fixed32, sizeof k_rec_fixed32);   base_len += sizeof k_rec_fixed32;
  memcpy(base_proto + base_len, k_rec_bytes, sizeof k_rec_bytes);       base_len += sizeof k_rec_bytes;

  ccord_proto_gif seed_gifs[2] = {
    {.url = "https://tenor.com/view/alpha-1", .src = "https://cdn.example/alpha.mp4",
     .format = 2, .width = 640, .height = 360, .order = 10},
    {.url = "https://tenor.com/view/beta-2", .src = "https://cdn.example/beta.gif",
     .format = 1, .width = 320, .height = 240, .order = 25},
  };
  uint8_t *initial_raw = nullptr;
  size_t initial_raw_len = 0;
  CT_CHECK(ccord_user_expressions_encode_gifs(seed_gifs, 2, base_proto, base_len,
                                              &initial_raw, &initial_raw_len, &err) == CCORD_OK);
  char *empty_json = nullptr;
  CT_CHECK(ccord_user_expressions_encode_gifs_json(nullptr, 0, nullptr, 0, &empty_json, &err) == CCORD_OK);
  // Build {"settings":"<b64>"} over base_proto + seed_gifs by encoding b64 directly.
  char *initial_b64 = nullptr;
  CT_CHECK(ccord_user_expressions_encode_gifs_b64(nullptr, 0, nullptr, 0, &initial_b64, &err) == CCORD_OK);
  ccord_free(initial_b64);
  ccord_free(empty_json);

  // Ingest 1: JSON body carrying base64 settings-proto (simulate GET /users/@me/settings-proto/2).
  // First seed base_proto via raw_proto on an auxiliary store to obtain JSON, or encode b64 manually:
  static const char b64_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char json_body[512];
  size_t pos = (size_t)snprintf(json_body, sizeof json_body, "{\"settings\":\"");
  for (size_t k = 0; k < initial_raw_len; k += 3) {
    uint32_t v = (uint32_t)initial_raw[k] << 16;
    if (k + 1 < initial_raw_len) v |= (uint32_t)initial_raw[k + 1] << 8;
    if (k + 2 < initial_raw_len) v |= (uint32_t)initial_raw[k + 2];
    json_body[pos++] = b64_tab[(v >> 18) & 0x3f];
    json_body[pos++] = b64_tab[(v >> 12) & 0x3f];
    json_body[pos++] = (k + 1 < initial_raw_len) ? b64_tab[(v >> 6) & 0x3f] : '=';
    json_body[pos++] = (k + 2 < initial_raw_len) ? b64_tab[v & 0x3f] : '=';
  }
  pos += (size_t)snprintf(json_body + pos, sizeof json_body - pos, "\"}");

  f->ev[f->n++] = (ccord_net_event){
    .type = CCORD_NET_EXPRESSION_DATA,
    .status = CCORD_OK,
    .expr_kind = CCORD_EXPR_SETTINGS_PROTO,
    .expr_arg = "",
    .expr_body = json_body,
    .expr_body_len = pos,
  };
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, "") == CCORD_EXPR_BLOB_READY);
  size_t cached_len = 0;
  const uint8_t *cached = ccord_store_get_raw_settings_proto(store, &cached_len);
  CT_CHECK(cached && cached_len == initial_raw_len && memcmp(cached, initial_raw, initial_raw_len) == 0);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 2);
  const ccord_gif_item *g0 = ccord_store_favorite_gif_at(store, 0);
  const ccord_gif_item *g1 = ccord_store_favorite_gif_at(store, 1);
  // Newest first: the order-25 favorite (format 1) leads the order-10 one (format 2, 640x360).
  CT_CHECK(g0 && g0->format == 1 && g0->order == 25);
  CT_CHECK(g1 && g1->format == 2 && g1->order == 10 && g1->width == 640 && g1->height == 360);
  CT_CHECK(ccord_store_is_favorite_emoji(store, "\xf0\x9f\x94\xa5"));

  // Toggle ON a 3rd GIF: re-encodes over cached wire bytes and calls patch_settings_proto.
  ccord_gif_item add_gif = {.width = 400, .height = 300};
  snprintf(add_gif.url, sizeof add_gif.url, "https://tenor.com/view/gamma-3");
  snprintf(add_gif.src, sizeof add_gif.src, "https://cdn.example/gamma.gif");
  CT_CHECK(ccord_session_toggle_favorite_gif(s, &add_gif, &err) == CCORD_OK);
  CT_CHECK(f->patch_proto_count == 1 && f->patched_proto != nullptr);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 3);

  ccord_user_expressions dec = {0};
  CT_CHECK(ccord_user_expressions_decode(f->patched_proto, f->patched_proto_len, &dec, &err) == CCORD_OK);
  CT_CHECK(dec.gif_count == 3);
  CT_CHECK(dec.gifs[0].format == 2 && dec.gifs[0].order == 10);
  CT_CHECK(dec.gifs[1].format == 1 && dec.gifs[1].order == 25);
  CT_CHECK(strcmp(dec.gifs[2].url, "https://tenor.com/view/gamma-3") == 0 && dec.gifs[2].order == 26);
  CT_CHECK(dec.favorite_emoji_count == 1 && strcmp(dec.favorite_emojis[0], "\xf0\x9f\x94\xa5") == 0);
  ccord_user_expressions_free(&dec);
  CT_CHECK(bytes_contain(f->patched_proto, f->patched_proto_len, k_rec_versions, sizeof k_rec_versions));
  CT_CHECK(bytes_contain(f->patched_proto, f->patched_proto_len, k_rec_varint, sizeof k_rec_varint));
  CT_CHECK(bytes_contain(f->patched_proto, f->patched_proto_len, k_rec_fixed64, sizeof k_rec_fixed64));
  CT_CHECK(bytes_contain(f->patched_proto, f->patched_proto_len, k_rec_fixed32, sizeof k_rec_fixed32));
  CT_CHECK(bytes_contain(f->patched_proto, f->patched_proto_len, k_rec_bytes, sizeof k_rec_bytes));

  // Toggle OFF the 1st GIF (alpha-1): removes from store, PATCHes 2 remaining GIFs, still preserves unknown fields.
  ccord_gif_item rem_gif = {0};
  snprintf(rem_gif.url, sizeof rem_gif.url, "https://tenor.com/view/alpha-1");
  CT_CHECK(ccord_session_toggle_favorite_gif(s, &rem_gif, &err) == CCORD_OK);
  CT_CHECK(f->patch_proto_count == 2);
  CT_CHECK(!ccord_store_is_favorite_gif(store, "https://tenor.com/view/alpha-1"));
  CT_CHECK(ccord_store_favorite_gif_count(store) == 2);
  CT_CHECK(ccord_user_expressions_decode(f->patched_proto, f->patched_proto_len, &dec, &err) == CCORD_OK);
  CT_CHECK(dec.gif_count == 2 && strcmp(dec.gifs[0].url, "https://tenor.com/view/beta-2") == 0);
  ccord_user_expressions_free(&dec);
  CT_CHECK(bytes_contain(f->patched_proto, f->patched_proto_len, k_rec_fixed64, sizeof k_rec_fixed64));
  CT_CHECK(bytes_contain(f->patched_proto, f->patched_proto_len, k_rec_bytes, sizeof k_rec_bytes));

  // Ingest 2: raw_proto event without expr_body updates the store cache directly.
  f->ev[f->n++] = (ccord_net_event){
    .type = CCORD_NET_EXPRESSION_DATA,
    .status = CCORD_OK,
    .expr_kind = CCORD_EXPR_SETTINGS_PROTO,
    .expr_arg = "",
    .raw_proto = f->patched_proto,
    .raw_proto_len = f->patched_proto_len,
  };
  CT_CHECK(pump_all(s) == 1);
  cached = ccord_store_get_raw_settings_proto(store, &cached_len);
  CT_CHECK(cached && cached_len == f->patched_proto_len);

  // Synchronous transport failure rolls back the local toggle so store & server cache never diverge.
  f->fail_patch_proto = true;
  CT_CHECK(ccord_session_toggle_favorite_gif(s, &rem_gif, &err) == CCORD_ERR_IO);
  CT_CHECK(!ccord_store_is_favorite_gif(store, "https://tenor.com/view/alpha-1"));
  ccord_gif_item rem_beta = {0};
  snprintf(rem_beta.url, sizeof rem_beta.url, "https://tenor.com/view/beta-2");
  CT_CHECK(ccord_session_toggle_favorite_gif(s, &rem_beta, &err) == CCORD_ERR_IO);
  CT_CHECK(ccord_store_is_favorite_gif(store, "https://tenor.com/view/beta-2"));
  const ccord_gif_item *beta_after = nullptr;
  for (size_t bi = 0; bi < ccord_store_favorite_gif_count(store); bi++) {
    const ccord_gif_item *cand = ccord_store_favorite_gif_at(store, bi);
    if (cand && strcmp(cand->url, "https://tenor.com/view/beta-2") == 0) beta_after = cand;
  }
  CT_CHECK(beta_after && beta_after->order == 25 && beta_after->format == 1);
  f->fail_patch_proto = false;

  // Input validation + store favorite-GIF cap overflow guard (must return CCORD_ERR_RANGE without calling patch_settings_proto).
  ccord_gif_item empty_gif = {0};
  CT_CHECK(ccord_session_toggle_favorite_gif(s, &empty_gif, &err) == CCORD_ERR_INVALID);
  for (size_t k = 0; k < 300u; k++) {
    ccord_gif_item fill = {.width = 100, .height = 100};
    snprintf(fill.url, sizeof fill.url, "https://tenor.com/view/fill-%zu", k);
    if (!ccord_store_toggle_favorite_gif(store, &fill)) break;
  }
  CT_CHECK(ccord_store_favorite_gif_count(store) == 128u);
  uint32_t patches_before = f->patch_proto_count;
  ccord_gif_item over = {.width = 100, .height = 100};
  snprintf(over.url, sizeof over.url, "https://tenor.com/view/overflow-cap");
  CT_CHECK(ccord_session_toggle_favorite_gif(s, &over, &err) == CCORD_ERR_RANGE);
  CT_CHECK(f->patch_proto_count == patches_before);

  ccord_free(initial_raw);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_messaging_parity_and_interactions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_parity_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) return;
  f->base.vt = &fake_parity_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_user users[2] = {{.id = 1, .name = "self", .status_text = ""}, {.id = 2, .name = "peer", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = 10, .name = "Guild", .owner_id = 2}};
  ccord_channel chans[6] = {
    {.id = 20, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 21, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "news", .topic = "",
     .discord_type = CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT},
    {.id = 22, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "Voice", .topic = ""},
    {.id = 24, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "locked-dest", .topic = ""},
    {.id = 30, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "peer", .topic = ""},
    {.id = 31, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "stranger", .topic = "",
     .is_message_request = true},
  };
  ccord_member members[2] = {{.guild_id = 10, .user_id = 1, .role_name = "M"},
                             {.guild_id = 10, .user_id = 2, .role_name = "M"}};
  ccord_role roles[2] = {
    {.id = 10, .guild_id = 10, .name = "@everyone",
     .permissions = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY},
    {.id = 200, .guild_id = 10, .name = "Extra", .position = 1, .permissions = 0},
  };
  ccord_id self_roles[1] = {200};
  ccord_member_roles mroles[1] = {{.guild_id = 10, .user_id = 1, .role_ids = self_roles, .count = 1}};
  ccord_overwrite deny_send[1] = {{.id = 10, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES}};
  ccord_channel_overwrites ow[1] = {{.channel_id = 24, .items = deny_send, .count = 1}};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 6, .members = members, .member_count = 2};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap,
                                    .roles = roles, .role_count = 2, .member_roles = mroles, .member_role_count = 1,
                                    .overwrites = ow, .overwrite_count = 1};
  CT_CHECK(pump_all(s) == 1);

  // Seed timeline + announcement + ephemeral messages.
  ccord_message_rich eph_rich = {.present = CCORD_RICH_ALL, .flags = CCORD_MSGFLAG_EPHEMERAL};
  ccord_message msgs20[4] = {
    {.id = 101, .channel_id = 20, .author_id = 2, .timestamp_ms = 1000, .content = "first"},
    {.id = 102, .channel_id = 20, .author_id = 2, .timestamp_ms = 2000, .content = "second"},
    {.id = 103, .channel_id = 20, .author_id = 1, .timestamp_ms = 3000, .content = "third"},
    {.id = 104, .channel_id = 20, .author_id = 2, .timestamp_ms = 4000, .content = "secret", .rich = &eph_rich},
  };
  ccord_message msgs21[2] = {
    {.id = 201, .channel_id = 21, .author_id = 1, .timestamp_ms = 1000, .content = "my announcement"},
    {.id = 202, .channel_id = 21, .author_id = 2, .timestamp_ms = 2000, .content = "peer announcement"},
  };
  for (size_t k = 0; k < 4; k++) CT_CHECK(ccord_store_append_message(store, &msgs20[k], &err) == CCORD_OK);
  for (size_t k = 0; k < 2; k++) CT_CHECK(ccord_store_append_message(store, &msgs21[k], &err) == CCORD_OK);

  // 1. TTS: text/channel/message-request checks + SEND_TTS_MESSAGES permission enforcement.
  CT_CHECK(ccord_session_send_tts_message(s, 20, "   ", 3, &err) == CCORD_ERR_INVALID);
  // A voice channel carries its own chat, so TTS is no longer refused for its kind (was CCORD_ERR_INVALID).
  CT_CHECK(ccord_session_send_tts_message(s, 22, "hi", 2, &err) != CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_send_tts_message(s, 31, "hi", 2, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_session_send_tts_message(s, 20, "aloud", 5, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_session_send_tts_message(s, 30, "dm tts", 6, &err) == CCORD_OK);
  CT_CHECK(f->tts_count == 1 && f->last_tts_ch == 30 && strcmp(f->last_tts_text, "dm tts") == 0);
  ccord_role grant_tts = {.id = 200, .guild_id = 10, .name = "Extra", .position = 1,
                          .permissions = CCORD_PERM_SEND_TTS_MESSAGES};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = 10, .roles = &grant_tts, .role_count = 1};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_session_send_tts_message(s, 20, "aloud", 5, &err) == CCORD_OK);
  CT_CHECK(f->tts_count == 2 && f->last_tts_ch == 20 && strcmp(f->last_tts_text, "aloud") == 0);

  // 2. Forward: known-message check, ephemeral/local-echo rejection, target SEND_MESSAGES check, happy path.
  CT_CHECK(ccord_session_forward_message(s, 30, 20, 9999, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_forward_message(s, 30, 20, 104, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_forward_message(s, 22, 20, 101, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_forward_message(s, 31, 20, 101, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_session_forward_message(s, 24, 20, 101, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_session_forward_message(s, 30, 20, 102, &err) == CCORD_OK);
  CT_CHECK(f->forward_count == 1 && f->last_fwd_dst == 30 && f->last_fwd_src == 20 && f->last_fwd_msg == 102);

  // 3. Crosspost: announcement channel only; own message needs SEND_MESSAGES, others' needs MANAGE_MESSAGES.
  CT_CHECK(ccord_session_crosspost_message(s, 20, 103, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_crosspost_message(s, 21, 201, &err) == CCORD_OK);
  CT_CHECK(f->crosspost_count == 1 && f->last_xpost_ch == 21 && f->last_xpost_msg == 201);
  CT_CHECK(ccord_session_crosspost_message(s, 21, 202, &err) == CCORD_ERR_PERMISSION);
  ccord_role grant_mod = {.id = 200, .guild_id = 10, .name = "Extra", .position = 1,
                          .permissions = CCORD_PERM_SEND_TTS_MESSAGES | CCORD_PERM_MANAGE_MESSAGES};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = 10, .roles = &grant_mod, .role_count = 1};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_session_crosspost_message(s, 21, 202, &err) == CCORD_OK);
  CT_CHECK(f->crosspost_count == 2 && f->last_xpost_msg == 202);

  // 4. Mark unread: moves store read state only when transport succeeds.
  CT_CHECK(ccord_store_apply_read(store, 20, 103, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(store, 20)->unread_count == 0);
  CT_CHECK(ccord_session_mark_unread(s, 20, 104, &err) == CCORD_ERR_INVALID);   // ephemeral rejected
  f->fail_mark_unread = true;
  CT_CHECK(ccord_session_mark_unread(s, 20, 102, &err) == CCORD_ERR_IO);
  CT_CHECK(ccord_store_channel(store, 20)->unread_count == 0);
  f->fail_mark_unread = false;
  CT_CHECK(ccord_session_mark_unread(s, 20, 102, &err) == CCORD_OK);
  CT_CHECK(f->mark_unread_count == 1 && f->last_unread_ch == 20 && f->last_unread_msg == 102);
  CT_CHECK(ccord_store_channel(store, 20)->unread_count >= 1);

  // 5. Saved messages / bookmarks: LIFO order, ephemeral rejected, unsave removes.
  CT_CHECK(ccord_session_save_message(s, 20, 104, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_save_message(s, 20, 101, &err) == CCORD_OK);
  CT_CHECK(ccord_session_save_message(s, 20, 102, &err) == CCORD_OK);
  CT_CHECK(ccord_session_saved_message_count(s) == 2);
  CT_CHECK(ccord_session_message_saved(s, 20, 101) && ccord_session_message_saved(s, 20, 102));
  const ccord_message *sv0 = ccord_session_saved_message_at(s, 0);
  CT_CHECK(sv0 && sv0->id == 102);
  CT_CHECK(ccord_session_unsave_message(s, 20, 101, &err) == CCORD_OK);
  CT_CHECK(!ccord_session_message_saved(s, 20, 101) && ccord_session_saved_message_count(s) == 1);

  // 6. Interactions: SUB_COMMAND, SUB_COMMAND_GROUP, and User/Message context-menu commands.
  ccord_interaction_request sub_bad = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND, .channel_id = 20, .application_id = 77,
    .command_id = 88, .command_name = "cfg", .command_type = 1, .subcommand_group = "role",
  };
  CT_CHECK(ccord_session_send_interaction(s, &sub_bad, &err) == CCORD_ERR_INVALID);
  ccord_interaction_request sub_ok = sub_bad;
  sub_ok.subcommand = "give";
  CT_CHECK(ccord_session_send_interaction(s, &sub_ok, &err) == CCORD_OK);
  CT_CHECK(f->interaction_count == 1 && f->last_interaction.guild_id == 10 &&
           strcmp(f->last_subcmd_group, "role") == 0 && strcmp(f->last_subcmd, "give") == 0);

  ccord_interaction_request user_cmd = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND, .channel_id = 20, .application_id = 77,
    .command_id = 89, .command_name = "Whois", .command_type = 2, .target_id = 2,
  };
  CT_CHECK(ccord_session_send_interaction(s, &user_cmd, &err) == CCORD_OK);
  CT_CHECK(f->interaction_count == 2 && f->last_interaction.command_type == 2 && f->last_interaction.target_id == 2);

  ccord_interaction_request msg_cmd = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND, .channel_id = 30, .guild_id = CCORD_DM_GUILD_ID,
    .application_id = 77, .command_id = 90, .command_name = "Bookmark", .command_type = 3, .target_id = 102,
  };
  CT_CHECK(ccord_session_send_interaction(s, &msg_cmd, &err) == CCORD_OK);
  CT_CHECK(f->interaction_count == 3 && f->last_interaction.guild_id == CCORD_ID_NONE &&
           f->last_interaction.command_type == 3 && f->last_interaction.target_id == 102);

  ccord_interaction_request bad_ctx = user_cmd;
  bad_ctx.target_id = CCORD_ID_NONE;
  CT_CHECK(ccord_session_send_interaction(s, &bad_ctx, &err) == CCORD_ERR_INVALID);
  bad_ctx = user_cmd;
  bad_ctx.target_id = (1ULL << 63) | 5u;   // local pending echo ID rejected
  CT_CHECK(ccord_session_send_interaction(s, &bad_ctx, &err) == CCORD_ERR_INVALID);
  bad_ctx = user_cmd;
  bad_ctx.subcommand = "nope";
  CT_CHECK(ccord_session_send_interaction(s, &bad_ctx, &err) == CCORD_ERR_INVALID);
  bad_ctx = user_cmd;
  bad_ctx.command_type = 4;
  CT_CHECK(ccord_session_send_interaction(s, &bad_ctx, &err) == CCORD_ERR_INVALID);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_stage_and_scheduled_events(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_parity_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) return;
  f->base.vt = &fake_parity_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_user users[2] = {{.id = 1, .name = "self", .status_text = ""}, {.id = 2, .name = "host", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = 10, .name = "Guild", .owner_id = 2}};
  ccord_channel chans[3] = {
    {.id = 20, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 22, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "Lounge", .topic = ""},
    {.id = 23, .guild_id = 10, .kind = CCORD_CHANNEL_STAGE, .name = "TownHall", .topic = ""},
  };
  ccord_member members[2] = {{.guild_id = 10, .user_id = 1, .role_name = "M"},
                             {.guild_id = 10, .user_id = 2, .role_name = "M"}};
  ccord_role roles[2] = {
    {.id = 10, .guild_id = 10, .name = "@everyone", .permissions = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CONNECT},
    {.id = 200, .guild_id = 10, .name = "Staff", .position = 1, .permissions = 0},
  };
  ccord_id self_roles[1] = {200};
  ccord_member_roles mroles[1] = {{.guild_id = 10, .user_id = 1, .role_ids = self_roles, .count = 1}};
  ccord_scheduled_event seed_ev = {
    .id = 500, .guild_id = 10, .channel_id = 23, .creator_id = 2, .name = "Town Hall",
    .scheduled_start_ms = 2000000, .privacy_level = 2,
    .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE, .user_count = 4,
  };
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 3, .members = members, .member_count = 2};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap,
                                    .roles = roles, .role_count = 2, .member_roles = mroles, .member_role_count = 1,
                                    .scheduled_events = &seed_ev, .scheduled_event_count = 1};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_scheduled_event_count(store, 10) == 1);

  // 1. Stage lifecycle: start & end require a stage channel and MUTE_MEMBERS.
  CT_CHECK(ccord_session_stage_start(s, 10, 22, "Wrong kind", &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_stage_start(s, 10, 23, "Town Hall Live", &err) == CCORD_ERR_PERMISSION);
  ccord_role grant_stage = {.id = 200, .guild_id = 10, .name = "Staff", .position = 1,
                            .permissions = CCORD_PERM_MUTE_MEMBERS};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = 10, .roles = &grant_stage, .role_count = 1};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_session_stage_start(s, 10, 23, "Town Hall Live", &err) == CCORD_OK);
  CT_CHECK(f->admin_count == 1 && f->last_admin.op == CCORD_ADMIN_STAGE_CREATE && f->last_admin.channel_id == 23);

  ccord_stage_instance live_st = {.id = 900, .guild_id = 10, .channel_id = 23, .privacy_level = 2};
  snprintf(live_st.topic, sizeof live_st.topic, "Town Hall Live");
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STAGE_INSTANCE_UPDATE, .channel_id = 23, .stage_instance = &live_st};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_session_stage_instance(s, 23) != nullptr);
  // Duplicate stage_start while an instance is active is rejected locally.
  CT_CHECK(ccord_session_stage_start(s, 10, 23, "Duplicate", &err) == CCORD_ERR_INVALID);
  // stage_end routes via channel_id (req.guild_id == 0) and enforces MUTE_MEMBERS.
  CT_CHECK(ccord_session_stage_end(s, 23, &err) == CCORD_OK);
  CT_CHECK(f->admin_count == 2 && f->last_admin.op == CCORD_ADMIN_STAGE_DELETE && f->last_admin.channel_id == 23);
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STAGE_INSTANCE_UPDATE, .channel_id = 23, .stage_deleted = true};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_session_stage_instance(s, 23) == nullptr);

  // 2. Scheduled events: list, create/modify/delete permissions (creator vs others), RSVP, and Gateway sync.
  CT_CHECK(ccord_session_request_scheduled_events(s, 10, &err) == CCORD_OK);
  CT_CHECK(f->admin_count == 3 && f->last_admin.op == CCORD_ADMIN_EVENT_LIST);

  ccord_scheduled_event new_ev = {
    .guild_id = 10, .channel_id = 22, .name = "Game Night", .scheduled_start_ms = 3000000,
    .entity_type = CCORD_SCHEDULED_EVENT_VOICE, .privacy_level = 2,
  };
  ccord_admin_request creq;
  CT_CHECK(ccord_admin_event_create(&creq, 10, &new_ev, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &creq, &err) == CCORD_ERR_PERMISSION);   // lacks CREATE_EVENTS / MANAGE_EVENTS

  // Grant CREATE_EVENTS (without MANAGE_EVENTS): self can create & edit own event, but NOT user 2's event 500.
  ccord_role grant_create = {.id = 200, .guild_id = 10, .name = "Staff", .position = 1,
                             .permissions = CCORD_PERM_MUTE_MEMBERS | CCORD_PERM_CREATE_EVENTS};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = 10, .roles = &grant_create, .role_count = 1};
  CT_CHECK(pump_all(s) == 1);
  // Wrong channel kind for VOICE event (stage channel 23) rejected:
  ccord_scheduled_event bad_kind = new_ev;
  bad_kind.channel_id = 23;
  CT_CHECK(ccord_admin_event_create(&creq, 10, &bad_kind, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &creq, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_event_create(&creq, 10, &new_ev, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &creq, &err) == CCORD_OK);
  CT_CHECK(f->last_admin.op == CCORD_ADMIN_EVENT_CREATE);

  ccord_scheduled_event own_ev = new_ev;
  own_ev.id = 501;
  own_ev.creator_id = 1;
  own_ev.status = CCORD_SCHEDULED_EVENT_SCHEDULED;
  own_ev.user_count = 1;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SCHEDULED_EVENT_UPDATE, .guild_id = 10,
                                    .scheduled_events = &own_ev, .scheduled_event_count = 1};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_scheduled_event_count(store, 10) == 2);

  ccord_admin_event_edit edit = CCORD_ADMIN_EVENT_EDIT_INIT;
  edit.name = "Renamed";
  ccord_admin_request mreq, dreq;
  CT_CHECK(ccord_admin_event_modify(&mreq, 10, 501, &edit, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &mreq, &err) == CCORD_OK);   // own event allowed with CREATE_EVENTS
  CT_CHECK(ccord_admin_event_modify(&mreq, 10, 500, &edit, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &mreq, &err) == CCORD_ERR_PERMISSION);   // user 2's event needs MANAGE_EVENTS
  CT_CHECK(ccord_admin_event_delete(&dreq, 10, 500, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &dreq, &err) == CCORD_ERR_PERMISSION);

  ccord_role grant_manage = {.id = 200, .guild_id = 10, .name = "Staff", .position = 1,
                             .permissions = CCORD_PERM_MUTE_MEMBERS | CCORD_PERM_MANAGE_EVENTS};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = 10, .roles = &grant_manage, .role_count = 1};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_session_admin(s, &mreq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &dreq, &err) == CCORD_OK);

  // RSVP intents + Gateway SCHEDULED_EVENT_USER_CHANGE (self vs peer) + delete sync.
  CT_CHECK(ccord_session_rsvp_scheduled_event(s, 10, 500, true, &err) == CCORD_OK);
  CT_CHECK(f->last_admin.op == CCORD_ADMIN_EVENT_RSVP_ADD);
  CT_CHECK(ccord_session_rsvp_scheduled_event(s, 10, 500, false, &err) == CCORD_OK);
  CT_CHECK(f->last_admin.op == CCORD_ADMIN_EVENT_RSVP_REMOVE);

  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, .guild_id = 10,
                                    .scheduled_event_id = 500, .user_id = 1, .scheduled_event_user_added = true};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, .guild_id = 10,
                                    .scheduled_event_id = 500, .user_id = 2, .scheduled_event_user_added = true};
  CT_CHECK(pump_all(s) == 2);
  const ccord_scheduled_event *ev500 = ccord_store_find_scheduled_event(store, 500);
  CT_CHECK(ev500 && ev500->me_rsvped && ev500->user_count == 6);

  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SCHEDULED_EVENT_UPDATE, .guild_id = 10,
                                    .scheduled_event_id = 500, .scheduled_event_deleted = true};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_find_scheduled_event(store, 500) == nullptr);
  CT_CHECK(ccord_store_scheduled_event_count(store, 10) == 1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// ---- Rich Presence session API: stored in the session, pushed through the transport, re-pushed on a new SNAPSHOT ----

typedef struct fake_rich_transport {
  ccord_transport base;
  ccord_net_event ev[4];
  size_t n, i;
  // set_rich_activities recording: the array last accepted (empty = cleared) and how many pushes reached the hook.
  char rich[512];
  size_t rich_len;
  uint32_t rich_push_count;
  bool refuse_rich;
  // presence broadcasts (set_presence / set_presence_ex): how many, and the last one.
  uint32_t presence_count;
  ccord_presence last_presence;
  char last_custom[64];
} fake_rich_transport;

static ccord_status fake_rich_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool fake_rich_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_rich_transport *f = (fake_rich_transport *)t;
  if (f->i >= f->n) return false;
  *out = f->ev[f->i++];
  return true;
}

static void fake_rich_destroy(ccord_transport *t)
{
  free(t);
}

static ccord_status fake_rich_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)limit;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_rich_send(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                   ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)utf8;
  (void)len;
  return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "fake: send");
}

static ccord_status fake_rich_set_presence(ccord_transport *t, ccord_presence presence, const char *status_text,
                                           ccord_error *err)
{
  fake_rich_transport *f = (fake_rich_transport *)t;
  f->presence_count++;
  f->last_presence = presence;
  snprintf(f->last_custom, sizeof f->last_custom, "%s", status_text ? status_text : "");
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_rich_set_presence_ex(ccord_transport *t, ccord_presence presence, const char *emoji_name,
                                              ccord_id emoji_id, const char *text, ccord_error *err)
{
  (void)emoji_name;
  (void)emoji_id;
  return fake_rich_set_presence(t, presence, text, err);
}

static ccord_status fake_rich_set_activities(ccord_transport *t, const char *json, size_t len, ccord_error *err)
{
  fake_rich_transport *f = (fake_rich_transport *)t;
  if (f->refuse_rich) return ccord_error_set(err, CCORD_ERR_INVALID, "fake: bad activities");
  if (len >= sizeof f->rich) return ccord_error_set(err, CCORD_ERR_RANGE, "fake: activities too large");
  if (json && len) memcpy(f->rich, json, len);
  f->rich_len = json ? len : 0;
  f->rich[f->rich_len] = '\0';
  f->rich_push_count++;
  ccord_error_clear(err);
  return CCORD_OK;
}

static const ccord_transport_vtable fake_rich_vt = {
  .name = "fake-rich",
  .connect = fake_rich_connect,
  .request_history = fake_rich_history,
  .send_message = fake_rich_send,
  .poll = fake_rich_poll,
  .destroy = fake_rich_destroy,
  .set_presence = fake_rich_set_presence,
  .set_presence_ex = fake_rich_set_presence_ex,
  .set_rich_activities = fake_rich_set_activities,
};

static void test_session_rich_presence(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_rich_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) return;
  f->base.vt = &fake_rich_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_user users[1] = {{.id = 1, .name = "me", .status_text = "", .presence = CCORD_PRESENCE_ONLINE}};
  ccord_guild guilds[1] = {{.id = 10, .name = "G"}};
  ccord_channel chans[1] = {{.id = 20, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""}};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 1, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 1};
  static const char activity[] = "[{\"name\":\"C-Cord\",\"type\":0,\"details\":\"coding\"}]";
  static const char activity2[] = "[{\"name\":\"Other\",\"type\":2}]";

  // Invalid input never reaches the transport; a null session is refused.
  CT_CHECK(ccord_session_set_rich_presence(nullptr, activity, &err) == CCORD_ERR_INVALID);

  // Before CONNECTED the array is pushed (the live gateway stores it) but no presence is broadcast.
  CT_CHECK(ccord_session_set_rich_presence(s, activity, &err) == CCORD_OK);
  CT_CHECK(f->rich_push_count == 1 && strcmp(f->rich, activity) == 0 && f->presence_count == 0);

  // CONNECTED: the SNAPSHOT re-pushes the stored array and rebroadcasts the presence.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTED};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) == 2);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTED);
  CT_CHECK(f->rich_push_count == 2 && strcmp(f->rich, activity) == 0);
  CT_CHECK(f->presence_count == 1 && f->last_presence == CCORD_PRESENCE_ONLINE);

  // A new set while CONNECTED is recorded on the transport and resends the presence at once.
  CT_CHECK(ccord_session_set_rich_presence(s, activity2, &err) == CCORD_OK);
  CT_CHECK(f->rich_push_count == 3 && strcmp(f->rich, activity2) == 0);
  CT_CHECK(f->presence_count == 2 && f->last_presence == CCORD_PRESENCE_ONLINE);

  // A status change keeps the activity: the transport is not asked to clear it, and the presence rebuild
  // (which splices the transport-held array) carries the new status.
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_IDLE, "away", &err) == CCORD_OK);
  CT_CHECK(f->presence_count == 3 && f->last_presence == CCORD_PRESENCE_IDLE && strcmp(f->last_custom, "away") == 0);
  CT_CHECK(f->rich_push_count == 3 && strcmp(f->rich, activity2) == 0);
  // The resend after a rich-only change keeps the status the user chose (IDLE pending until the server echoes).
  CT_CHECK(ccord_session_set_rich_presence(s, activity, &err) == CCORD_OK);
  CT_CHECK(f->rich_push_count == 4 && strcmp(f->rich, activity) == 0);
  CT_CHECK(f->presence_count == 4 && f->last_presence == CCORD_PRESENCE_IDLE);

  // A rejected set keeps both the stored array and the transport's.
  f->refuse_rich = true;
  CT_CHECK(ccord_session_set_rich_presence(s, "not json", &err) == CCORD_ERR_INVALID);
  f->refuse_rich = false;
  CT_CHECK(f->rich_push_count == 4 && f->presence_count == 4 && strcmp(f->rich, activity) == 0);

  // Reconnect: a fresh gateway starts empty; the next SNAPSHOT restores the activity from the session.
  f->rich_len = 0;
  f->rich[0] = '\0';
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(f->rich_push_count == 5 && strcmp(f->rich, activity) == 0);
  CT_CHECK(f->presence_count == 5);

  // NULL clears: pushed to the transport as an empty set, stored array dropped, presence resent without it.
  CT_CHECK(ccord_session_set_rich_presence(s, nullptr, &err) == CCORD_OK);
  CT_CHECK(f->rich_push_count == 6 && f->rich_len == 0 && f->presence_count == 6);
  // Cleared means a later reconnect pushes nothing and sends no extra presence from this path.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(f->rich_push_count == 6 && f->rich_len == 0 && f->presence_count == 6);
  // "" clears as well (idempotent).
  CT_CHECK(ccord_session_set_rich_presence(s, "", &err) == CCORD_OK);
  CT_CHECK(f->rich_len == 0 && f->rich_push_count == 7);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Embedded activities through the mock: the shelf cache, launch (voice-gated), leave, the ghost-participant cleanup
// on a voice disconnect, and instances of other users arriving as updates.
static bool activity_has(const ccord_activity_instance *inst, ccord_id user)
{
  for (size_t i = 0; inst && i < inst->participant_count; i++) {
    if (inst->participants[i] == user) return true;
  }
  return false;
}

static void test_session_activities_mock(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mcfg = {.seed = 53, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&mcfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(bus && store && tr && s);
  if (!bus || !store || !tr || !s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTED);

  const ccord_id guild = 2001, voice_ch = 3121, other_voice = 3122, text_ch = 3111;
  const ccord_id sketch = 97001, poker = 97002, watch = 97003;
  const ccord_id self = ccord_store_self_id(store), other_user = self + 1;
  size_t n = 99;

  // Shelf: empty until the answer arrives, then three apps in shelf order; the cache serves a repeat ask.
  CT_CHECK(ccord_session_activity_shelf(s, guild, &n) == nullptr && n == 0);
  CT_CHECK(ccord_session_activity_shelf_fetch(s, guild, &err) == CCORD_OK);
  CT_CHECK(ccord_session_activity_shelf_fetch(s, guild, &err) == CCORD_OK);   // still loading: a no-op
  size_t admins = ccord_mock_admin_count(tr);
  pump_all(s);
  const ccord_activity_app *shelf = ccord_session_activity_shelf(s, guild, &n);
  CT_CHECK(shelf != nullptr && n == 3);
  if (shelf && n == 3) {
    CT_CHECK(shelf[0].application_id == sketch && strcmp(shelf[0].name, "Sketch Heads") == 0);
    CT_CHECK(shelf[1].application_id == poker && shelf[2].application_id == watch);
    CT_CHECK(shelf[0].description[0] != '\0' && shelf[0].max_participants > 0);
  }
  CT_CHECK(ccord_session_activity_shelf_fetch(s, guild, &err) == CCORD_OK);   // fresh: served from the cache
  CT_CHECK(ccord_mock_admin_count(tr) == admins);
  CT_CHECK(ccord_session_activity_shelf(s, guild + 1, &n) == nullptr && n == 0);   // per guild
  CT_CHECK(ccord_session_activity_shelf_fetch(s, 424242, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_activity_shelf_fetch(s, CCORD_DM_GUILD_ID, &err) == CCORD_ERR_INVALID);

  // Launch needs the voice connection: not connected, wrong kind and unknown channel are all refused locally.
  CT_CHECK(ccord_session_activity_launch(s, voice_ch, sketch, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_activity_instances(s, voice_ch, &n) == nullptr && n == 0);
  CT_CHECK(ccord_session_activity_launch(s, text_ch, sketch, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_activity_launch(s, 424242, sketch, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_activity_launch(s, voice_ch, CCORD_ID_NONE, &err) != CCORD_OK);

  // Connected to voice channel A: launching adds self to the channel's instance; a repeat joins the same one.
  CT_CHECK(ccord_session_join_voice(s, guild, voice_ch, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_activity_launch(s, other_voice, sketch, &err) == CCORD_ERR_INVALID);   // not THAT channel
  CT_CHECK(ccord_session_activity_launch(s, voice_ch, sketch, &err) == CCORD_OK);
  pump_all(s);
  const ccord_activity_instance *rows = ccord_session_activity_instances(s, voice_ch, &n);
  CT_CHECK(rows != nullptr && n == 1);
  ccord_activity_instance sketch_inst = {0};
  if (rows && n == 1) {
    sketch_inst = rows[0];
    CT_CHECK(rows[0].application_id == sketch && rows[0].channel_id == voice_ch && rows[0].guild_id == guild);
    CT_CHECK(rows[0].location_id == voice_ch && rows[0].instance_id != CCORD_ID_NONE);
    CT_CHECK(rows[0].participant_count == 1 && activity_has(&rows[0], self));
    CT_CHECK(ccord_store_find_activity_instance(store, rows[0].instance_id) == &rows[0]);
  }
  CT_CHECK(ccord_session_activity_launch(s, voice_ch, sketch, &err) == CCORD_OK);
  pump_all(s);
  rows = ccord_session_activity_instances(s, voice_ch, &n);
  CT_CHECK(n == 1 && rows && rows[0].instance_id == sketch_inst.instance_id && rows[0].participant_count == 1);
  CT_CHECK(ccord_session_activity_launch(s, voice_ch, poker, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_activity_instances(s, voice_ch, &n) != nullptr && n == 2);

  // Discord refuses an unknown app: the instance list is untouched and the failure surfaces.
  ccord_action_failure fail;
  while (ccord_session_pop_failure(s, &fail)) ccord_action_failure_free(&fail);
  CT_CHECK(ccord_session_activity_launch(s, voice_ch, 12345, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_pop_failure(s, &fail));
  CT_CHECK(fail.action == CCORD_ACTION_ADMIN && fail.admin_op == CCORD_ADMIN_ACTIVITY_LAUNCH &&
           fail.status == CCORD_ERR_NOT_FOUND);
  ccord_action_failure_free(&fail);
  CT_CHECK(ccord_session_activity_instances(s, voice_ch, &n) != nullptr && n == 2);

  // Leave: the instance disappears when its last participant goes; an unknown instance is refused locally.
  CT_CHECK(ccord_session_activity_leave(s, &sketch_inst, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_find_activity_instance(store, sketch_inst.instance_id) == nullptr);
  rows = ccord_session_activity_instances(s, voice_ch, &n);
  CT_CHECK(rows != nullptr && n == 1 && rows[0].application_id == poker);
  CT_CHECK(ccord_session_activity_leave(s, &sketch_inst, &err) == CCORD_ERR_NOT_FOUND);   // already gone
  CT_CHECK(ccord_session_activity_leave(s, nullptr, &err) == CCORD_ERR_INVALID);

  // Another user's instance arrives as an update (self is not a participant there).
  const ccord_id others[] = {other_user};
  ccord_id watch_id = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_push_activity_instance(tr, other_voice, watch, others, 1, &watch_id, &err) == CCORD_OK);
  pump_all(s);
  rows = ccord_session_activity_instances(s, other_voice, &n);
  CT_CHECK(rows != nullptr && n == 1 && rows[0].instance_id == watch_id && !activity_has(&rows[0], self));
  CT_CHECK(activity_has(&rows[0], other_user));

  // Ghost participant: the user is in two instances; a voice disconnect makes the session send the leave for the
  // one it is still in, so the other participant stays and we are gone. Nothing is sent for an instance it is not in.
  const ccord_id poker_both[] = {other_user, self};
  CT_CHECK(ccord_mock_push_activity_instance(tr, voice_ch, poker, poker_both, 2, nullptr, &err) == CCORD_OK);
  pump_all(s);
  rows = ccord_session_activity_instances(s, voice_ch, &n);
  CT_CHECK(rows != nullptr && n == 1 && rows[0].participant_count == 2 && activity_has(&rows[0], self));
  size_t before_leave = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_session_leave_voice(s, guild, &err) == CCORD_OK);
  pump_all(s);
  rows = ccord_session_activity_instances(s, voice_ch, &n);
  CT_CHECK(rows != nullptr && n == 1 && rows[0].participant_count == 1);
  CT_CHECK(!activity_has(&rows[0], self) && activity_has(&rows[0], other_user));
  CT_CHECK(ccord_mock_admin_count(tr) == before_leave + 1u);   // exactly one leave, for the instance we were in
  rows = ccord_session_activity_instances(s, other_voice, &n);
  CT_CHECK(rows != nullptr && n == 1 && rows[0].instance_id == watch_id);   // untouched

  // Out of voice again: launching is refused.
  CT_CHECK(ccord_session_activity_launch(s, voice_ch, sketch, &err) == CCORD_ERR_INVALID);

  // A reconnect keeps the server's running instances in the store.
  ccord_transport_disconnect(tr);
  pump_all(s);
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_activity_instances(s, voice_ch, &n) != nullptr && n == 1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Same API on the mock transport: recorded, kept across a status change, cleared by NULL, restored after reconnect.
static void test_session_rich_presence_mock(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mcfg = {.seed = 47, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&mcfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(bus && store && tr && s);
  if (!bus || !store || !tr || !s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTED);

  static const char activity[] = "[{\"name\":\"C-Cord\",\"type\":0}]";
  size_t len = 99;
  CT_CHECK(ccord_mock_rich_activities(tr, &len) == nullptr && len == 0);
  CT_CHECK(ccord_session_set_rich_presence(s, activity, &err) == CCORD_OK);
  const char *got = ccord_mock_rich_activities(tr, &len);
  CT_CHECK(got && len == strlen(activity) && memcmp(got, activity, len) == 0);

  // A status change keeps it.
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_DND, "busy", &err) == CCORD_OK);
  pump_all(s);
  got = ccord_mock_rich_activities(tr, &len);
  CT_CHECK(got && len == strlen(activity) && memcmp(got, activity, len) == 0);

  // A malformed array is rejected by the transport and the stored one survives.
  CT_CHECK(ccord_session_set_rich_presence(s, "{\"name\":\"x\"}", &err) == CCORD_ERR_INVALID);
  got = ccord_mock_rich_activities(tr, &len);
  CT_CHECK(got && len == strlen(activity) && memcmp(got, activity, len) == 0);

  // Reconnect: the new gateway forgot the activity (simulated by clearing it on the transport); the session
  // restores it from its own copy when the next SNAPSHOT arrives.
  CT_CHECK(ccord_transport_set_rich_activities(tr, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_rich_activities(tr, &len) == nullptr);
  ccord_transport_disconnect(tr);
  pump_all(s);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_DISCONNECTED);
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTED);
  got = ccord_mock_rich_activities(tr, &len);
  CT_CHECK(got && len == strlen(activity) && memcmp(got, activity, len) == 0);

  // NULL clears it, and it stays cleared across a further reconnect.
  CT_CHECK(ccord_session_set_rich_presence(s, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_rich_activities(tr, &len) == nullptr && len == 0);
  ccord_transport_disconnect(tr);
  pump_all(s);
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_mock_rich_activities(tr, &len) == nullptr && len == 0);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Membership screening end to end over the mock: the canned "mv-test" invite joins a guild whose self member is
// pending; sends are refused with a distinct error until the rules form is accepted, and the gate reopens only when
// the transport confirms (a refused PUT leaves the member pending).
static void test_session_membership_screening(void)
{
  enum { MV_GUILD = 9101 };
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 61, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  CT_CHECK(!ccord_session_self_pending(s, MV_GUILD) && !ccord_session_self_pending(nullptr, MV_GUILD));
  CT_CHECK(!ccord_session_self_pending(s, CCORD_ID_NONE) && !ccord_session_self_pending(s, CCORD_DM_GUILD_ID));
  CT_CHECK(ccord_session_member_verification(s, MV_GUILD) == nullptr);
  CT_CHECK(ccord_session_member_verification_submit(s, MV_GUILD, &err) == CCORD_ERR_NOT_FOUND);   // no cached form

  // Joining through the canned invite leaves the signed-in member pending.
  ccord_admin_request req;
  CT_CHECK(ccord_admin_accept_invite(&req, "mv-test", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_guild(store, MV_GUILD) != nullptr);
  CT_CHECK(ccord_session_self_pending(s, MV_GUILD));
  const ccord_member *self_m = ccord_store_find_member(store, MV_GUILD, ccord_store_self_id(store));
  CT_CHECK(self_m && self_m->pending);
  ccord_id text_ch = CCORD_ID_NONE;
  for (size_t i = 0; ccord_store_channel_at(store, MV_GUILD, i); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, MV_GUILD, i);
    if (c->kind == CCORD_CHANNEL_TEXT) text_ch = c->id;
  }
  CT_CHECK(text_ch != CCORD_ID_NONE);

  // Every send path in the guild is refused with the distinct screening error; nothing reaches the transport.
  ccord_error serr;
  CT_CHECK(ccord_session_send_message(s, text_ch, "hello", 5, &serr) == CCORD_ERR_PERMISSION);
  CT_CHECK(strstr(serr.message, "screening") != nullptr);
  CT_CHECK(ccord_session_send_tts_message(s, text_ch, "hello", 5, &serr) == CCORD_ERR_PERMISSION);
  CT_CHECK(strstr(serr.message, "screening") != nullptr);
  CT_CHECK(ccord_session_send_sticker(s, text_ch, 1, &serr) == CCORD_ERR_PERMISSION);
  CT_CHECK(strstr(serr.message, "screening") != nullptr);

  // The form arrives through the transport and is cached per guild in the session.
  CT_CHECK(ccord_session_member_verification_fetch(s, MV_GUILD, &err) == CCORD_OK);
  CT_CHECK(ccord_session_member_verification(s, MV_GUILD) == nullptr);   // nothing changes until the answer arrives
  pump_all(s);
  const ccord_member_verification *form = ccord_session_member_verification(s, MV_GUILD);
  CT_CHECK(form && form->field_count == 1 && form->version && form->version[0] != '\0');
  if (form && form->field_count == 1) {
    CT_CHECK(form->fields[0].field_type == CCORD_MEMBER_VERIFICATION_TERMS && form->fields[0].required);
    CT_CHECK(form->fields[0].value_count == 3 && strcmp(form->fields[0].label, "Server rules") == 0);
    CT_CHECK(form->description && strstr(form->description, "Rules Lounge") != nullptr);
  }
  CT_CHECK(ccord_session_member_verification(s, MV_GUILD + 1) == nullptr);   // per guild
  CT_CHECK(ccord_session_self_pending(s, MV_GUILD));                          // fetching changes nothing

  // A guild without screening has no form: the failure leaves the cache empty and the member unaffected.
  CT_CHECK(ccord_session_member_verification_fetch(s, 2001, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_member_verification(s, 2001) == nullptr);

  // A refused submit (Discord answers an error) does not clear anything.
  ccord_mock_fail_next_admin(tr, true);
  CT_CHECK(ccord_session_member_verification_submit(s, MV_GUILD, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_self_pending(s, MV_GUILD));
  CT_CHECK(ccord_session_send_message(s, text_ch, "hello", 5, &serr) == CCORD_ERR_PERMISSION);

  // The accepted submit is a PUT carrying the form; the transport's confirmation clears pending and reopens sending.
  size_t admin_calls = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_session_member_verification_submit(s, MV_GUILD, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == admin_calls + 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION && last->guild_id == MV_GUILD);
  CT_CHECK(ccord_session_self_pending(s, MV_GUILD));   // not optimistic: still pending until the answer is applied
  uint64_t rev = ccord_store_revision(store);
  pump_all(s);
  CT_CHECK(!ccord_session_self_pending(s, MV_GUILD));
  CT_CHECK(ccord_store_revision(store) > rev);
  CT_CHECK(ccord_session_send_message(s, text_ch, "hello", 5, &serr) == CCORD_OK);
  CT_CHECK(ccord_session_member_verification(s, MV_GUILD) != nullptr);   // the cache outlives the pending flag

  // Other guilds were never gated.
  const ccord_guild *g0 = ccord_store_guild_at(store, 0);
  CT_CHECK(g0 && !ccord_session_self_pending(s, g0->id));

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Voice messages: one Ogg/Opus upload named voice-message.ogg with duration_ms + waveform_b64 on the message
// (flag 8192), gated by Send Messages + Attach Files. The mock echoes the upload as MESSAGE_CREATE, so the stored
// message proves what reached the transport.
static void test_session_send_voice_message(void)
{
  enum { GUILD = 2003, CHANNEL = 3311 };
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 71, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  ccord_id self = ccord_store_self_id(store);
  CT_CHECK(self != CCORD_ID_NONE);
  CT_CHECK(ccord_store_channel(store, CHANNEL) != nullptr);
  CT_CHECK(ccord_store_permissions_exact(store, GUILD, self));   // the mock sent the role table and self's roles

  // @everyone may send here but not attach files (self is an ordinary member of this guild).
  const uint64_t base = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY;
  ccord_role everyone = {.id = GUILD, .guild_id = GUILD, .name = "@everyone", .permissions = base};
  CT_CHECK(ccord_store_upsert_role(store, &everyone, &err) == CCORD_OK);
  CT_CHECK((ccord_store_permissions(store, GUILD, CHANNEL, self, 0) & CCORD_PERM_SEND_MESSAGES) != 0);
  CT_CHECK((ccord_store_permissions(store, GUILD, CHANNEL, self, 0) & CCORD_PERM_ATTACH_FILES) == 0);

  static const uint8_t clip[] = {'O', 'g', 'g', 'S', 0, 2, 0, 0, 1, 2, 3, 4};
  size_t before = ccord_store_message_count(store, CHANNEL);
  // An empty clip or an unknown duration is refused before anything else.
  CT_CHECK(ccord_session_send_voice_message(s, CHANNEL, clip, 0, 900, nullptr, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_send_voice_message(s, CHANNEL, clip, sizeof clip, 0, nullptr, &err) == CCORD_ERR_INVALID);
  // Missing Attach Files: permission error, nothing posted.
  CT_CHECK(ccord_session_send_voice_message(s, CHANNEL, clip, sizeof clip, 900, "AQIDBAUG", &err) ==
           CCORD_ERR_PERMISSION);
  CT_CHECK(strstr(err.message, "Attach Files") != nullptr);
  pump_all(s);
  CT_CHECK(ccord_store_message_count(store, CHANNEL) == before);

  // With Attach Files the clip is one upload carrying its duration and waveform.
  everyone.permissions = base | CCORD_PERM_ATTACH_FILES;
  CT_CHECK(ccord_store_upsert_role(store, &everyone, &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_voice_message(s, CHANNEL, clip, sizeof clip, 4321, "AQIDBAUG", &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_message_count(store, CHANNEL) == before + 1);
  const ccord_message *m = ccord_store_message_at(store, CHANNEL, before);
  CT_CHECK(m && m->author_id == self && m->rich != nullptr);
  CT_CHECK(m && m->rich && (m->rich->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0);
  CT_CHECK(m && m->rich && m->rich->attachment_count == 1);
  if (m && m->rich && m->rich->attachment_count == 1) {
    const ccord_attachment *a = &m->rich->attachments[0];
    CT_CHECK(strcmp(a->filename, "voice-message.ogg") == 0);
    CT_CHECK(strcmp(a->content_type, "audio/ogg") == 0);
    CT_CHECK(a->size == sizeof clip);
    CT_CHECK(a->duration_ms == 4321);
    CT_CHECK(a->waveform_b64 && strcmp(a->waveform_b64, "AQIDBAUG") == 0);
  }

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_camera_passthrough(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 72, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  char key[CCORD_CAM_TEXTURE_KEY_MAX];
  ccord_session_camera_texture_key(1005, key, sizeof key);
  CT_CHECK(strcmp(key, "__cam__:1005") == 0);

  static const uint8_t au_a[] = {0, 0, 0, 1, 0x65, 0xA1, 0xA2, 0xA3};
  static const uint8_t au_b[] = {0, 0, 0, 1, 0x41, 0xB1};
  ccord_camera_rx rx[CCORD_CAM_RX_MAX];
  const uint8_t *data = nullptr;
  size_t len = 0;
  bool key_frame = false;
  // Outside voice: nothing, even with a sender announced.
  CT_CHECK(ccord_mock_set_remote_camera(tr, 2001, 3121, 1005, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_set_remote_camera(tr, 2001, 3121, 1006, true, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_camera_senders(s, rx, CCORD_CAM_RX_MAX) == 0);
  CT_CHECK(!ccord_session_camera_poll_frame(s, 1005, &data, &len, &key_frame) && data == nullptr && len == 0);

  // In voice: both senders are listed and a unit pushed for 1005 comes back for 1005 only, byte-identical.
  CT_CHECK(ccord_session_join_voice(s, 2001, 3121, &err) == CCORD_OK);
  pump_all(s);
  size_t n = ccord_session_camera_senders(s, rx, CCORD_CAM_RX_MAX);
  CT_CHECK(n == 2 && rx[0].decoding && rx[1].decoding);
  CT_CHECK(ccord_mock_push_camera_frame(tr, 1005, au_a, sizeof au_a, 10, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_push_camera_frame(tr, 1006, au_b, sizeof au_b, 20, false, &err) == CCORD_OK);
  CT_CHECK(!ccord_session_camera_poll_frame(s, 1007, &data, &len, &key_frame));
  CT_CHECK(ccord_session_camera_poll_frame(s, 1005, &data, &len, &key_frame));
  CT_CHECK(len == sizeof au_a && data && memcmp(data, au_a, len) == 0 && key_frame);
  CT_CHECK(!ccord_session_camera_poll_frame(s, 1005, &data, &len, &key_frame));   // 1005's queue is empty
  CT_CHECK(ccord_session_camera_poll_frame(s, 1006, &data, &len, &key_frame));
  CT_CHECK(len == sizeof au_b && memcmp(data, au_b, len) == 0 && !key_frame);

  // Leaving voice: back to nothing.
  CT_CHECK(ccord_session_leave_voice(s, 2001, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_camera_senders(s, rx, CCORD_CAM_RX_MAX) == 0);
  CT_CHECK(!ccord_session_camera_poll_frame(s, 1005, &data, &len, &key_frame));

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}


static void test_session_guild_profile_and_notification_filters(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_transport *mock = ccord_mock_transport_create(&(ccord_mock_config){.seed = 42, .latency_ms = 0}, &err);
  ccord_session *s = ccord_session_create(bus, store, mock, &err);
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  for (int i = 0; i < 64 && ccord_session_state(s) != CCORD_CONN_CONNECTED; i++) ccord_session_pump(s);
  ccord_session_pump(s);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTED);

  size_t ng = ccord_store_guild_count(store);
  ccord_id gid = CCORD_ID_NONE;
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *g = ccord_store_guild_at(store, i);
    if (g && g->id != CCORD_DM_GUILD_ID) { gid = g->id; break; }
  }
  CT_CHECK(gid != CCORD_ID_NONE);
  CT_CHECK(ccord_session_guild_profile_fetch(s, gid, &err) == CCORD_OK);
  for (int i = 0; i < 16 && !ccord_session_guild_profile(s, gid); i++) ccord_session_pump(s);
  const ccord_guild_profile *gp = ccord_session_guild_profile(s, gid);
  CT_CHECK(gp && gp->guild_id == gid && !gp->unavailable && gp->tag[0] != '\0');
  CT_CHECK(!ccord_session_guild_profile_loading(s, gid));

  // Invite preview lookup/fetch by code: valid invite loads preview; bad invite settles as failed.
  bool inv_load = false, inv_fail = false;
  CT_CHECK(ccord_session_invite_lookup(s, "https://discord.gg/c23-study", &inv_load, &inv_fail) == nullptr &&
           !inv_load && !inv_fail);
  CT_CHECK(ccord_session_invite_fetch(s, "https://discord.gg/c23-study", &err) == CCORD_OK);
  for (int i = 0; i < 16 && !ccord_session_invite_lookup(s, "c23-study", &inv_load, &inv_fail); i++) {
    ccord_session_pump(s);
  }
  const ccord_invite_preview *ip = ccord_session_invite_lookup(s, "discord.gg/c23-study", &inv_load, &inv_fail);
  CT_CHECK(ip && strcmp(ip->code, "c23-study") == 0 && ip->guild_id != CCORD_ID_NONE && !inv_fail);

  CT_CHECK(ccord_session_invite_fetch(s, "discord.gg/no-such-code", &err) == CCORD_OK);
  for (int i = 0; i < 16; i++) ccord_session_pump(s);
  CT_CHECK(ccord_session_invite_lookup(s, "no-such-code", &inv_load, &inv_fail) == nullptr && !inv_load && inv_fail);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);

  // Test SESSIONS_REPLACE clearing status_text, case-insensitive rich presence clear, and RESUMED Op 3 flush.
  bus = ccord_bus_create(&err);
  store = ccord_store_create(bus, &err);
  fake_rich_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) return;
  f->base.vt = &fake_rich_vt;
  s = ccord_session_create(bus, store, &f->base, &err);
  ccord_user users[2] = {
    {.id = 1, .name = "me", .status_text = "Playing Tipsy", .presence = CCORD_PRESENCE_ONLINE,
     .has_activity = true, .activity_type = CCORD_ACTIVITY_PLAYING, .activity_name = "Tipsy"},
    {.id = 2, .name = "other", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "G"}};
  ccord_channel chans[2] = {
    {.id = 20, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 30, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "dm", .topic = "",
     .is_message_request = true},
  };
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 2};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTED};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) == 2);
  f->i = f->n = 0;

  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SESSIONS_REPLACE, .presence = CCORD_PRESENCE_ONLINE,
                                    .has_presence_detail = true};
  CT_CHECK(pump_all(s) == 1);
  f->i = f->n = 0;
  const ccord_user *me = ccord_store_user(store, 1);
  CT_CHECK(me && !me->has_activity && me->status_text[0] == '\0');

  uint32_t pres_before = f->presence_count;
  CT_CHECK(ccord_session_set_rich_presence(s, "[{\"name\":\"tipsy\",\"type\":0}]", &err) == CCORD_OK);
  CT_CHECK(f->presence_count == pres_before + 1);
  // Case-insensitive rich presence clear while stale: dirty flag queues flush on RESUMED.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTING};
  CT_CHECK(pump_all(s) == 1);
  f->i = f->n = 0;
  CT_CHECK(ccord_session_set_rich_presence(s, nullptr, &err) == CCORD_OK);
  CT_CHECK(f->presence_count == pres_before + 1);
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTED};
  CT_CHECK(pump_all(s) == 1);
  f->i = f->n = 0;
  CT_CHECK(f->presence_count == pres_before + 2);

  // Message request DM does not increment unread_count or mention_count.
  ccord_message dm_msg = {.id = 500, .channel_id = 30, .author_id = 2, .content = "hi"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .channel_id = 30,
                                    .messages = &dm_msg, .message_count = 1};
  CT_CHECK(pump_all(s) == 1);
  f->i = f->n = 0;
  const ccord_channel *dmc = ccord_store_channel(store, 30);
  CT_CHECK(dmc && dmc->unread_count == 0 && dmc->mention_count == 0);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_supplemental_passive_and_guild_voice_states(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_rich_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) { free(f); return; }
  f->base.vt = &fake_rich_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);

  ccord_user users[2] = {
    {.id = 1, .name = "Self", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Alice", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[2] = {
    {.id = 10, .name = "Alpha"},
    {.id = 20, .name = "Beta"},
  };
  ccord_channel chans[4] = {
    {.id = 11, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "chat", .topic = ""},
    {.id = 12, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "Lounge", .topic = ""},
    {.id = 21, .guild_id = 20, .kind = CCORD_CHANNEL_VOICE, .name = "Gaming", .topic = ""},
    {.id = 22, .guild_id = 20, .kind = CCORD_CHANNEL_STAGE, .name = "Town Hall", .topic = ""},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 2,
    .guilds = guilds, .guild_count = 2,
    .channels = chans, .channel_count = 4,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTED};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};

  // 1. READY_SUPPLEMENTAL delivers voice states across both guilds (including unknown user 3 -> stub).
  ccord_voice_state supp_vs[3] = {
    {.guild_id = 10, .channel_id = 12, .user_id = 2},
    {.guild_id = 20, .channel_id = 21, .user_id = 3},
    {.guild_id = 20, .channel_id = 22, .user_id = 2, .suppress = true},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATES, .guild_id = CCORD_ID_NONE,
                                    .voice_states = supp_vs, .voice_state_count = 3};
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) == 3);
  f->i = f->n = 0;

  CT_CHECK(ccord_store_voice_count(store, 12) == 1);
  CT_CHECK(ccord_store_voice_count(store, 21) == 1);
  CT_CHECK(ccord_store_voice_count(store, 22) == 1);
  CT_CHECK(ccord_store_user(store, 3) != nullptr);

  // 2. PASSIVE_UPDATE_V2 removes user 3 from guild 20 and adds user 4 with their user object.
  ccord_user user4 = {.id = 4, .name = "Dave", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_voice_state pv2_vs[2] = {
    {.guild_id = 20, .channel_id = CCORD_ID_NONE, .user_id = 3},
    {.guild_id = 20, .channel_id = 21, .user_id = 4, .self_mute = true},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATES, .guild_id = 20,
                                    .voice_states = pv2_vs, .voice_state_count = 2,
                                    .users = &user4, .user_count = 1};
  CT_CHECK(pump_all(s) == 1);
  f->i = f->n = 0;

  CT_CHECK(ccord_store_voice_count(store, 21) == 1);
  const ccord_voice_state *v4 = ccord_store_voice_at(store, 21, 0);
  CT_CHECK(v4 && v4->user_id == 4 && v4->self_mute);
  const ccord_user *u4 = ccord_store_user(store, 4);
  CT_CHECK(u4 && strcmp(u4->name, "Dave") == 0);

  // 3. GUILD_CREATE (replace_voice_states = true) replaces guild 10 voice states cleanly.
  ccord_voice_state g10_vs[1] = {{.guild_id = 10, .channel_id = 12, .user_id = 4}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_UPSERT, .guild_id = 10, .guild = &guilds[0],
                                    .guild_channels = chans, .guild_channel_count = 2,
                                    .voice_states = g10_vs, .voice_state_count = 1,
                                    .replace_voice_states = true};
  CT_CHECK(pump_all(s) == 1);
  f->i = f->n = 0;

  CT_CHECK(ccord_store_voice_count(store, 12) == 1);
  const ccord_voice_state *v12 = ccord_store_voice_at(store, 12, 0);
  CT_CHECK(v12 && v12->user_id == 4);

  // 4. GUILD_UPDATE without voice_states (replace_voice_states = false) preserves existing voice state.
  ccord_guild g10_upd = {.id = 10, .name = "Alpha Renamed"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_UPSERT, .guild_id = 10, .guild = &g10_upd};
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_voice_count(store, 12) == 1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_prejoin_and_pending_voice_mute_deafen(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 7, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_id guild = ccord_store_guild_at(store, 0)->id;
  ccord_id voice_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, guild, i);
    if (c && c->kind == CCORD_CHANNEL_VOICE) {
      voice_ch = c->id;
      break;
    }
  }
  CT_CHECK(voice_ch != CCORD_ID_NONE);

  // Pre-join mute/deafen toggles cleanly and unmuting clears deafen too.
  CT_CHECK(!ccord_session_voice_muted(s) && !ccord_session_voice_deafened(s));
  CT_CHECK(ccord_session_set_voice_mute(s, true, &err) == CCORD_OK);
  CT_CHECK(ccord_session_voice_muted(s) && !ccord_session_voice_deafened(s));
  CT_CHECK(ccord_session_set_voice_deafen(s, true, &err) == CCORD_OK);
  CT_CHECK(ccord_session_voice_muted(s) && ccord_session_voice_deafened(s));
  CT_CHECK(ccord_session_set_voice_mute(s, false, &err) == CCORD_OK);
  CT_CHECK(!ccord_session_voice_muted(s) && !ccord_session_voice_deafened(s));

  // Set pre-join mute=true before joining, then toggle deafen after join before pumping.
  CT_CHECK(ccord_session_set_voice_mute(s, true, &err) == CCORD_OK);
  CT_CHECK(ccord_session_join_voice(s, guild, voice_ch, &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_voice_deafen(s, true, &err) == CCORD_OK);
  pump_all(s);

  const ccord_voice_state *selfv = ccord_store_voice_user(store, guild, ccord_store_self_id(store));
  CT_CHECK(selfv != nullptr && selfv->channel_id == voice_ch && selfv->self_mute && selfv->self_deaf);
  CT_CHECK(ccord_session_voice_muted(s) && ccord_session_voice_deafened(s));

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_upload_progress(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  g_now = 50000;
  ccord_mock_config cfg = {.seed = 7, .latency_ms = 200, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  g_now += 250;
  pump_all(s);
  g_now += 250;
  pump_all(s);

  ccord_id ch = ccord_store_selected_channel(store);
  CT_CHECK(ch != CCORD_ID_NONE);
  uint8_t d0[4000], d1[6000];
  memset(d0, 1, sizeof d0);
  memset(d1, 2, sizeof d1);
  ccord_upload_file files[2] = {
    {.filename = "photo.png", .data = d0, .size = sizeof d0, .spoiler = false},
    {.filename = "clip.mp4", .data = d1, .size = sizeof d1, .spoiler = true},
  };
  CT_CHECK(ccord_session_send_message_with_files(s, ch, CCORD_ID_NONE, true, "progress", 8, files, 2, &err) == CCORD_OK);

  ccord_upload_progress up = {0};
  CT_CHECK(ccord_session_active_uploads(s, ch + 99999u, &up, 1) == 0);
  CT_CHECK(ccord_session_active_uploads(s, CCORD_ID_NONE, nullptr, 0) == 1);
  CT_CHECK(ccord_session_active_uploads(s, ch, &up, 1) == 1);
  CT_CHECK(up.channel_id == ch && up.file_count == 2 && up.file_index == 0);
  CT_CHECK(up.total_bytes == 10000 && up.uploaded_bytes == 0 && up.phase == CCORD_UPLOAD_SLOT);
  CT_CHECK(strcmp(up.filename, "photo.png") == 0);

  // Mid-upload: transitions to PUT on the active file with proportional byte progress.
  g_now += 100;
  CT_CHECK(ccord_session_active_uploads(s, ch, &up, 1) == 1);
  CT_CHECK(up.phase == CCORD_UPLOAD_PUT && up.uploaded_bytes > 0 && up.uploaded_bytes < 10000);
  CT_CHECK(up.file_index == 1 && strcmp(up.filename, "SPOILER_clip.mp4") == 0);

  // Near completion: transitions to FINALIZE with all bytes uploaded.
  g_now += 95;
  CT_CHECK(ccord_session_active_uploads(s, ch, &up, 1) == 1);
  CT_CHECK(up.phase == CCORD_UPLOAD_FINALIZE && up.uploaded_bytes == 10000);

  // Delivery clears the active upload entry.
  g_now += 20;
  pump_all(s);
  CT_CHECK(ccord_session_active_uploads(s, CCORD_ID_NONE, &up, 1) == 0);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}


int main(void)
{
  test_session_favorite_gif_lossless_proto_roundtrip();
  test_session_messaging_parity_and_interactions();
  test_session_stage_and_scheduled_events();
  test_session_activities_mock();
  test_session_rich_presence();
  test_session_rich_presence_mock();
  test_session_membership_screening();
  test_session_send_voice_message();
  test_session_camera_passthrough();
  test_session_guild_profile_and_notification_filters();
  test_session_supplemental_passive_and_guild_voice_states();
  test_session_prejoin_and_pending_voice_mute_deafen();
  test_session_upload_progress();
  return CT_RESULT("test_session_features");
}
