// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "live_transport_harness.h"

static void test_interaction_subcommands_and_context_menus(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 0);

  // 1. Subcommand (type 1 wrapper around leaf options; command_type=0 defaults to 1).
  const char *sub_names[2] = {"count", "label"};
  uint8_t sub_kinds[2] = {CCORD_SLASH_OPT_INTEGER, CCORD_SLASH_OPT_STRING};
  const char *sub_vals[2] = {"7", "alpha"};
  ccord_interaction_request sub_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9101,
    .command_version = 9102,
    .command_type = 0,
    .command_name = "ticket",
    .subcommand = "open",
    .option_names = sub_names,
    .option_kinds = sub_kinds,
    .option_values = sub_vals,
    .option_count = 2,
    .focused_option = -1,
  };
  CT_CHECK(ccord_live_build_interaction_body(&sub_req, "sess-sub", "3001", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"version\":\"9102\",\"id\":\"9101\",\"name\":\"ticket\",\"type\":1") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b),
                  "\"options\":[{\"type\":1,\"name\":\"open\",\"options\":["
                  "{\"type\":4,\"name\":\"count\",\"value\":7},"
                  "{\"type\":3,\"name\":\"label\",\"value\":\"alpha\"}]}]") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"target_id\"") == nullptr);

  // 2. Subcommand group + subcommand + autocomplete focus inside nested leaf options.
  const char *grp_names[1] = {"role"};
  uint8_t grp_kinds[1] = {CCORD_SLASH_OPT_STRING};
  const char *grp_vals[1] = {"mod"};
  ccord_interaction_request grp_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9103,
    .command_type = 1,
    .command_name = "config",
    .subcommand_group = "permissions",
    .subcommand = "grant",
    .option_names = grp_names,
    .option_kinds = grp_kinds,
    .option_values = grp_vals,
    .option_count = 1,
    .focused_option = 0,
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&grp_req, "sess-grp", "3002", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":4") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b),
                  "\"options\":[{\"type\":2,\"name\":\"permissions\",\"options\":["
                  "{\"type\":1,\"name\":\"grant\",\"options\":["
                  "{\"type\":3,\"name\":\"role\",\"value\":\"mod\",\"focused\":true}]}]}]") != nullptr);

  // 3. Subcommand with zero leaf options emits empty inner options array.
  ccord_interaction_request empty_sub = sub_req;
  empty_sub.subcommand = "list";
  empty_sub.option_count = 0;
  empty_sub.option_names = nullptr;
  empty_sub.option_kinds = nullptr;
  empty_sub.option_values = nullptr;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&empty_sub, "sess-sub", "3003", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"options\":[{\"type\":1,\"name\":\"list\",\"options\":[]}]") != nullptr);

  // 4. User (type 2) and Message (type 3) context-menu commands allow spaces in name and emit target_id.
  ccord_interaction_request user_ctx = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9201,
    .command_type = 2,
    .command_name = "View User Info",
    .target_id = 3333,
    .focused_option = -1,
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&user_ctx, "sess-ctx", "3004", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"name\":\"View User Info\",\"type\":2") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"target_id\":\"3333\"") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"options\":[]") != nullptr);

  ccord_interaction_request msg_ctx = user_ctx;
  msg_ctx.command_id = 9202;
  msg_ctx.command_type = 3;
  msg_ctx.command_name = "Bookmark Message";
  msg_ctx.target_id = 8888;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&msg_ctx, "sess-ctx", "3005", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"name\":\"Bookmark Message\",\"type\":3") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"target_id\":\"8888\"") != nullptr);

  // 5. Validation gates: spaces rejected for CHAT_INPUT, missing target_id or extra options rejected for context menus,
  // and subcommand_group without subcommand rejected.
  ccord_interaction_request bad = sub_req;
  bad.command_name = "has space";
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = user_ctx;
  bad.target_id = CCORD_ID_NONE;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = msg_ctx;
  bad.option_names = grp_names;
  bad.option_kinds = grp_kinds;
  bad.option_values = grp_vals;
  bad.option_count = 1;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = grp_req;
  bad.type = CCORD_INTERACTION_APPLICATION_COMMAND;
  bad.focused_option = -1;
  bad.subcommand = nullptr;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = sub_req;
  bad.target_id = 3333;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  ccord_buf_free(&b);
}


static size_t respond_hook_fx(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)idx;
  const hook_fx_cfg *cfg = ud;
  const char *m = req->method;
  const char *p = req->path;
  bool get = strcmp(m, "GET") == 0;
  bool post = strcmp(m, "POST") == 0;
  bool patch = strcmp(m, "PATCH") == 0;
  bool put = strcmp(m, "PUT") == 0;
  bool del = strcmp(m, "DELETE") == 0;

  if (post && strcmp(p, "/channels/2222/attachments") == 0) {
    char slot[256];
    (void)snprintf(slot, sizeof slot,
                   "{\"attachments\":[{\"id\":0,\"upload_url\":\"http://127.0.0.1:%d/upload/0\","
                   "\"upload_filename\":\"u0/voice-message.ogg\"}]}", cfg->srv->port);
    return fd_reply(out, cap, 200, "OK", nullptr, slot);
  }
  if (put && strcmp(p, "/upload/0") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "");
  }
  if (post && strcmp(p, "/channels/2222/messages") == 0) {
    if (strstr(req->body, "\"duration_secs\"") != nullptr) {
      // Echo what Discord would: the voice flag and the attachment's length + waveform.
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"9110\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                      "\"content\":\"\",\"flags\":8192,\"timestamp\":\"2026-11-10T12:00:09Z\","
                      "\"attachments\":[{\"id\":\"77\",\"filename\":\"voice-message.ogg\","
                      "\"url\":\"https://cdn.example/voice-message.ogg\",\"content_type\":\"audio/ogg\","
                      "\"size\":4,\"duration_secs\":2.5,\"waveform\":\"AAECAwQFBgcICQ==\"}]}");
    }
    if (strstr(req->body, "\"tts\":true") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"9100\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                      "\"content\":\"heads up\",\"tts\":true,\"timestamp\":\"2026-11-10T12:00:00Z\"}");
    }
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9102\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"\",\"timestamp\":\"2026-11-10T12:00:02Z\"}");
  }
  if (post && strcmp(p, "/channels/4444/messages") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9101\",\"channel_id\":\"4444\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"\",\"timestamp\":\"2026-11-10T12:00:01Z\"}");
  }
  if (post && strcmp(p, "/channels/2222/messages/9003/crosspost") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9003\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"announcement\",\"flags\":1,\"timestamp\":\"2026-11-10T12:00:03Z\"}");
  }
  if (post && (strcmp(p, "/channels/2222/messages/9004/ack") == 0 ||
               strcmp(p, "/channels/2222/messages/8989/ack") == 0 ||
               strcmp(p, "/channels/4444/messages/9101/ack") == 0)) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"token\":null}");
  }
  if (post && strcmp(p, "/channels/4444/messages/9100/ack") == 0) {
    return fd_reply(out, cap, 500, "Internal Server Error", nullptr, "{\"message\":\"ack failed\"}");
  }
  if (get && strcmp(p, "/users/@me/settings-proto/2") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, cfg->initial_proto_json);
  }
  if (patch && strcmp(p, "/users/@me/settings-proto/2") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, req->body);
  }
  if (post && strcmp(p, "/interactions") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (post && strcmp(p, "/stage-instances") == 0) {
    return fd_reply(out, cap, 201, "Created", nullptr,
                    "{\"id\":\"6601\",\"guild_id\":\"1111\",\"channel_id\":\"2223\","
                    "\"topic\":\"Town Hall\",\"privacy_level\":2}");
  }
  if (del && strcmp(p, "/stage-instances/2223") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/guilds/1111/scheduled-events?with_user_count=true") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"7701\",\"guild_id\":\"1111\",\"channel_id\":\"2223\",\"creator_id\":\"42\","
                    "\"name\":\"Town Hall Event\",\"description\":\"Q&A\","
                    "\"scheduled_start_time\":\"2026-11-15T18:00:00.000Z\","
                    "\"privacy_level\":2,\"status\":1,\"entity_type\":1,\"user_count\":5}]");
  }
  if (post && strcmp(p, "/guilds/1111/scheduled-events") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"7702\",\"guild_id\":\"1111\",\"channel_id\":\"2223\",\"creator_id\":\"42\","
                    "\"name\":\"Launch Party\",\"description\":\"Bring snacks\","
                    "\"scheduled_start_time\":\"2026-11-16T18:00:00.000Z\","
                    "\"privacy_level\":2,\"status\":1,\"entity_type\":1,\"user_count\":1}");
  }
  if (patch && strcmp(p, "/guilds/1111/scheduled-events/7702") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"7702\",\"guild_id\":\"1111\",\"channel_id\":\"2223\",\"creator_id\":\"42\","
                    "\"name\":\"Launch Party Live\",\"description\":\"Bring snacks\","
                    "\"scheduled_start_time\":\"2026-11-16T18:00:00.000Z\","
                    "\"privacy_level\":2,\"status\":2,\"entity_type\":1,\"user_count\":4}");
  }
  if ((put || del) && strcmp(p, "/guilds/1111/scheduled-events/7702/users/@me") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{}");
  }
  if (del && strcmp(p, "/guilds/1111/scheduled-events/7702") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/guilds/1111/member-verification?with_guild=true") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"version\":\"2026-11-01T00:00:00.000000+00:00\",\"description\":\"Town rules\","
                    "\"form_fields\":[{\"field_type\":\"TERMS\",\"label\":\"Rules\",\"required\":true,"
                    "\"values\":[\"Be kind\",\"No spam\"]}],\"guild\":{\"id\":\"1111\",\"name\":\"Town\"}}");
  }
  if (put && strcmp(p, "/guilds/1111/requests/@me") == 0) {
    // A form labelled "silent" gets the bare 204; anything else gets the member object back with pending cleared.
    if (strstr(req->body, "\"label\":\"silent\"") != nullptr) return fd_reply(out, cap, 204, "No Content", nullptr, "");
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[],\"pending\":false}");
  }
  if (get && strncmp(p, "/channels/2222/messages?limit=", 30) == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"9004\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"after\",\"timestamp\":\"2026-11-10T11:59:00Z\"},"
                    "{\"id\":\"9003\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"announcement\",\"timestamp\":\"2026-11-10T11:58:00Z\"},"
                    "{\"id\":\"8990\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"earliest\",\"timestamp\":\"2026-11-10T11:57:00Z\"}]");
  }
  if (get && strncmp(p, "/channels/", 10) == 0 && strstr(p, "/messages?limit=") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  }
  return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"unhandled route\"}");
}

static bool poll_live_for(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, out) && out->type == want) return true;
    usleep(2000);
  }
  return false;
}

static const fd_hit *find_hit(const fd_hit *hits, size_t n, const char *method, const char *path, size_t *cursor)
{
  size_t start = cursor ? *cursor : 0;
  for (size_t i = start; i < n; i++) {
    if (strcmp(hits[i].method, method) == 0 && strcmp(hits[i].path, path) == 0) {
      if (cursor) *cursor = i + 1;
      return &hits[i];
    }
  }
  return nullptr;
}

static void test_loopback_messaging_proto_stage_and_events(void)
{
  ccord_error err;
  char cache_dir[512];
  if (!make_temp(cache_dir, sizeof cache_dir)) {
    fprintf(stderr, "test_live_transport: no TMPDIR, skipping loopback hook test\n");
    return;
  }

  // Seed initial settings-proto/2 with an unknown wire field (field 99 varint = 0x42 -> 0x98,0x06,0x42, base64 "mAZC")
  // plus one favorite GIF so we can verify raw_proto exposure and lossless round-trip across GET -> PATCH -> cache.
  const uint8_t unknown_seed[] = {0x98u, 0x06u, 0x42u};
  ccord_proto_gif init_gif = {
    .url = "https://tenor.com/view/cat-1",
    .src = "https://media.tenor.com/cat-1.gif",
    .format = 1,
    .width = 320,
    .height = 240,
    .order = 1,
  };
  uint8_t *init_raw = nullptr;
  size_t init_raw_len = 0;
  char *init_json = nullptr;
  CT_CHECK(ccord_user_expressions_encode_gifs(&init_gif, 1, unknown_seed, sizeof unknown_seed, &init_raw, &init_raw_len,
                                              &err) == CCORD_OK);
  const char *seed_json = "{\"settings\":\"mAZC\"}";
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&init_gif, 1, seed_json, strlen(seed_json), &init_json, &err) ==
           CCORD_OK);

  hook_fx_cfg fx_cfg = {0};
  CT_CHECK(init_json != nullptr && strlen(init_json) < sizeof fx_cfg.initial_proto_json);
  if (init_json) snprintf(fx_cfg.initial_proto_json, sizeof fx_cfg.initial_proto_json, "%s", init_json);
  ccord_free(init_json);

  fd_server srv;
  fx_cfg.srv = &srv;
  CT_CHECK(fd_start(&srv, respond_hook_fx, &fx_cfg));

  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = srv.base,
    .disable_gateway = true,
    .request_timeout_ms = 3000,
  };
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);

  ccord_net_event ev;
  CT_CHECK(poll_live_for(t, CCORD_NET_STATE, &ev, 3000) && ev.state == CCORD_CONN_CONNECTED);

  // Register guild-channel mappings (1111 -> 2222, 1111 -> 2223) and a DM channel (4444), and load 2222 history.
  CT_CHECK(ccord_transport_select_channel(t, 1111, 2222, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_select_channel(t, 1111, 2223, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_select_channel(t, CCORD_DM_GUILD_ID, 4444, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_request_history(t, 2222, 50, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_HISTORY, &ev, 3000) && ev.message_count == 3);

  // 1. send_tts_message -> POST /channels/2222/messages with "tts":true -> CCORD_NET_MESSAGE_CREATE.
  CT_CHECK(ccord_transport_send_tts_message(t, 2222, "heads up", 8, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9100 && ev.messages[0].tts &&
           strcmp(ev.messages[0].content, "heads up") == 0);

  // 2. forward_message: guild source includes guild_id; DM source omits guild_id.
  CT_CHECK(ccord_transport_forward_message(t, 4444, 2222, 9001, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9101 && ev.channel_id == 4444);

  CT_CHECK(ccord_transport_forward_message(t, 2222, 4444, 9002, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9102 && ev.channel_id == 2222);

  // 3. crosspost_message -> POST /channels/2222/messages/9003/crosspost -> CCORD_NET_MESSAGE_UPDATE.
  CT_CHECK(ccord_transport_crosspost_message(t, 2222, 9003, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_UPDATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9003 &&
           (ccord_message_flags(&ev.messages[0]) & CCORD_MSGFLAG_CROSSPOSTED) != 0);

  // 4. mark_unread acks the message BEFORE the target:
  //    - channel 2222 has [8990, 9003, 9004, 9100, 9102], so marking 9100 unread acks 9004 (never 9100);
  //    - marking earliest message 8990 unread acks 8990 - 1 == 8989;
  //    - snowflake <= 1 has no preceding message and returns CCORD_ERR_INVALID;
  //    - a failed mark_unread emits ACTION_FAILED and reconciles MESSAGE_ACK back to last_ack_id.
  CT_CHECK(ccord_transport_mark_unread(t, 2222, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_mark_unread(t, 2222, 9100, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_mark_unread(t, 2222, 8990, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_ack_message(t, 4444, 9101, &err) == CCORD_OK);
  for (int i = 0; i < 30; i++) {
    (void)ccord_transport_poll(t, &ev);
    usleep(3000);
  }
  CT_CHECK(ccord_transport_mark_unread(t, 4444, 9101, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_ACTION_FAILED, &ev, 3000));
  CT_CHECK(ev.channel_id == 4444 && ev.message_id == 9101);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_ACK, &ev, 3000));
  CT_CHECK(ev.channel_id == 4444 && ev.message_id == 9101);

  // 5. settings-proto/2 GET -> raw_proto + PATCH -> raw_proto.
  CT_CHECK(ccord_transport_request_expression_data(t, CCORD_EXPR_SETTINGS_PROTO, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_EXPRESSION_DATA, &ev, 3000));
  CT_CHECK(ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.expr_body != nullptr && ev.expr_body_len > 0);
  CT_CHECK(ev.raw_proto != nullptr && ev.raw_proto_len == init_raw_len &&
           memcmp(ev.raw_proto, init_raw, init_raw_len) == 0);
  ccord_user_expressions ux = {0};
  CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &ux, &err) == CCORD_OK && ux.gif_count == 1);
  ccord_user_expressions_free(&ux);

  ccord_proto_gif next_gifs[2] = {
    init_gif,
    {
      .url = "https://tenor.com/view/dog-2",
      .src = "https://media.tenor.com/dog-2.mp4",
      .format = 2,
      .width = 400,
      .height = 300,
      .order = 2,
    },
  };
  uint8_t *patched_raw = nullptr;
  size_t patched_raw_len = 0;
  CT_CHECK(ccord_user_expressions_encode_gifs(next_gifs, 2, ev.raw_proto, ev.raw_proto_len, &patched_raw,
                                              &patched_raw_len, &err) == CCORD_OK);
  CT_CHECK(patched_raw != nullptr && patched_raw_len > sizeof unknown_seed &&
           memcmp(patched_raw, unknown_seed, sizeof unknown_seed) == 0);

  CT_CHECK(ccord_transport_patch_settings_proto(t, patched_raw, patched_raw_len, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_EXPRESSION_DATA, &ev, 3000));
  CT_CHECK(ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.raw_proto != nullptr &&
           ev.raw_proto_len == patched_raw_len && memcmp(ev.raw_proto, patched_raw, patched_raw_len) == 0);
  CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &ux, &err) == CCORD_OK && ux.gif_count == 2);
  ccord_user_expressions_free(&ux);

  // 6. send_interaction for Message context-menu command over loopback.
  ccord_interaction_request ireq = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9202,
    .command_type = 3,
    .command_name = "Pin Fast",
    .target_id = 9003,
    .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);

  // 7. Stage instance create & delete (channel 2223 belongs to guild 1111 via chan_guilds).
  // Select DM channel first so selected_guild is DM, proving STAGE_DELETE resolves guild 1111 from chan_guilds!
  CT_CHECK(ccord_transport_select_channel(t, CCORD_DM_GUILD_ID, 4444, &err) == CCORD_OK);

  ccord_admin_request ar;
  CT_CHECK(ccord_admin_stage_create(&ar, 1111, 2223, "Town Hall", 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 3000));
  CT_CHECK(!ev.stage_deleted && ev.guild_id == 1111 && ev.channel_id == 2223 && ev.stage_instance != nullptr &&
           strcmp(ev.stage_instance->topic, "Town Hall") == 0);

  CT_CHECK(ccord_admin_stage_delete(&ar, 2223, "wrap up", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 3000));
  CT_CHECK(ev.stage_deleted && ev.guild_id == 1111 && ev.channel_id == 2223 && ev.stage_instance != nullptr &&
           ev.stage_instance->channel_id == 2223 && ev.stage_instance->guild_id == 1111);

  // 8. Scheduled events: list, create, modify, RSVP add, RSVP remove, delete.
  CT_CHECK(ccord_admin_event_list(&ar, 1111, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.scheduled_event_count == 1 && ev.scheduled_events[0].id == 7701 &&
           strcmp(ev.scheduled_events[0].name, "Town Hall Event") == 0);

  ccord_scheduled_event new_se = {0};
  new_se.guild_id = 1111;
  new_se.channel_id = 2223;
  snprintf(new_se.name, sizeof new_se.name, "Launch Party");
  snprintf(new_se.description, sizeof new_se.description, "Bring snacks");
  new_se.scheduled_start_ms = 1794852000000LL;
  new_se.entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE;
  CT_CHECK(ccord_admin_event_create(&ar, 1111, &new_se, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev, 3000));
  CT_CHECK(!ev.scheduled_event_deleted && ev.guild_id == 1111 && ev.scheduled_event_id == 7702 &&
           ev.scheduled_event_count == 1 && strcmp(ev.scheduled_events[0].name, "Launch Party") == 0);

  ccord_admin_event_edit edit = CCORD_ADMIN_EVENT_EDIT_INIT;
  edit.name = "Launch Party Live";
  edit.status = CCORD_SCHEDULED_EVENT_ACTIVE;
  CT_CHECK(ccord_admin_event_modify(&ar, 1111, 7702, &edit, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev, 3000));
  CT_CHECK(!ev.scheduled_event_deleted && ev.scheduled_event_id == 7702 &&
           ev.scheduled_events[0].status == CCORD_SCHEDULED_EVENT_ACTIVE &&
           strcmp(ev.scheduled_events[0].name, "Launch Party Live") == 0);

  CT_CHECK(ccord_admin_event_rsvp_add(&ar, 1111, 7702, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.scheduled_event_id == 7702 && ev.scheduled_event_user_added);

  CT_CHECK(ccord_admin_event_rsvp_remove(&ar, 1111, 7702, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.scheduled_event_id == 7702 && !ev.scheduled_event_user_added);

  CT_CHECK(ccord_admin_event_delete(&ar, 1111, 7702, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev, 3000));
  CT_CHECK(ev.scheduled_event_deleted && ev.guild_id == 1111 && ev.scheduled_event_id == 7702 &&
           ev.scheduled_event_count == 1 && ev.scheduled_events[0].id == 7702);

  // 9. Membership screening: the form answer, then the PUT answered with a member object and with a bare 204.
  CT_CHECK(ccord_admin_get_member_verification(&ar, 1111, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MEMBER_VERIFICATION, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.member_verification != nullptr && ev.member_verification->field_count == 1 &&
           strcmp(ev.member_verification->version, "2026-11-01T00:00:00.000000+00:00") == 0 &&
           strcmp(ev.member_verification->description, "Town rules") == 0 &&
           ev.member_verification->fields[0].value_count == 2 &&
           strcmp(ev.member_verification->fields[0].values[1], "No spam") == 0);
  ccord_member_verification ms_form = {
    .version = "2026-11-01T00:00:00.000000+00:00",
    .fields = {{.field_type = CCORD_MEMBER_VERIFICATION_TERMS, .label = "Rules", .required = true,
                .values = {"Be kind", "No spam"}, .value_count = 2}},
    .field_count = 1,
  };
  CT_CHECK(ccord_admin_submit_member_verification(&ar, 1111, &ms_form, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_GUILD_MEMBERS, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.guild_member_count == 1 && ev.guild_members[0].user_id == 42 &&
           !ev.guild_members[0].pending && !ev.guild_members[0].omit_pending);
  ms_form.fields[0].label = "silent";
  CT_CHECK(ccord_admin_submit_member_verification(&ar, 1111, &ms_form, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(!poll_live_for(t, CCORD_NET_GUILD_MEMBERS, &ev, 700));   // a 204 changes nothing locally

  // 10. Voice message: POST /attachments -> PUT slot -> POST /messages, with the metadata riding the final POST.
  static const uint8_t k_ogg[4] = {'O', 'g', 'g', 'S'};
  ccord_upload_file vfile = {.filename = "voice-message.ogg", .data = k_ogg, .size = sizeof k_ogg,
                             .duration_ms = 2500, .waveform_b64 = "AAECAwQFBgcICQ=="};
  CT_CHECK(ccord_transport_send_message_with_files(t, 2222, CCORD_ID_NONE, true, "", 0, &vfile, 1, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9110);
  if (ev.message_count == 1 && ev.messages[0].rich && ev.messages[0].rich->attachment_count == 1) {
    const ccord_message_rich *vr = ev.messages[0].rich;
    CT_CHECK((vr->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0 && vr->attachments[0].duration_ms == 2500 &&
             strcmp(vr->attachments[0].waveform_b64, "AAECAwQFBgcICQ==") == 0);
  } else {
    CT_CHECK(false);
  }
  ccord_upload_file badv = vfile;
  badv.waveform_b64 = "not base64!";
  CT_CHECK(ccord_transport_send_message_with_files(t, 2222, CCORD_ID_NONE, true, "", 0, &badv, 1, &err) == CCORD_ERR_INVALID);

  ccord_transport_destroy(t);
  fd_hit hits[FD_MAX_HITS];
  size_t nhits = fd_stop(&srv, hits, FD_MAX_HITS);

  // Verify wire payloads received by fake_discord.
  size_t cur = 0;
  const fd_hit *h_tts = find_hit(hits, nhits, "POST", "/channels/2222/messages", &cur);
  CT_CHECK(h_tts != nullptr && strstr(h_tts->body, "\"content\":\"heads up\"") != nullptr &&
           strstr(h_tts->body, "\"tts\":true") != nullptr);

  size_t fwd_cur = 0;
  const fd_hit *h_fwd_guild = find_hit(hits, nhits, "POST", "/channels/4444/messages", &fwd_cur);
  CT_CHECK(h_fwd_guild != nullptr &&
           strstr(h_fwd_guild->body,
                  "\"message_reference\":{\"type\":1,\"message_id\":\"9001\",\"channel_id\":\"2222\",\"guild_id\":\"1111\"}") !=
             nullptr);

  const fd_hit *h_fwd_dm = find_hit(hits, nhits, "POST", "/channels/2222/messages", &cur);
  CT_CHECK(h_fwd_dm != nullptr &&
           strstr(h_fwd_dm->body,
                  "\"message_reference\":{\"type\":1,\"message_id\":\"9002\",\"channel_id\":\"4444\"}") != nullptr &&
           strstr(h_fwd_dm->body, "\"guild_id\"") == nullptr);

  CT_CHECK(find_hit(hits, nhits, "POST", "/channels/2222/messages/9003/crosspost", nullptr) != nullptr);

  // The voice upload: one slot request, one PUT of the bytes, and a final POST with the voice metadata.
  const fd_hit *h_slot = find_hit(hits, nhits, "POST", "/channels/2222/attachments", nullptr);
  CT_CHECK(h_slot != nullptr && strstr(h_slot->body, "\"filename\":\"voice-message.ogg\",\"file_size\":4") != nullptr);
  CT_CHECK(find_hit(hits, nhits, "PUT", "/upload/0", nullptr) != nullptr);
  const fd_hit *h_voice = nullptr;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].method, "POST") == 0 && strcmp(hits[i].path, "/channels/2222/messages") == 0 &&
        strstr(hits[i].body, "\"duration_secs\"") != nullptr) {
      h_voice = &hits[i];
    }
  }
  CT_CHECK(h_voice != nullptr && strstr(h_voice->body, "\"uploaded_filename\":\"u0/voice-message.ogg\"") != nullptr &&
           strstr(h_voice->body, "\"duration_secs\":2.5") != nullptr &&
           strstr(h_voice->body, "\"waveform\":\"AAECAwQFBgcICQ==\"") != nullptr &&
           strstr(h_voice->body, "\"flags\":8192") != nullptr);

  // Target 9100 itself must never be acked; instead its predecessor 9004 is acked, and earliest 8990 acks 8989.
  CT_CHECK(find_hit(hits, nhits, "POST", "/channels/2222/messages/9100/ack", nullptr) == nullptr);
  const fd_hit *h_unread_prev = find_hit(hits, nhits, "POST", "/channels/2222/messages/9004/ack", nullptr);
  CT_CHECK(h_unread_prev != nullptr && strcmp(h_unread_prev->body, "{\"manual\":true,\"mention_count\":0}") == 0);
  const fd_hit *h_unread_first = find_hit(hits, nhits, "POST", "/channels/2222/messages/8989/ack", nullptr);
  CT_CHECK(h_unread_first != nullptr && strcmp(h_unread_first->body, "{\"manual\":true,\"mention_count\":0}") == 0);
  const fd_hit *h_unread_fail = find_hit(hits, nhits, "POST", "/channels/4444/messages/9100/ack", nullptr);
  CT_CHECK(h_unread_fail != nullptr && strcmp(h_unread_fail->body, "{\"manual\":true,\"mention_count\":0}") == 0);

  size_t get_proto_count = 0;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].method, "GET") == 0 && strcmp(hits[i].path, "/users/@me/settings-proto/2") == 0) {
      get_proto_count++;
    }
  }
  CT_CHECK(get_proto_count == 1);  // second request was served from cache after PATCH updated disk cache
  const fd_hit *h_patch_proto = find_hit(hits, nhits, "PATCH", "/users/@me/settings-proto/2", nullptr);
  CT_CHECK(h_patch_proto != nullptr && strncmp(h_patch_proto->body, "{\"settings\":\"", 13) == 0);

  // Verify settings-proto/2 disk cache hit also populates raw_proto / raw_proto_len when api_base == CCORD_LIVE_API_BASE.
  if (h_patch_proto) {
    const char * route = "/users/@me/settings-proto/2";
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char *rp = (const unsigned char *)route; *rp; rp++) {
      h ^= (uint64_t)*rp;
      h *= 1099511628211ull;
    }
    char sub1[576], sub2[600], cfile[640];
    snprintf(sub1, sizeof sub1, "%s/ccord", cache_dir);
    snprintf(sub2, sizeof sub2, "%s/ccord/api", cache_dir);
    snprintf(cfile, sizeof cfile, "%s/ccord/api/%016llx.json", cache_dir, (unsigned long long)h);
    (void)mkdir(sub1, 0700);
    (void)mkdir(sub2, 0700);
    FILE *cf = fopen(cfile, "wb");
    CT_CHECK(cf != nullptr);
    if (cf) {
      CT_CHECK(fwrite(h_patch_proto->body, 1, strlen(h_patch_proto->body), cf) == strlen(h_patch_proto->body));
      fclose(cf);
    }
    const char *prev_xdg = getenv("XDG_CACHE_HOME");
    char saved_xdg[512] = "";
    bool had_xdg = prev_xdg != nullptr;
    if (had_xdg) snprintf(saved_xdg, sizeof saved_xdg, "%s", prev_xdg);
    (void)setenv("XDG_CACHE_HOME", cache_dir, 1);

    ccord_live_config ccfg = {
      .token = "REDACTED_TEST_TOKEN",
      .disable_gateway = true,
    };
    ccord_transport *tc = ccord_live_transport_create(&ccfg, &err);
    CT_CHECK(tc != nullptr);
    if (tc) {
      CT_CHECK(ccord_transport_connect(tc, &err) == CCORD_OK);
      CT_CHECK(poll_live_for(tc, CCORD_NET_STATE, &ev, 1000));
      CT_CHECK(ccord_transport_request_expression_data(tc, CCORD_EXPR_SETTINGS_PROTO, nullptr, 0, &err) == CCORD_OK);
      CT_CHECK(poll_live_for(tc, CCORD_NET_EXPRESSION_DATA, &ev, 1000));
      CT_CHECK(ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.raw_proto != nullptr &&
               ev.raw_proto_len == patched_raw_len && memcmp(ev.raw_proto, patched_raw, patched_raw_len) == 0);
      CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &ux, &err) == CCORD_OK &&
               ux.gif_count == 2);
      ccord_user_expressions_free(&ux);
      ccord_transport_destroy(tc);
    }

    if (had_xdg) {
      (void)setenv("XDG_CACHE_HOME", saved_xdg, 1);
    } else {
      (void)unsetenv("XDG_CACHE_HOME");
    }
    (void)unlink(cfile);
    (void)rmdir(sub2);
    (void)rmdir(sub1);
  }

  const fd_hit *h_inter = find_hit(hits, nhits, "POST", "/interactions", nullptr);
  CT_CHECK(h_inter != nullptr && strstr(h_inter->body, "\"name\":\"Pin Fast\",\"type\":3") != nullptr &&
           strstr(h_inter->body, "\"target_id\":\"9003\"") != nullptr);

  CT_CHECK(find_hit(hits, nhits, "POST", "/stage-instances", nullptr) != nullptr);
  const fd_hit *h_stage_del = find_hit(hits, nhits, "DELETE", "/stage-instances/2223", nullptr);
  CT_CHECK(h_stage_del != nullptr && strcmp(h_stage_del->audit, "wrap%20up") == 0);

  CT_CHECK(find_hit(hits, nhits, "GET", "/guilds/1111/scheduled-events?with_user_count=true", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "POST", "/guilds/1111/scheduled-events", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "PATCH", "/guilds/1111/scheduled-events/7702", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "PUT", "/guilds/1111/scheduled-events/7702/users/@me", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "DELETE", "/guilds/1111/scheduled-events/7702/users/@me", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "GET", "/guilds/1111/member-verification?with_guild=true", nullptr) != nullptr);
  size_t ms_cur = 0;
  const fd_hit *h_ms = find_hit(hits, nhits, "PUT", "/guilds/1111/requests/@me", &ms_cur);
  CT_CHECK(h_ms != nullptr && strstr(h_ms->body, "\"version\":\"2026-11-01T00:00:00.000000+00:00\"") != nullptr &&
           strstr(h_ms->body, "\"field_type\":\"TERMS\"") != nullptr && strstr(h_ms->body, "\"response\":true") != nullptr);
  CT_CHECK(find_hit(hits, nhits, "DELETE", "/guilds/1111/scheduled-events/7702", nullptr) != nullptr);

  ccord_free(init_raw);
  ccord_free(patched_raw);
  (void)rmdir(cache_dir);
}

static void ms_cat(ccord_buf *b, const char *s) { (void)ccord_buf_append_str(b, s); }

// Membership screening: the member-verification form (GET answer), the PUT answer path and the `pending` flag on every
// member source (gateway member add/update, GUILD_CREATE, READY merged_members, REST member, message author stub).
static void test_member_screening_json_and_pending(void)
{
  ccord_live_event ev;
  ccord_error err;

  // ---- GET /guilds/{g}/member-verification?with_guild=true ----
  const char *form =
    "{\"version\":\"2022-02-09T21:15:23.570000+00:00\",\"description\":\"Welcome to HQ\","
    "\"form_fields\":[{\"field_type\":\"TERMS\",\"label\":\"Server Rules\",\"required\":true,"
    "\"values\":[\"Be kind\",\"No spam\",7,\"Stay on topic\"],\"automations\":null},"
    "{\"field_type\":\"MULTIPLE_CHOICE\",\"label\":\"skipped\",\"values\":[\"a\"],\"required\":true},"
    "{\"field_type\":\"PARAGRAPH\",\"label\":\"Why join?\",\"required\":false,\"values\":null},"
    "\"junk\",{\"label\":\"No type means terms\"}],"
    "\"guild\":{\"id\":\"77\",\"name\":\"HQ\"}}";
  CT_CHECK(ccord_live_json_member_verification(form, strlen(form), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MEMBER_VERIFICATION && ev.ev.guild_id == 77 && ev.ev.member_verification);
  const ccord_member_verification *mv = ev.ev.member_verification;
  CT_CHECK(strcmp(mv->version, "2022-02-09T21:15:23.570000+00:00") == 0 && strcmp(mv->description, "Welcome to HQ") == 0);
  CT_CHECK(mv->field_count == 3);                                   // multiple choice and the non-object entry are dropped
  CT_CHECK(mv->fields[0].field_type == CCORD_MEMBER_VERIFICATION_TERMS && strcmp(mv->fields[0].label, "Server Rules") == 0 &&
           mv->fields[0].required && mv->fields[0].value_count == 3 && strcmp(mv->fields[0].values[0], "Be kind") == 0 &&
           strcmp(mv->fields[0].values[2], "Stay on topic") == 0);      // the number among the rules is skipped
  CT_CHECK(mv->fields[1].field_type == CCORD_MEMBER_VERIFICATION_TEXT_INPUT && !mv->fields[1].required &&
           mv->fields[1].value_count == 0 && strcmp(mv->fields[1].label, "Why join?") == 0);
  CT_CHECK(mv->fields[2].field_type == CCORD_MEMBER_VERIFICATION_TERMS && mv->fields[2].required &&
           strcmp(mv->fields[2].label, "No type means terms") == 0);
  ccord_live_event_free(&ev);

  // A bare object is a form with nothing in it, never a crash; non-objects and bad args are refused.
  CT_CHECK(ccord_live_json_member_verification("{}", 2, 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_verification->field_count == 0 && strcmp(ev.ev.member_verification->version, "") == 0 &&
           strcmp(ev.ev.member_verification->description, "") == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_member_verification("[]", 2, 77, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_member_verification("{}", 2, 0, &ev, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_json_member_verification("{}", 2, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_json_member_verification(nullptr, 0, 77, &ev, &err) == CCORD_ERR_INVALID);

  // Everything past the model.h caps is cut: 10 fields -> 8, 20 values -> 16, 130-byte text -> 100 bytes (UTF-8 safe).
  ccord_buf big;
  ccord_buf_init(&big, 65536);
  ms_cat(&big, "{\"version\":\"");
  for (int i = 0; i < 40; i++) ms_cat(&big, "v");
  ms_cat(&big, "\",\"description\":\"");
  for (int i = 0; i < 700; i++) ms_cat(&big, "d");
  ms_cat(&big, "\",\"form_fields\":[");
  for (int f = 0; f < 10; f++) {
    ms_cat(&big, f ? ",{\"field_type\":\"TERMS\",\"label\":\"" : "{\"field_type\":\"TERMS\",\"label\":\"");
    for (int i = 0; i < 130; i++) ms_cat(&big, "\xc3\xa9");   // 260 bytes of é
    ms_cat(&big, "\",\"values\":[");
    for (int v = 0; v < 20; v++) {
      ms_cat(&big, v ? ",\"" : "\"");
      for (int i = 0; i < 130; i++) ms_cat(&big, "r");
      ms_cat(&big, "\"");
    }
    ms_cat(&big, "]}");
  }
  ms_cat(&big, "]}");
  CT_CHECK(ccord_live_json_member_verification(ccord_buf_cstr(&big), big.len, 77, &ev, &err) == CCORD_OK);
  mv = ev.ev.member_verification;
  CT_CHECK(strlen(mv->version) == CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES);
  CT_CHECK(strlen(mv->description) == CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES);
  CT_CHECK(mv->field_count == CCORD_MEMBER_VERIFICATION_MAX_FIELDS);
  CT_CHECK(strlen(mv->fields[0].label) == CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES && ccord_utf8_valid(mv->fields[0].label, strlen(mv->fields[0].label)));
  CT_CHECK(mv->fields[0].value_count == CCORD_MEMBER_VERIFICATION_MAX_VALUES &&
           strlen(mv->fields[0].values[15]) == CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES);
  ccord_live_event_free(&ev);
  ccord_buf_free(&big);

  // ---- pending on every member source ----
  const char *add =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_ADD\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"pending\":true}}";
  CT_CHECK(ccord_live_gateway_decode(add, strlen(add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].pending && !ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  const char *accepted =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"pending\":false}}";
  CT_CHECK(ccord_live_gateway_decode(accepted, strlen(accepted), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && !ev.members[0].pending && !ev.members[0].omit_pending);   // an explicit false clears it
  ccord_live_event_free(&ev);
  const char *silent =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"nick\":\"B\"}}";
  CT_CHECK(ccord_live_gateway_decode(silent, strlen(silent), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && !ev.members[0].pending && ev.members[0].omit_pending);   // absent: nothing said
  ccord_live_event_free(&ev);
  const char *junk =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"pending\":\"yes\"}}";
  CT_CHECK(ccord_live_gateway_decode(junk, strlen(junk), &ev, &err) == CCORD_OK);
  CT_CHECK(!ev.members[0].pending && ev.members[0].omit_pending);                            // a non-bool is not a statement
  ccord_live_event_free(&ev);

  const char *create =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"d\":{\"id\":\"77\",\"name\":\"HQ\",\"members\":["
    "{\"user\":{\"id\":\"43\",\"username\":\"Ada\"},\"roles\":[],\"pending\":true},"
    "{\"user\":{\"id\":\"45\",\"username\":\"Cy\"},\"roles\":[]}],\"channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(create, strlen(create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && ev.member_count == 2);
  if (ev.member_count == 2) {
    CT_CHECK(ev.members[0].user_id == 43 && ev.members[0].pending && !ev.members[0].omit_pending);
    CT_CHECK(ev.members[1].user_id == 45 && !ev.members[1].pending && ev.members[1].omit_pending);
  }
  ccord_live_event_free(&ev);

  const char *ready =
    "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"guilds\":[{\"id\":\"100\",\"name\":\"Screened\",\"roles\":[{\"id\":\"100\",\"name\":\"@everyone\"}],"
    "\"channels\":[]}],\"merged_members\":[[{\"user_id\":\"42\",\"roles\":[],\"pending\":true}]]}}";
  CT_CHECK(ccord_live_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT && ev.snapshot.member_count == 1);
  if (ev.snapshot.member_count == 1) {
    CT_CHECK(ev.snapshot.members[0].user_id == 42 && ev.snapshot.members[0].pending && !ev.snapshot.members[0].omit_pending);
  }
  ccord_live_event_free(&ev);

  // REST member object (and the PUT /requests/@me answer, which takes the same path).
  const char *rest = "{\"user\":{\"id\":\"43\",\"username\":\"ada\"},\"roles\":[],\"pending\":false}";
  CT_CHECK(ccord_live_json_member(rest, strlen(rest), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && !ev.members[0].pending && !ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  const char *rest_pending = "{\"user\":{\"id\":\"43\",\"username\":\"ada\"},\"roles\":[],\"pending\":true}";
  CT_CHECK(ccord_live_json_member(rest_pending, strlen(rest_pending), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.members[0].pending && !ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  // A 204 / join-request answer is not a member object: refused so the caller applies nothing.
  const char *join_req = "{\"id\":\"9\",\"application_status\":\"SUBMITTED\",\"guild_id\":\"77\"}";
  CT_CHECK(ccord_live_json_member(join_req, strlen(join_req), 77, &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // MESSAGE_CREATE's partial author member never says anything about screening, even when the member object does.
  const char *msg =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"guild_id\":\"77\","
    "\"author\":{\"id\":\"43\",\"username\":\"ada\"},\"content\":\"hi\",\"member\":{\"nick\":\"Ace\",\"roles\":[],"
    "\"pending\":true}}}";
  CT_CHECK(ccord_live_gateway_decode(msg, strlen(msg), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE && ev.member_count == 1);
  CT_CHECK(ev.members[0].user_id == 43 && !ev.members[0].pending && ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  const char *msg2 =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"101\",\"channel_id\":\"200\",\"guild_id\":\"77\","
    "\"author\":{\"id\":\"43\",\"username\":\"ada\"},\"content\":\"hi\"}}";
  CT_CHECK(ccord_live_gateway_decode(msg2, strlen(msg2), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].omit_pending && !ev.members[0].pending);
  ccord_live_event_free(&ev);
}

// ---- raw media download (the live video path) against a loopback HTTP server ---------------------------------------
//
// live_fetch_media queues a raw GET with bulk=true and max_response=CCORD_LIVE_MAX_MEDIA (live_queue_add_raw); the host
// allow-list only gates live_fetch_media itself, so this drives the same queue request at 127.0.0.1. The server forks a
// handler per connection and answers by path: a fast 20 MiB body, a ~1 MiB/s body, 403, 302, an over-cap body, and a
// header that is held back (to keep raw slots busy).

#define MD_MIB (1024u * 1024u)

static void md_send_all(int fd, const char *p, size_t n)
{
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void md_handle(int cfd)
{
  alarm(60);
  char req[2048];
  size_t len = 0;
  while (len + 1 < sizeof req) {
    ssize_t n = read(cfd, req + len, sizeof req - 1 - len);
    if (n <= 0) return;
    len += (size_t)n;
    req[len] = '\0';
    if (strstr(req, "\r\n\r\n")) break;
  }
  char path[64] = "";
  (void)sscanf(req, "GET %63s", path);
  static char chunk[65536];
  memset(chunk, 0x5a, sizeof chunk);
  char head[256];
  size_t total = 0, step = sizeof chunk, pause_us = 0;
  if (strcmp(path, "/ok20.mp4") == 0) {
    total = 20u * MD_MIB;
  } else if (strcmp(path, "/slow.mp4") == 0) {
    total = 3u * MD_MIB;            // 64 KiB every 62 ms is about 1 MiB/s
    pause_us = 62000;
  } else if (strcmp(path, "/over.mp4") == 0) {
    total = 33u * MD_MIB;           // above CCORD_LIVE_MAX_MEDIA (32 MiB)
  } else if (strcmp(path, "/hold.mp4") == 0) {
    total = 1024;
    sleep(3);
  } else if (strcmp(path, "/small.mp4") == 0) {
    total = 1024;
  } else if (strcmp(path, "/403.mp4") == 0) {
    int n = snprintf(head, sizeof head, "HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno");
    md_send_all(cfd, head, (size_t)n);
    return;
  } else if (strcmp(path, "/302.mp4") == 0) {
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 302 Found\r\nLocation: https://cdn.discordapp.com/elsewhere.mp4\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n");
    md_send_all(cfd, head, (size_t)n);
    return;
  } else {
    int n = snprintf(head, sizeof head, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    md_send_all(cfd, head, (size_t)n);
    return;
  }
  int n = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: %zu\r\n"
                                      "Connection: close\r\n\r\n", total);
  md_send_all(cfd, head, (size_t)n);
  for (size_t sent = 0; sent < total;) {
    size_t c = total - sent < step ? total - sent : step;
    md_send_all(cfd, chunk, c);
    sent += c;
    if (pause_us) usleep((useconds_t)pause_us);
  }
}

static pid_t md_start(int *port)
{
  (void)signal(SIGPIPE, SIG_IGN);
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return -1;
  int opt = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t sl = sizeof sin;
  if (bind(lfd, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(lfd, 32) != 0 ||
      getsockname(lfd, (struct sockaddr *)&sin, &sl) != 0) {
    close(lfd);
    return -1;
  }
  *port = ntohs(sin.sin_port);
  pid_t pid = fork();
  if (pid == 0) {
    (void)signal(SIGCHLD, SIG_IGN);
    alarm(120);
    for (;;) {
      int cfd = accept(lfd, nullptr, nullptr);
      if (cfd < 0) _exit(0);
      if (fork() == 0) {
        close(lfd);
        md_handle(cfd);
        close(cfd);
        _exit(0);
      }
      close(cfd);
    }
  }
  close(lfd);
  return pid;
}

typedef struct md_result {
  int calls;
  ccord_status st;
  long http;
  size_t len;
  uint8_t first, last;
} md_result;

static void md_done(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  md_result *r = ud;
  r->calls++;
  r->st = st;
  r->http = http;
  r->len = len;
  r->first = len ? (uint8_t)body[0] : 0;
  r->last = len ? (uint8_t)body[len - 1] : 0;
}

static void md_get(ccord_rest_queue *q, int port, const char *path, md_result *r)
{
  char url[96];
  (void)snprintf(url, sizeof url, "http://127.0.0.1:%d%s", port, path);
  ccord_rest_request rq = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_VISIBLE);
  rq.full_url = url;
  rq.cb = md_done;
  rq.ud = r;
  rq.bulk = true;                              // as live_queue_add_raw sets it for LIVE_REST_MEDIA
  rq.max_response = CCORD_LIVE_MAX_MEDIA;
  ccord_error err;
  CT_CHECK(ccord_rest_queue_submit(q, &rq, &err) == CCORD_OK);
}

// Pumps until every result has its callback, or `limit_s` real seconds pass. `skew_ms` is added to the clock the
// queue sees (curl keeps real time), so the 45 s pending rule can be crossed without waiting 45 s.
static double md_pump(ccord_rest_queue *q, md_result *const *rs, size_t n, double limit_s, uint64_t skew_ms)
{
  uint64_t t0 = fd_now_ms();
  for (;;) {
    ccord_rest_queue_pump(q, fd_now_ms() + skew_ms);
    bool all = true;
    for (size_t i = 0; i < n; i++) all = all && rs[i]->calls > 0;
    double el = (double)(fd_now_ms() - t0) / 1000.0;
    if (all || el > limit_s) return el;
    usleep(2000);
  }
}

static void test_raw_media_download(void)
{
  int port = 0;
  pid_t srv = md_start(&port);
  CT_CHECK(srv > 0);
  if (srv <= 0) return;
  ccord_rest_config cfg = {.timeout_ms = 15000};
  ccord_error err;
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;

  // A 20 MiB video (under the 32 MiB cap) arrives whole.
  md_result big = {0};
  md_get(q, port, "/ok20.mp4", &big);
  md_result *one[1] = {&big};
  double el = md_pump(q, one, 1, 30.0, 0);
  CT_CHECK(big.calls == 1 && big.st == CCORD_OK && big.http == 200 && big.len == 20u * MD_MIB);
  CT_CHECK(big.first == 0x5a && big.last == 0x5a);
  printf("raw media: 20 MiB fast body took %.1f s\n", el);

  // A body at about 1 MiB/s (3 MiB, 3 s here; 20 MiB would take 20 s, far under the 300 s ceiling and the
  // 2 KiB/s-for-20 s stall rule) is not cut by the stall rule.
  md_result slow = {0};
  md_get(q, port, "/slow.mp4", &slow);
  one[0] = &slow;
  el = md_pump(q, one, 1, 30.0, 0);
  CT_CHECK(slow.calls == 1 && slow.st == CCORD_OK && slow.http == 200 && slow.len == 3u * MD_MIB);
  CT_CHECK(el > 2.0);   // it really was throttled
  printf("raw media: 3 MiB at ~1 MiB/s took %.1f s\n", el);

  // Over the cap: refused as too large, nothing partial delivered.
  md_result over = {0};
  md_get(q, port, "/over.mp4", &over);
  one[0] = &over;
  (void)md_pump(q, one, 1, 30.0, 0);
  // The callback also gets the partial buffer; live_rest_done ignores the body when status != CCORD_OK.
  CT_CHECK(over.calls == 1 && over.st == CCORD_ERR_RANGE);
  CT_CHECK(over.len <= CCORD_LIVE_MAX_MEDIA);

  // 403 and 302 reach the callback as the HTTP status; a redirect is not followed (the Location is never fetched).
  md_result r403 = {0}, r302 = {0};
  md_get(q, port, "/403.mp4", &r403);
  md_get(q, port, "/302.mp4", &r302);
  md_result *two[2] = {&r403, &r302};
  (void)md_pump(q, two, 2, 10.0, 0);
  CT_CHECK(r403.calls == 1 && r403.st == CCORD_OK && r403.http == 403);
  CT_CHECK(r302.calls == 1 && r302.st == CCORD_OK && r302.http == 302 && r302.len == 0);

  // Four held downloads fill the raw slots; a fifth video waits behind them for longer than 45 s of queue time
  // (the queue's clock is skewed by 60 s). It must be started when a slot frees, not dropped as stale.
  md_result hold[4] = {{0}}, fifth = {0};
  for (int i = 0; i < 4; i++) md_get(q, port, "/hold.mp4", &hold[i]);
  md_get(q, port, "/small.mp4", &fifth);
  uint64_t t = fd_now_ms();
  ccord_rest_queue_pump(q, t);
  ccord_rest_queue_pump(q, t + 60000u);
  CT_CHECK(fifth.calls == 0);   // pre-fix: already delivered as CCORD_ERR_TIMEOUT here
  for (int i = 0; i < 8000 && (fifth.calls == 0 || hold[3].calls == 0); i++) {
    ccord_rest_queue_pump(q, fd_now_ms() + 60000u);
    usleep(2000);
  }
  CT_CHECK(fifth.calls == 1 && fifth.st == CCORD_OK && fifth.http == 200 && fifth.len == 1024);
  for (int i = 0; i < 4; i++) CT_CHECK(hold[i].calls == 1 && hold[i].st == CCORD_OK && hold[i].len == 1024);

  ccord_rest_queue_destroy(q);
  (void)kill(srv, SIGTERM);
  (void)waitpid(srv, nullptr, 0);
}

// ---- per-account API cache, warm history file, history stale-while-revalidate ---------------------------------------

// History GET n (0-based, counted inside the forked server) answers with n+1 messages, so a test can tell which
// response (cached file or which network answer) it received by message_count.
static size_t respond_hist_count(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)ud;
  (void)idx;
  static int gets = 0;
  if (strcmp(req->method, "GET") != 0 || strstr(req->path, "/messages?limit=") == nullptr) {
    return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"unhandled route\"}");
  }
  char body[1024] = "[";
  size_t n = (size_t)gets++ + 1u;
  for (size_t i = 0; i < n; i++) {
    char one[200];
    snprintf(one, sizeof one,
             "%s{\"id\":\"%zu\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
             "\"content\":\"m%zu\",\"timestamp\":\"2026-11-10T11:5%zu:00Z\"}",
             i ? "," : "", 9000 - i, i, i);
    strncat(body, one, sizeof body - strlen(body) - 1);
  }
  strncat(body, "]", sizeof body - strlen(body) - 1);
  return fd_reply(out, cap, 200, "OK", nullptr, body);
}

static ccord_transport *hist_transport(const char *api_base, const char *cache_dir)
{
  ccord_error err;
  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = api_base,
    .cache_dir = cache_dir,
    .disable_gateway = true,
    .request_timeout_ms = 3000,
  };
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  ccord_net_event ev;
  if (t && (ccord_transport_connect(t, &err) != CCORD_OK || !poll_live_for(t, CCORD_NET_STATE, &ev, 3000))) {
    ccord_transport_destroy(t);
    return nullptr;
  }
  return t;
}

// Reads the first history event; returns its message count, or -1 when none arrives within the timeout.
static int next_history_count(ccord_transport *t, uint64_t timeout_ms)
{
  ccord_net_event ev;
  if (!poll_live_for(t, CCORD_NET_HISTORY, &ev, timeout_ms)) return -1;
  return (int)ev.message_count;
}

static unsigned mode_bits(const char *p)
{
  struct stat st;
  return stat(p, &st) == 0 ? (unsigned)(st.st_mode & 0777) : 0xFFFFu;
}

static size_t count_files(const char *dir)
{
  size_t n = 0;
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] != '.') n++;
  }
  closedir(d);
  return n;
}

static void test_account_scoped_cache_and_history_revalidate(void)
{
  char root[512];
  if (!make_temp(root, sizeof root)) {
    fprintf(stderr, "test_live_transport: no TMPDIR, skipping account cache test\n");
    return;
  }
  (void)umask(0);  // files must still come out 0600 / 0700
  char dir_a[600], dir_b[600], file[700], api_a[700], api_b[700];
  snprintf(dir_a, sizeof dir_a, "%s/accounts/aaaa", root);
  snprintf(dir_b, sizeof dir_b, "%s/accounts/bbbb", root);
  snprintf(api_a, sizeof api_a, "%s/api", dir_a);
  snprintf(api_b, sizeof api_b, "%s/api", dir_b);

  // ccord_live_history_cache_path
  CT_CHECK(ccord_live_history_cache_path("/d/acc", 2222, file, sizeof file) &&
           strcmp(file, "/d/acc/warm/history/2222.json") == 0);
  CT_CHECK(!ccord_live_history_cache_path(nullptr, 2222, file, sizeof file));
  CT_CHECK(!ccord_live_history_cache_path("", 2222, file, sizeof file));
  CT_CHECK(!ccord_live_history_cache_path("/d/acc", 2222, file, 8));

  fd_server srv;
  CT_CHECK(fd_start(&srv, respond_hist_count, nullptr));
  char hfile_a[700], hfile_b[700];
  CT_CHECK(ccord_live_history_cache_path(dir_a, 2222, hfile_a, sizeof hfile_a));
  CT_CHECK(ccord_live_history_cache_path(dir_b, 2222, hfile_b, sizeof hfile_b));

  // Account A, cold: no cache, so only the network answer (1 message) arrives; it is persisted 0600 in a 0700 dir.
  ccord_error err;
  ccord_transport *ta = hist_transport(srv.base, dir_a);
  CT_CHECK(ta != nullptr);
  if (ta) {
    CT_CHECK(ccord_transport_request_history(ta, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(ta, 3000) == 1);
    CT_CHECK(count_files(api_a) == 1);
    CT_CHECK(mode_bits(api_a) == 0700 && mode_bits(dir_a) == 0700);
    CT_CHECK(mode_bits(hfile_a) == 0600);
    char pdir[700];
    snprintf(pdir, sizeof pdir, "%s/warm/history", dir_a);
    CT_CHECK(mode_bits(pdir) == 0700);
    DIR *d = opendir(api_a);
    struct dirent *e;
    while (d && (e = readdir(d)) != nullptr) {
      if (e->d_name[0] == '.') continue;
      char f[1100];
      snprintf(f, sizeof f, "%s/%s", api_a, e->d_name);
      CT_CHECK(mode_bits(f) == 0600);
    }
    if (d) closedir(d);
    ccord_transport_destroy(ta);
  }

  // Account B asks for the same REST path: A's cached page must not be delivered. The first (and only) event is the
  // network answer (2 messages); B gets its own cache file and A's file is untouched.
  ccord_transport *tb = hist_transport(srv.base, dir_b);
  CT_CHECK(tb != nullptr);
  if (tb) {
    CT_CHECK(ccord_transport_request_history(tb, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(tb, 3000) == 2);
    CT_CHECK(count_files(api_b) == 1);
    ccord_net_event extra;
    CT_CHECK(!poll_live_for(tb, CCORD_NET_HISTORY, &extra, 150));  // nothing else was served from disk
    ccord_transport_destroy(tb);
  }
  char one_a[1024] = "", one_b[1024] = "";
  for (int k = 0; k < 2; k++) {
    const char *dir = k ? api_b : api_a;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != nullptr) {
      if (e->d_name[0] != '.') snprintf(k ? one_b : one_a, 1024, "%s/%s", dir, e->d_name);
    }
    if (d) closedir(d);
  }
  CT_CHECK(one_a[0] && one_b[0] && strcmp(one_a, one_b) != 0);
  CT_CHECK(strcmp(strrchr(one_a, '/'), strrchr(one_b, '/')) == 0);  // same key, different account directory
  ccord_error rerr;
  size_t la = 0, lb = 0;
  char *ca = ccord_file_read_max(one_a, 1 << 20, &la, &rerr);
  char *cb = ccord_file_read_max(one_b, 1 << 20, &lb, &rerr);
  CT_CHECK(ca && cb && la > 0 && lb > la);  // A holds the 1-message page, B the 2-message page
  ccord_free(ca);
  ccord_free(cb);

  // Account A again (a warm start): the fresh cached page (1 message) is delivered first, and the network is still
  // asked: the second event is the live answer (3 messages). No TTL read-through.
  ta = hist_transport(srv.base, dir_a);
  CT_CHECK(ta != nullptr);
  if (ta) {
    CT_CHECK(ccord_transport_request_history(ta, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(ta, 3000) == 1);
    CT_CHECK(next_history_count(ta, 3000) == 3);
    ccord_transport_destroy(ta);
  }
  fd_hit hits[FD_MAX_HITS];
  size_t nhits = fd_stop(&srv, hits, FD_MAX_HITS);
  size_t gets = 0;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].method, "GET") == 0 && strstr(hits[i].path, "/messages?limit=") != nullptr) gets++;
  }
  CT_CHECK(gets == 3);  // A cold, B cold, A warm: the cache hit still went to the network

  // Warm history files are pruned to the 8 newest.
  char hdir[700], extra_file[800];
  snprintf(hdir, sizeof hdir, "%s/warm/history", dir_a);
  for (int i = 0; i < 12; i++) {
    snprintf(extra_file, sizeof extra_file, "%s/%d.json", hdir, 100 + i);
    FILE *f = fopen(extra_file, "wb");
    if (f) {
      fputs("[]", f);
      fclose(f);
    }
  }
  fd_server srv2;
  CT_CHECK(fd_start(&srv2, respond_hist_count, nullptr));
  ta = hist_transport(srv2.base, dir_a);
  CT_CHECK(ta != nullptr);
  if (ta) {
    CT_CHECK(ccord_transport_request_history(ta, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(ta, 3000) == 3);   // stale page saved by the earlier run
    CT_CHECK(next_history_count(ta, 3000) == 1);   // live answer from the fresh server
    ccord_transport_destroy(ta);
  }
  (void)fd_stop(&srv2, hits, FD_MAX_HITS);
  CT_CHECK(count_files(hdir) == 8);
  CT_CHECK(mode_bits(hfile_a) == 0600);  // the newest (just written) file survived the prune

  ccord_error rm;
  CT_CHECK(ccord_rmtree(root, &rm) == CCORD_OK);
}

static void test_network_event_hook(void)
{
  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);
  ccord_error err;

  // A transport whose vtable has no hook: UNSUPPORTED, no crash; a null transport is the same.
  static const ccord_transport_vtable bare_vt = {.name = "bare"};
  ccord_transport bare = {.vt = &bare_vt};
  CT_CHECK(ccord_transport_network_event(&bare, CCORD_LINK_RESUMED, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_transport_network_event(nullptr, CCORD_LINK_CHANGED, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_transport_network_event(&bare, CCORD_LINK_CHANGED, nullptr) == CCORD_ERR_UNSUPPORTED);

  // Live, no gateway (connected in the offline mode): the event reaches live_network_event and is a no-op.
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN", .disable_gateway = true};
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(t->vt->network_event != nullptr);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_NONE, &err) == CCORD_OK);
  ccord_transport_destroy(t);

  // Live with a gateway that was never started (state DISCONNECTED: wake verdict IGNORED): OK, nothing happens.
  ccord_live_config gcfg = {.token = "REDACTED_TEST_TOKEN", .gateway_url = "ws://127.0.0.1:1/"};
  t = ccord_live_transport_create(&gcfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  // Started (CONNECTING toward a closed port): the event is still delivered and returns OK. The verdict itself is
  // pinned by the gateway task's tests once ccord_gateway_wake exists; after disconnect it is a no-op again.
  if (ccord_transport_connect(t, &err) == CCORD_OK) {
    CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
    ccord_transport_disconnect(t);
    CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  }
  ccord_transport_destroy(t);
}

// ---- status through settings-proto/1 ----
// Base proto the fake server holds: field 3 "abc", status online + show_current_game, field 99 varint.
#define SP1_BASE_B64 "GgNhYmNaDgoICgZvbmxpbmUaAggBmAZC"

typedef struct sp1_cfg {
  bool fail_get;
} sp1_cfg;

static size_t respond_sp1(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)idx;
  const sp1_cfg *cfg = ud;
  if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/users/@me/settings-proto/1") == 0) {
    if (cfg->fail_get) return fd_reply(out, cap, 500, "Internal Server Error", nullptr, "{\"message\":\"boom\"}");
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"settings\":\"" SP1_BASE_B64 "\"}");
  }
  if (strcmp(req->method, "PATCH") == 0 && strcmp(req->path, "/users/@me/settings-proto/1") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, req->body);
  }
  return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"unknown\"}");
}

static void sp1_pump(ccord_transport *t, int ms)
{
  ccord_net_event ev;
  for (int i = 0; i < ms / 3; i++) {
    (void)ccord_transport_poll(t, &ev);
    usleep(3000);
  }
}

static void test_status_proto_write_and_updates(void)
{
  ccord_error err;
  // Gateway side: a type-1 update carries its bytes (and "partial"); one with folders keeps both.
  ccord_live_event gev;
  const char *upd = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"partial\":true,\"settings\":{\"type\":1,"
                    "\"proto\":\"WggKBgoEaWRsZQ==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(upd, strlen(upd), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_USER_SETTINGS_PROTO && gev.ev.settings_proto1_len == 10 &&
           gev.ev.settings_proto1_partial && gev.ev.settings_proto1 && gev.ev.settings_proto1[0] == 0x5a);
  ccord_proto_status dec;
  CT_CHECK(ccord_user_status_decode(gev.ev.settings_proto1, gev.ev.settings_proto1_len, &dec, &err) == CCORD_OK &&
           dec.present && strcmp(dec.status, "idle") == 0);
  ccord_live_event_free(&gev);
  const char *full = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"settings\":{\"type\":1,"
                     "\"proto\":\"choKFAgeCAoSAgg3GgUKA09wcyIDCP8BCgIIFA==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(full, strlen(full), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_GUILD_LAYOUT && gev.ev.settings_proto1_len > 0 && !gev.ev.settings_proto1_partial);
  ccord_live_event_free(&gev);
  const char *t2 = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"settings\":{\"type\":2,\"proto\":\"WggKBgoEaWRsZQ==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(t2, strlen(t2), &gev, &err) == CCORD_OK && gev.ev.settings_proto1_len == 0);
  // ...and rides as raw_proto for the favourites path (type 2 is FrecencyUserSettings), with its partial flag.
  CT_CHECK(gev.ev.type == CCORD_NET_USER_SETTINGS_PROTO && gev.ev.raw_proto_len == 10 && !gev.ev.settings_proto1_partial);
  ccord_live_event_free(&gev);
  const char *t2p = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"partial\":true,\"settings\":{\"type\":2,"
                    "\"proto\":\"WggKBgoEaWRsZQ==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(t2p, strlen(t2p), &gev, &err) == CCORD_OK && gev.ev.raw_proto &&
           gev.ev.settings_proto1_partial);
  ccord_live_event_free(&gev);

  // SESSIONS_REPLACE: the "all" session is the overall status; a lone session stands for it; "unknown" says nothing.
  const char *sr1 = "{\"op\":0,\"t\":\"SESSIONS_REPLACE\",\"d\":[{\"session_id\":\"abc\",\"status\":\"online\"},"
                    "{\"session_id\":\"all\",\"status\":\"dnd\",\"activities\":[]}]}";
  CT_CHECK(ccord_live_gateway_decode(sr1, strlen(sr1), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_SESSIONS_REPLACE && gev.ev.presence == CCORD_PRESENCE_DND);
  ccord_live_event_free(&gev);
  const char *sr2 = "{\"op\":0,\"t\":\"SESSIONS_REPLACE\",\"d\":[{\"session_id\":\"abc\",\"status\":\"idle\"}]}";
  CT_CHECK(ccord_live_gateway_decode(sr2, strlen(sr2), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_SESSIONS_REPLACE && gev.ev.presence == CCORD_PRESENCE_IDLE);
  ccord_live_event_free(&gev);
  const char *sr3 = "{\"op\":0,\"t\":\"SESSIONS_REPLACE\",\"d\":[{\"session_id\":\"all\",\"status\":\"unknown\"}]}";
  CT_CHECK(ccord_live_gateway_decode(sr3, strlen(sr3), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type != CCORD_NET_SESSIONS_REPLACE);
  ccord_live_event_free(&gev);

  // REST side: no base known -> GET settings-proto/1 first, then PATCH built on it; the next write reuses the answer.
  sp1_cfg cfgs = {0};
  fd_server srv;
  CT_CHECK(fd_start(&srv, respond_sp1, &cfgs));
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN", .api_base = srv.base, .disable_gateway = true,
                           .request_timeout_ms = 3000};
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_live_for(t, CCORD_NET_STATE, &ev, 3000) && ev.state == CCORD_CONN_CONNECTED);

  ccord_proto_status want = {0};
  want.has_status = true;
  memcpy(want.status, "dnd", 4);
  CT_CHECK(ccord_transport_set_status_proto(t, &want, &err) == CCORD_OK);
  sp1_pump(t, 600);
  ccord_proto_status bad = want;
  memcpy(bad.status, "busy", 5);
  CT_CHECK(ccord_transport_set_status_proto(t, &bad, &err) == CCORD_ERR_INVALID);   // refused before any request
  ccord_proto_status want2 = {0};
  want2.has_status = true;
  memcpy(want2.status, "invisible", 10);
  want2.has_custom = true;
  memcpy(want2.custom_text, "brb", 4);
  CT_CHECK(ccord_transport_set_status_proto(t, &want2, &err) == CCORD_OK);
  sp1_pump(t, 600);
  ccord_transport_destroy(t);

  fd_hit hits[FD_MAX_HITS];
  size_t nhits = fd_stop(&srv, hits, FD_MAX_HITS);
  size_t gets = 0, patches = 0;
  const fd_hit *p1 = nullptr, *p2 = nullptr;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].path, "/users/@me/settings-proto/1") != 0) continue;
    if (strcmp(hits[i].method, "GET") == 0) gets++;
    if (strcmp(hits[i].method, "PATCH") == 0) {
      patches++;
      if (!p1) p1 = &hits[i];
      else if (!p2) p2 = &hits[i];
    }
  }
  CT_CHECK(gets == 1 && patches == 2);   // the first write fetched the base; the second built on the first's bytes
  char *exp1 = nullptr, *exp2 = nullptr;
  CT_CHECK(ccord_user_status_patch_b64(SP1_BASE_B64, strlen(SP1_BASE_B64), &want, &exp1, &err) == CCORD_OK);
  CT_CHECK(exp1 && ccord_user_status_patch_b64(exp1, strlen(exp1), &want2, &exp2, &err) == CCORD_OK);
  char want_body1[1024], want_body2[1024];
  snprintf(want_body1, sizeof want_body1, "{\"settings\":\"%s\"}", exp1 ? exp1 : "");
  snprintf(want_body2, sizeof want_body2, "{\"settings\":\"%s\"}", exp2 ? exp2 : "");
  CT_CHECK(p1 && strcmp(p1->body, want_body1) == 0);
  CT_CHECK(p2 && strcmp(p2->body, want_body2) == 0);
  if (exp1) {
    ccord_proto_status chk;
    CT_CHECK(ccord_user_status_decode_b64(exp1, strlen(exp1), &chk, &err) == CCORD_OK && strcmp(chk.status, "dnd") == 0);
  }
  ccord_free(exp1);
  ccord_free(exp2);

  // A failed GET leaves no base: nothing is PATCHed (a guess would overwrite the account's settings).
  sp1_cfg bad_cfg = {.fail_get = true};
  fd_server srv2;
  CT_CHECK(fd_start(&srv2, respond_sp1, &bad_cfg));
  ccord_live_config cfg2 = {.token = "REDACTED_TEST_TOKEN", .api_base = srv2.base, .disable_gateway = true,
                            .request_timeout_ms = 3000};
  ccord_transport *t2c = ccord_live_transport_create(&cfg2, &err);
  CT_CHECK(t2c != nullptr && ccord_transport_connect(t2c, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t2c, CCORD_NET_STATE, &ev, 3000));
  CT_CHECK(ccord_transport_set_status_proto(t2c, &want, &err) == CCORD_OK);
  sp1_pump(t2c, 600);
  ccord_transport_destroy(t2c);
  size_t nh2 = fd_stop(&srv2, hits, FD_MAX_HITS);
  size_t patches2 = 0;
  for (size_t i = 0; i < nh2; i++) {
    if (strcmp(hits[i].method, "PATCH") == 0) patches2++;
  }
  CT_CHECK(nh2 >= 1 && patches2 == 0);
}

static void test_guild_load_role_gated_and_self_roles(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY without inline merged_members (e.g., PRIORITIZED_READY_PAYLOAD where merged_members is deferred to
  //    READY_SUPPLEMENTAL) must preserve role-gated channels and parent categories whose children are visible.
  const char *ready_no_roles =
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"guilds\":[{\"id\":\"900\",\"properties\":{\"id\":\"900\",\"name\":\"Community\",\"owner_id\":\"5\"},"
    "\"roles\":[{\"id\":\"900\",\"name\":\"@everyone\",\"permissions\":\"0\"},"
    "{\"id\":\"901\",\"name\":\"Member\",\"permissions\":\"1024\"}],"
    "\"channels\":["
    "{\"id\":\"910\",\"type\":4,\"name\":\"Main\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
    "{\"id\":\"911\",\"type\":0,\"parent_id\":\"910\",\"name\":\"general\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"},"
    "{\"id\":\"901\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]}"
    "]}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_no_roles, strlen(ready_no_roles), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.guild_count == 1);
  CT_CHECK(strcmp(ev.snapshot.guilds[0].name, "Community") == 0);
  // Both parent category 910 and child channel 911 must remain present when self_roles wasn't delivered yet.
  CT_CHECK(ev.snapshot.channel_count == 2);
  ccord_live_event_free(&ev);

  // 2. When self_roles ARE known and deny VIEW_CHANNEL on a parent category, the category stays if a child is visible.
  const char *ready_cat_child =
    "{\"op\":0,\"t\":\"READY\",\"s\":2,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"merged_members\":[[{\"user_id\":\"42\",\"roles\":[\"901\"]}]],"
    "\"guilds\":[{\"id\":\"900\",\"properties\":{\"id\":\"900\",\"name\":\"Community\",\"owner_id\":\"5\"},"
    "\"roles\":[{\"id\":\"900\",\"name\":\"@everyone\",\"permissions\":\"0\"},"
    "{\"id\":\"901\",\"name\":\"Member\",\"permissions\":\"0\"}],"
    "\"channels\":["
    "{\"id\":\"910\",\"type\":4,\"name\":\"Hidden Cat\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
    "{\"id\":\"911\",\"type\":0,\"parent_id\":\"910\",\"name\":\"open-child\","
    "\"permission_overwrites\":[{\"id\":\"901\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]},"
    "{\"id\":\"912\",\"type\":0,\"parent_id\":\"910\",\"name\":\"secret-mod\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]}"
    "]}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_cat_child, strlen(ready_cat_child), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.guild_count == 1 && ev.snapshot.channel_count == 2);
  CT_CHECK(ev.snapshot.channels[0].id == 910 && ev.snapshot.channels[1].id == 911);
  ccord_live_event_free(&ev);

  // 3. ccord_live_gateway_decode_ex collects self_roles on GUILD_CREATE when self_id is supplied.
  const char *guild_create =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"s\":3,\"d\":{\"id\":\"900\",\"name\":\"Community\",\"owner_id\":\"5\","
    "\"roles\":[{\"id\":\"900\",\"name\":\"@everyone\",\"permissions\":\"0\"},"
    "{\"id\":\"901\",\"name\":\"Member\",\"permissions\":\"0\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[\"901\"]}],"
    "\"channels\":[{\"id\":\"911\",\"type\":0,\"name\":\"general\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"},"
    "{\"id\":\"901\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]},"
    "{\"id\":\"912\",\"type\":0,\"name\":\"secret\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]}]}}";
  CT_CHECK(ccord_live_gateway_decode_ex(guild_create, strlen(guild_create), 42, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT);
  CT_CHECK(ev.snap_channel_count == 1 && ev.snap_channels[0].id == 911);
  ccord_live_event_free(&ev);
}

static void test_guild_profile_and_primary_guild_clan_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. Guild profile wrapped under "profile", integer brand_color_primary, and fallback missing name -> unavailable.
  const char *wrapped = "{\"profile\":{\"id\":\"900\",\"name\":\"Tag Server\",\"tag\":\"TAG\","
                        "\"badge\":\"bhash\",\"brand_color_primary\":16711680,"
                        "\"online_count\":12,\"member_count\":99}}";
  CT_CHECK(ccord_live_json_guild_profile(wrapped, strlen(wrapped), 900, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_PROFILE && ev.ev.guild_profile.guild_id == 900);
  CT_CHECK(strcmp(ev.ev.guild_profile.name, "Tag Server") == 0);
  CT_CHECK(strcmp(ev.ev.guild_profile.tag, "TAG") == 0);
  CT_CHECK(ev.ev.guild_profile.has_brand_color && ev.ev.guild_profile.brand_color == 0xFF0000u);
  CT_CHECK(!ev.ev.guild_profile.unavailable && ev.ev.guild_profile.badge_url[0] != '\0');
  ccord_live_event_free(&ev);

  const char *empty_name = "{\"id\":\"901\",\"tag\":\"TAG\"}";
  CT_CHECK(ccord_live_json_guild_profile(empty_name, strlen(empty_name), 901, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.guild_profile.unavailable);
  ccord_live_event_free(&ev);

  // 2. Partial "clan" without identity_guild_id merges missing identity_guild_id from "primary_guild".
  const char *ready_clan =
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\","
    "\"clan\":{\"tag\":\"TAG\",\"identity_enabled\":true},"
    "\"primary_guild\":{\"identity_guild_id\":\"900\",\"identity_enabled\":true,\"tag\":\"TAG\",\"badge\":\"bhash\"}},"
    "\"guilds\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_clan, strlen(ready_clan), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.user_count >= 1);
  CT_CHECK(strcmp(ev.snapshot.users[0].clan_tag, "TAG") == 0 && ev.snapshot.users[0].clan_guild_id == 900);
  ccord_live_event_free(&ev);
}

static void test_ready_supplemental_and_passive_voice_states_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY_SUPPLEMENTAL carries guild voice_states (omitted from prioritized READY).
  const char *rsupp =
    "{\"op\":0,\"t\":\"READY_SUPPLEMENTAL\",\"s\":2,\"d\":{"
    "\"guilds\":["
    "{\"id\":\"900\",\"voice_states\":["
    "{\"user_id\":\"101\",\"channel_id\":\"911\",\"self_mute\":true,\"self_stream\":true,\"suppress\":false},"
    "{\"user_id\":\"102\",\"channel_id\":null}"
    "]},"
    "{\"id\":\"901\",\"voice_states\":["
    "{\"user_id\":\"103\",\"channel_id\":\"921\",\"self_video\":true,"
    "\"member\":{\"user\":{\"id\":\"103\",\"username\":\"carol\",\"global_name\":\"Carol\"}}}"
    "]}"
    "],"
    "\"merged_members\":[[{\"user\":{\"id\":\"101\",\"username\":\"alice\",\"global_name\":\"Alice\"}}]]}}";
  CT_CHECK(ccord_live_gateway_decode(rsupp, strlen(rsupp), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATES);
  CT_CHECK(ev.ev.guild_id == CCORD_ID_NONE && !ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 2);
  CT_CHECK(ev.ev.voice_states[0].guild_id == 900 && ev.ev.voice_states[0].channel_id == 911);
  CT_CHECK(ev.ev.voice_states[0].user_id == 101 && ev.ev.voice_states[0].self_mute && ev.ev.voice_states[0].self_stream);
  CT_CHECK(ev.ev.voice_states[1].guild_id == 901 && ev.ev.voice_states[1].channel_id == 921);
  CT_CHECK(ev.ev.voice_states[1].user_id == 103 && ev.ev.voice_states[1].self_video);
  CT_CHECK(ev.ev.user_count == 2);
  ccord_live_event_free(&ev);

  // Empty READY_SUPPLEMENTAL is valid and emits no event.
  const char *rsupp_empty = "{\"op\":0,\"t\":\"READY_SUPPLEMENTAL\",\"s\":3,\"d\":{\"guilds\":[{\"id\":\"900\",\"voice_states\":[]}]}}";
  CT_CHECK(ccord_live_gateway_decode(rsupp_empty, strlen(rsupp_empty), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_NONE);
  ccord_live_event_free(&ev);

  // 2. PASSIVE_UPDATE_V2 carries updated_voice_states, removed_voice_states, and updated_members.
  const char *pv2 =
    "{\"op\":0,\"t\":\"PASSIVE_UPDATE_V2\",\"s\":4,\"d\":{"
    "\"guild_id\":\"900\","
    "\"updated_members\":[{\"user\":{\"id\":\"104\",\"username\":\"dave\",\"global_name\":\"Dave\"}}],"
    "\"updated_voice_states\":["
    "{\"user_id\":\"104\",\"channel_id\":\"911\",\"self_deaf\":true},"
    "{\"user_id\":\"101\",\"channel_id\":null}"
    "],"
    "\"removed_voice_states\":[\"105\"]}}";
  CT_CHECK(ccord_live_gateway_decode(pv2, strlen(pv2), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATES);
  CT_CHECK(ev.ev.guild_id == 900 && !ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 3);
  CT_CHECK(ev.ev.voice_states[0].user_id == 104 && ev.ev.voice_states[0].channel_id == 911 && ev.ev.voice_states[0].self_deaf);
  CT_CHECK(ev.ev.voice_states[1].user_id == 101 && ev.ev.voice_states[1].channel_id == CCORD_ID_NONE);
  CT_CHECK(ev.ev.voice_states[2].user_id == 105 && ev.ev.voice_states[2].channel_id == CCORD_ID_NONE);
  CT_CHECK(ev.ev.user_count == 1 && ev.ev.users[0].id == 104 && strcmp(ev.ev.users[0].name, "Dave") == 0);
  ccord_live_event_free(&ev);

  // 3. PASSIVE_UPDATE_V1 carries authoritative guild voice_states.
  const char *pv1 =
    "{\"op\":0,\"t\":\"PASSIVE_UPDATE_V1\",\"s\":5,\"d\":{\"guild_id\":\"900\",\"voice_states\":["
    "{\"user_id\":\"104\",\"channel_id\":\"911\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(pv1, strlen(pv1), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATES && ev.ev.guild_id == 900 && ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 1 && ev.ev.voice_states[0].user_id == 104);
  ccord_live_event_free(&ev);

  // 4. GUILD_CREATE surfaces voice_states with replace_voice_states=true; GUILD_UPDATE without voice_states keeps false.
  const char *gcreate =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"s\":6,\"d\":{\"id\":\"900\",\"name\":\"G\","
    "\"channels\":[{\"id\":\"911\",\"type\":2,\"name\":\"Voice\"}],"
    "\"voice_states\":[{\"user_id\":\"104\",\"channel_id\":\"911\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(gcreate, strlen(gcreate), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 1 && ev.ev.voice_states[0].user_id == 104 && ev.ev.voice_states[0].channel_id == 911);
  ccord_live_event_free(&ev);

  const char *gupdate = "{\"op\":0,\"t\":\"GUILD_UPDATE\",\"s\":7,\"d\":{\"id\":\"900\",\"name\":\"G2\"}}";
  CT_CHECK(ccord_live_gateway_decode(gupdate, strlen(gupdate), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && !ev.ev.replace_voice_states && ev.ev.voice_state_count == 0);
  ccord_live_event_free(&ev);
}


int main(void)
{
  test_interaction_subcommands_and_context_menus();
  test_member_screening_json_and_pending();
  test_loopback_messaging_proto_stage_and_events();
  test_raw_media_download();
  test_account_scoped_cache_and_history_revalidate();
  test_network_event_hook();
  test_status_proto_write_and_updates();
  test_guild_load_role_gated_and_self_roles();
  test_guild_profile_and_primary_guild_clan_json();
  test_ready_supplemental_and_passive_voice_states_json();
  return CT_RESULT("test_live_features");
}
