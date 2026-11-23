// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_test_harness.h"

static void test_flow(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 7, .big_channel_messages = 300, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_DISCONNECTED);
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTED);
  ccord_id ch = ccord_store_selected_channel(store);
  CT_CHECK(ch != CCORD_ID_NONE);
  // History of the selected channel was requested by the snapshot and arrives in the same pump loop (50-msg chunk).
  CT_CHECK(ccord_store_history_loaded(store, ch));
  size_t before = ccord_store_message_count(store, ch);
  CT_CHECK(before == CCORD_SESSION_HISTORY_LIMIT);
  CT_CHECK(ccord_session_history_has_more(s, ch));

  CT_CHECK(ccord_session_send_message(s, ch, "hello", 5, &err) == CCORD_OK);
  // M0-3: a visibly-pending local echo appears at once (optimistic local echo exception).
  CT_CHECK(ccord_store_message_count(store, ch) == before + 1);
  const ccord_message *pend = ccord_store_message_at(store, ch, before);
  CT_CHECK(pend && pend->send_state == CCORD_MSG_SEND_PENDING && strcmp(pend->content, "hello") == 0);
  CT_CHECK(pend && (pend->id & (1ULL << 63)) != 0 && pend->nonce[0] != '\0');
  char hello_nonce[32] = "";
  if (pend) memcpy(hello_nonce, pend->nonce, sizeof hello_nonce);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 1);   // the nonce-matched echo replaced it, not duplicated
  const ccord_message *last = ccord_store_message_at(store, ch, before);
  CT_CHECK(last && strcmp(last->content, "hello") == 0 && last->author_id == ccord_store_self_id(store));
  CT_CHECK(last && last->send_state == CCORD_MSG_SEND_CONFIRMED && (last->id & (1ULL << 63)) == 0);
  CT_CHECK(last && strcmp(last->nonce, hello_nonce) == 0);
  ccord_id mid = last ? last->id : CCORD_ID_NONE;

  CT_CHECK(ccord_session_send_typing(s, ch, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_typing_count(store, ch) == 1);
  CT_CHECK(ccord_store_typing_user_at(store, ch, 0) == ccord_store_self_id(store));

  CT_CHECK(ccord_session_edit_message(s, ch, mid, "hello edited", 12, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  last = ccord_store_find_message(store, ch, mid);
  CT_CHECK(last && last->edited && strcmp(last->content, "hello edited") == 0);

  CT_CHECK(ccord_session_toggle_reaction(s, ch, mid, "\xf0\x9f\x91\x8d", 4, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  last = ccord_store_find_message(store, ch, mid);
  CT_CHECK(last && last->reaction_count == 1 && last->self_reacted);
  CT_CHECK(last && last->reaction_kind_count == 1);
  CT_CHECK(last && strcmp(last->reactions[0].emoji, "\xf0\x9f\x91\x8d") == 0);
  CT_CHECK(ccord_session_toggle_reaction(s, ch, mid, "\xf0\x9f\x91\x8d", 4, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  last = ccord_store_find_message(store, ch, mid);
  CT_CHECK(last && last->reaction_count == 0 && !last->self_reacted);
  CT_CHECK(last && last->reaction_kind_count == 0);

  CT_CHECK(ccord_session_ack_message(s, ch, mid, &err) == CCORD_OK);
  CT_CHECK(ccord_session_delete_message(s, ch, mid, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_find_message(store, ch, mid) == nullptr);

  CT_CHECK(ccord_session_send_message(s, ch, "   \n ", 5, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_send_message(s, ch, "", 0, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_send_message(s, ch, "\xff\xfe", 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_send_message(s, ch, "a\0b", 3, &err) == CCORD_ERR_INVALID);
  char big[CCORD_SEND_MAX_BYTES + 2];
  memset(big, 'x', sizeof big);
  CT_CHECK(ccord_session_send_message(s, ch, big, CCORD_SEND_MAX_BYTES, &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_message(s, ch, big, CCORD_SEND_MAX_BYTES + 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_send_message(s, 424242, "x", 1, &err) == CCORD_ERR_NOT_FOUND);

  // Select another text channel of the same guild: history arrives once.
  ccord_id guild = ccord_store_selected_guild(store);
  ccord_id other = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, guild, i);
    if (c->kind == CCORD_CHANNEL_TEXT && c->id != ch) {
      other = c->id;
      break;
    }
  }
  CT_CHECK(other != CCORD_ID_NONE);
  CT_CHECK(!ccord_store_history_loaded(store, other));
  CT_CHECK(ccord_session_select_channel(s, other, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_channel(store) == other);
  pump_all(s);
  CT_CHECK(ccord_store_history_loaded(store, other));
  CT_CHECK(ccord_session_select_channel(s, 424242, &err) == CCORD_ERR_NOT_FOUND);
  // Voice channels carry their own chat: selecting and sending work; a category has none.
  ccord_id voice_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, guild, i);
    if (c->kind == CCORD_CHANNEL_VOICE) { voice_ch = c->id; break; }
  }
  CT_CHECK(voice_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(s, voice_ch, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_channel(store) == voice_ch);
  CT_CHECK(ccord_session_send_message(s, voice_ch, "x", 1, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_message_count(store, voice_ch) >= 1);
  CT_CHECK(ccord_session_select_channel(s, other, &err) == CCORD_OK);
  ccord_id category_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, guild, i);
    if (c->kind == CCORD_CHANNEL_CATEGORY) { category_ch = c->id; break; }
  }
  CT_CHECK(category_ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(s, category_ch, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_send_message(s, category_ch, "x", 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_start_stream(s, guild, voice_ch, &(ccord_stream_config){0}, &err) == CCORD_ERR_INVALID);

  // Voice lifecycle through the mock transport.
  CT_CHECK(ccord_session_join_voice(s, guild, voice_ch, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_active_voice_channel(store) == voice_ch);
  CT_CHECK(ccord_store_voice_count(store, voice_ch) >= 1);
  const ccord_voice_state *selfv = ccord_store_voice_user(store, guild, ccord_store_self_id(store));
  CT_CHECK(selfv != nullptr && selfv->channel_id == voice_ch);
  CT_CHECK(ccord_store_user_speaking(store, ccord_store_self_id(store)));
  CT_CHECK(ccord_session_set_voice_mute(s, true, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_voice_user(store, guild, ccord_store_self_id(store))->self_mute);
  CT_CHECK(!ccord_store_user_speaking(store, ccord_store_self_id(store)));
  CT_CHECK(ccord_session_set_voice_deafen(s, true, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_voice_user(store, guild, ccord_store_self_id(store))->self_deaf);
  CT_CHECK(ccord_session_set_voice_camera(s, true, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_voice_user(store, guild, ccord_store_self_id(store))->self_video);
  CT_CHECK(ccord_session_set_voice_camera(s, false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_store_voice_user(store, guild, ccord_store_self_id(store))->self_video);

  ccord_stream_config bad_codec = {.encoder = CCORD_STREAM_ENCODER_VAAPI, .codec = (ccord_stream_codec)2,
                                   .framework = CCORD_STREAM_FRAMEWORK_PIPEWIRE,
                                   .resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_session_start_stream(s, guild, voice_ch, &bad_codec, &err) == CCORD_ERR_INVALID);
  ccord_stream_config scfg = {.encoder = CCORD_STREAM_ENCODER_VAAPI, .codec = CCORD_STREAM_CODEC_AV1,
                              .framework = CCORD_STREAM_FRAMEWORK_PIPEWIRE,
                              .resolution = CCORD_STREAM_RES_1080P, .fps = CCORD_STREAM_FPS_30};
  CT_CHECK(ccord_session_start_stream(s, guild, voice_ch, &scfg, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_stream_active(store));
  CT_CHECK(ccord_store_stream(store)->config.encoder == CCORD_STREAM_ENCODER_VAAPI);
  CT_CHECK(ccord_session_stop_stream(s, guild, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_store_stream_active(store));

  // Reply and pin through the mock transport.
  CT_CHECK(ccord_session_send_message(s, ch, "reply target", 12, &err) == CCORD_OK);
  pump_all(s);
  const ccord_message *target = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1);
  CT_CHECK(target != nullptr);
  ccord_id target_id = target ? target->id : CCORD_ID_NONE;
  CT_CHECK(ccord_session_send_reply(s, ch, target_id, "reply body", 10, &err) == CCORD_OK);
  pump_all(s);
  const ccord_message *replym = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1);
  CT_CHECK(replym != nullptr && replym->reply_to_id == target_id);
  CT_CHECK(ccord_session_send_reply(s, ch, 424242, "x", 1, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_toggle_pin(s, ch, target_id, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_find_message(store, ch, target_id)->pinned);
  CT_CHECK(ccord_session_toggle_pin(s, ch, target_id, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_store_find_message(store, ch, target_id)->pinned);

  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_IDLE, "away", &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_user(store, ccord_store_self_id(store))->presence == CCORD_PRESENCE_IDLE);
  // Invisible is a real request (gateway status "invisible"), not an error; only out-of-range values are invalid.
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_OFFLINE, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_presence(s, (ccord_presence)99, nullptr, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_session_toggle_pin(s, ch, target_id, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_request_pins(s, ch, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_find_message(store, ch, target_id)->pinned);
  CT_CHECK(ccord_store_message_count(store, ch) > 0);
  const ccord_message *oldest = ccord_store_message_at(store, ch, 0);
  CT_CHECK(oldest != nullptr);
  ccord_id oldest_id = oldest ? oldest->id : CCORD_ID_NONE;
  size_t count_before_page = ccord_store_message_count(store, ch);
  CT_CHECK(ccord_session_request_history_before(s, ch, oldest_id, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_message_count(store, ch) == count_before_page + CCORD_SESSION_HISTORY_LIMIT);
  CT_CHECK(ccord_session_request_history_before(s, ch, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);

  CT_CHECK(ccord_session_leave_voice(s, guild, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_active_voice_channel(store) == CCORD_ID_NONE);

  CT_CHECK(ccord_bus_drain(bus) > 0);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_create_rejects_null(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_create(nullptr, nullptr, nullptr, &err) == nullptr);
  CT_CHECK(err.code == CCORD_ERR_INVALID);
  ccord_session_destroy(nullptr);
}

static void test_live_event_pump(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ"}};
  ccord_channel chans[1] = {{.id = 22, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                             .name = "general", .topic = ""}};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 1, .members = nullptr, .member_count = 0};
  CT_CHECK(ccord_store_set_snapshot(store, &snap, &err) == CCORD_OK);
  ccord_message hist[2] = {
    {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "a"},
    {.id = 200, .channel_id = 22, .author_id = 2, .timestamp_ms = 2, .content = "b"},
  };
  CT_CHECK(ccord_store_set_history(store, 22, hist, 2, &err) == CCORD_OK);

  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->channel = (ccord_channel){.id = 23, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                               .name = "live", .topic = "new"};
  ccord_message update = {.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1,
                          .edited_timestamp_ms = 900, .content = "edited"};
  ccord_message unknown_update = {.id = 999, .channel_id = 22, .author_id = 2, .content = "never cached"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_PRESENCE_UPDATE, .user_id = 2,
                                    .presence = CCORD_PRESENCE_DND, .status_text = "busy"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_PRESENCE_UPDATE, .user_id = 999,
                                    .presence = CCORD_PRESENCE_IDLE, .status_text = "lurking"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_TYPING_START, .guild_id = 10, .channel_id = 22,
                                    .user_id = 2, .timestamp_ms = 5555};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_CHANNEL_UPSERT, .channel = &f->channel};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_UPDATE, .channel_id = 22,
                                    .messages = &update, .message_count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_UPDATE, .channel_id = 22,
                                    .messages = &unknown_update, .message_count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_REACTION_UPDATE, .guild_id = 10, .channel_id = 22,
                                    .message_id = 100, .user_id = 1, .reaction_count = 2, .self_reacted = true,
                                    .reaction_emoji = "\xf0\x9f\x91\x8d"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_REACTION_UPDATE, .guild_id = 10, .channel_id = 22,
                                    .message_id = 998, .user_id = 1, .reaction_count = 1, .self_reacted = true,
                                    .reaction_emoji = "\xf0\x9f\x91\x8d"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_DELETE, .channel_id = 22, .message_id = 200};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_CHANNEL_DELETE, .guild_id = 10, .channel_id = 23};

  ccord_user guild_users[2] = {
    {.id = 43, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 44, .name = "Bob", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guild40 = {.id = 40, .name = "Second"};
  ccord_channel guild_chans[1] = {
    {.id = 41, .guild_id = 40, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT, .name = "second", .topic = ""},
  };
  ccord_member guild_members[1] = {{.guild_id = 40, .user_id = 43, .role_name = "Members"}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_UPSERT, .guild = &guild40,
                                    .guild_channels = guild_chans, .guild_channel_count = 1,
                                    .guild_members = guild_members, .guild_member_count = 1,
                                    .users = guild_users, .user_count = 1};
  ccord_member chunk[1] = {{.guild_id = 40, .user_id = 44, .role_name = "Members"}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = 40,
                                    .guild_members = chunk, .guild_member_count = 1,
                                    .users = &guild_users[1], .user_count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_DELETE, .guild_id = 40};

  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_send_typing(s, 22, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_session_pump(s) == f->n);
  CT_CHECK(ccord_store_user(store, 2)->presence == CCORD_PRESENCE_DND);
  CT_CHECK(strcmp(ccord_store_user(store, 2)->status_text, "busy") == 0);
  CT_CHECK(ccord_store_user(store, 999) != nullptr);
  CT_CHECK(ccord_store_user(store, 999)->presence == CCORD_PRESENCE_IDLE);
  CT_CHECK(ccord_store_typing_count(store, 22) == 1);
  CT_CHECK(ccord_store_typing_user_at(store, 22, 0) == 2);
  CT_CHECK(ccord_store_typing_expires_at(store, 22, 0) == 5555);
  const ccord_message *m = ccord_store_find_message(store, 22, 100);
  CT_CHECK(m && m->edited && m->edited_timestamp_ms == 900 && strcmp(m->content, "edited") == 0);
  CT_CHECK(m && m->reaction_count == 2 && m->self_reacted);
  CT_CHECK(m && m->reaction_kind_count == 1 && strcmp(m->reactions[0].emoji, "\xf0\x9f\x91\x8d") == 0);
  CT_CHECK(ccord_store_find_message(store, 22, 999) == nullptr);
  CT_CHECK(ccord_store_find_message(store, 22, 200) == nullptr);
  CT_CHECK(ccord_store_channel(store, 23) == nullptr);
  // Guild 40 was created and then deleted through the same pump.
  CT_CHECK(ccord_store_guild(store, 40) == nullptr);
  CT_CHECK(ccord_store_channel(store, 41) == nullptr);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_features(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mcfg = {.seed = 21, .big_channel_messages = 15, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&mcfg, &err);
  ccord_session *s = ccord_session_create(bus, store, t, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  (void)ccord_session_pump(s);

  const uint8_t img_bytes[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  CT_CHECK(ccord_session_send_message_with_file(s, 3111, "with image", 10, "pic.png", img_bytes, sizeof img_bytes,
                                                &err) == CCORD_OK);
  (void)ccord_session_pump(s);

  CT_CHECK(ccord_session_fetch_media(s, "https://cdn.discordapp.com/avatars/1001/a0.png", &err) == CCORD_OK);
  (void)ccord_session_pump(s);
  ccord_media_item item;
  CT_CHECK(ccord_session_pop_media(s, &item));
  CT_CHECK(item.len == 67 && item.data != nullptr);
  CT_CHECK(ccord_session_get_media(s, "https://cdn.discordapp.com/avatars/1001/a0.png") != nullptr);

  CT_CHECK(ccord_session_search_messages(s, 2001, 3111, "with image", 10, &err) == CCORD_OK);
  (void)ccord_session_pump(s);
  CT_CHECK(ccord_session_search_result_count(s) >= 1);
  CT_CHECK(ccord_session_search_result_at(s, 0) != nullptr);

  CT_CHECK(ccord_session_open_dm(s, 1002, &err) == CCORD_OK);
  (void)ccord_session_pump(s);
  const ccord_channel *dm = ccord_store_channel(store, 901002);
  CT_CHECK(dm != nullptr && dm->guild_id == CCORD_DM_GUILD_ID);
  // Opening a DM automatically fetches the recipient's profile.
  const ccord_user_profile *prof = ccord_store_user_profile(store, 1002);
  CT_CHECK(prof != nullptr && prof->user_id == 1002 && strcmp(prof->display_name, "Ada") == 0);
  // Requesting the same profile again is cached (no-op CCORD_OK).
  CT_CHECK(ccord_session_request_profile(s, 1002, &err) == CCORD_OK);
  // Requesting another user's profile on demand populates the store.
  CT_CHECK(ccord_store_user_profile(store, 1003) == nullptr);
  CT_CHECK(ccord_session_request_profile(s, 1003, &err) == CCORD_OK);
  (void)ccord_session_pump(s);
  CT_CHECK(ccord_store_user_profile(store, 1003) != nullptr);

  CT_CHECK(ccord_store_relationship_count(store) >= 4);
  CT_CHECK(ccord_session_set_relationship(s, 1008, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  (void)ccord_session_pump(s);
  CT_CHECK(ccord_store_relationship(store, 1008) == CCORD_RELATIONSHIP_FRIEND);
  CT_CHECK(ccord_session_set_relationship(s, 1008, CCORD_RELATIONSHIP_NONE, &err) == CCORD_OK);
  (void)ccord_session_pump(s);
  CT_CHECK(ccord_store_relationship(store, 1008) == CCORD_RELATIONSHIP_NONE);

  int16_t pcm_in[1920] = {1, 2, 3, 4};
  int16_t pcm_out[1920] = {0};
  CT_CHECK(ccord_session_push_voice_pcm(s, pcm_in, 960, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pull_voice_pcm(s, pcm_out, 960) == 960);
  CT_CHECK(pcm_out[0] == 1 && pcm_out[3] == 4);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_snapshot_initial_history_and_fallback(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;

  ccord_user users[1] = {{.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE}};
  ccord_guild guilds[2] = {{.id = 10, .name = "VoiceOnly"}, {.id = 20, .name = "TextGuild"}};
  ccord_channel chans[3] = {
    {.id = 101, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "Voice", .topic = ""},
    {.id = 201, .guild_id = 20, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = ""},
    {.id = 202, .guild_id = 20, .parent_id = 201, .kind = CCORD_CHANNEL_TEXT, .name = "welcome", .topic = ""},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 1,
    .guilds = guilds, .guild_count = 2,
    .channels = chans, .channel_count = 3,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};

  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_selected_guild(store) == 20);
  CT_CHECK(ccord_store_selected_channel(store) == 202);
  CT_CHECK(f->history_request_count == 1);
  CT_CHECK(f->last_requested_history_channel == 202);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_history_error_retry_and_author_upserts(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;

  ccord_user users[1] = {{.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE}};
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ"}};
  ccord_channel chans[1] = {{.id = 22, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""}};
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 1,
    .guilds = guilds, .guild_count = 1,
    .channels = chans, .channel_count = 1,
  };
  ccord_user hist_author = {.id = 777, .name = "WebhookBot", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_message hist_msg = {.id = 500, .channel_id = 22, .author_id = 777, .timestamp_ms = 123, .content = "loaded on retry"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ERROR, .status = CCORD_ERR_IO, .channel_id = 22, .text = "transient timeout"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY, .channel_id = 22,
                                    .messages = &hist_msg, .message_count = 1,
                                    .users = &hist_author, .user_count = 1};

  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) == 3);
  // Initial snapshot requested history (1), CCORD_NET_ERROR auto-retried once (2), and CCORD_NET_HISTORY succeeded.
  CT_CHECK(f->history_request_count == 2);
  CT_CHECK(ccord_store_history_loaded(store, 22));
  CT_CHECK(ccord_store_message_count(store, 22) == 1);
  // Since the history chunk returned < 50 messages (1 message), history_has_more is now false!
  CT_CHECK(!ccord_session_history_has_more(s, 22));
  CT_CHECK(ccord_session_request_history_before(s, 22, 500, &err) == CCORD_OK);
  CT_CHECK(f->history_before_request_count == 0);
  CT_CHECK(ccord_store_user(store, 777) != nullptr);
  CT_CHECK(strcmp(ccord_store_user(store, 777)->name, "WebhookBot") == 0);
  // Verify guild channel history also seeded author 777 into guild 10's member list!
  CT_CHECK(ccord_store_member_count(store, 10) >= 1);
  bool found_777 = false;
  for (size_t mi = 0; mi < ccord_store_member_count(store, 10); mi++) {
    const ccord_member *m = ccord_store_member_at(store, 10, mi);
    if (m && m->user_id == 777) {
      found_777 = true;
      CT_CHECK(strcmp(m->role_name, "Members") == 0);
    }
  }
  CT_CHECK(found_777);

  // Verify CCORD_NET_MESSAGE_CREATE on a guild channel seeds both explicit ev.members and message author into guild members:
  ccord_user live_author = {.id = 888, .name = "LiveAuthor", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_member live_member = {.guild_id = 10, .user_id = 888, .role_name = "Mod", .role_color = 0x1ABC9Cu, .role_rank = 1};
  ccord_message live_msg = {.id = 501, .channel_id = 22, .author_id = 888, .timestamp_ms = 124, .content = "live guild message"};
  f->ev[f->n++] = (ccord_net_event){
    .type = CCORD_NET_MESSAGE_CREATE,
    .guild_id = 10,
    .messages = &live_msg,
    .message_count = 1,
    .users = &live_author,
    .user_count = 1,
    .guild_members = &live_member,
    .guild_member_count = 1,
  };
  CT_CHECK(ccord_session_pump(s) == 1);
  bool found_888 = false;
  for (size_t mi = 0; mi < ccord_store_member_count(store, 10); mi++) {
    const ccord_member *m = ccord_store_member_at(store, 10, mi);
    if (m && m->user_id == 888) {
      found_888 = true;
      CT_CHECK(strcmp(m->role_name, "Mod") == 0 && m->role_rank == 1);
    }
  }
  CT_CHECK(found_888);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_pagination_dedup_history_failed_and_cross_guild_select(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;

  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 43, .name = "Ada", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
  };
  ccord_guild guilds[2] = {{.id = 10, .name = "G10"}, {.id = 20, .name = "G20"}};
  ccord_channel chans[3] = {
    {.id = 101, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "g10-text", .topic = ""},
    {.id = 201, .guild_id = 20, .kind = CCORD_CHANNEL_TEXT, .name = "g20-text", .topic = ""},
    {.id = 901, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ada", .topic = "", .user_id = 43},
  };
  ccord_snapshot snap = {
    .self_id = 1, .users = users, .user_count = 2,
    .guilds = guilds, .guild_count = 2,
    .channels = chans, .channel_count = 3,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};

  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(f->last_selected_guild == 10);
  CT_CHECK(f->last_selected_channel_guild == 10);
  CT_CHECK(f->last_selected_channel_id == 101);

  // Selecting a channel in another guild calls transport select_channel for the new guild and channel.
  CT_CHECK(ccord_session_select_channel(s, 201, &err) == CCORD_OK);
  CT_CHECK(f->last_selected_guild == 20);
  CT_CHECK(f->last_selected_channel_guild == 20);
  CT_CHECK(f->last_selected_channel_id == 201);
  CT_CHECK(f->last_requested_history_channel == 201);

  // Selecting a DM channel automatically requests the DM recipient's profile once.
  CT_CHECK(ccord_session_select_channel(s, 901, &err) == CCORD_OK);
  CT_CHECK(f->profile_request_count == 1);
  CT_CHECK(ccord_session_request_profile(s, 43, &err) == CCORD_OK);
  CT_CHECK(f->profile_request_count == 1); // deduplicated

  // In-flight pagination request is deduplicated until CCORD_NET_HISTORY or CCORD_NET_HISTORY_FAILED arrives.
  CT_CHECK(ccord_session_request_history_before(s, 201, 5000, &err) == CCORD_OK);
  CT_CHECK(f->history_before_request_count == 1);
  CT_CHECK(ccord_session_request_history_before(s, 201, 5000, &err) == CCORD_OK);
  CT_CHECK(f->history_before_request_count == 1); // duplicate ignored while in-flight

  // Simulate CCORD_NET_HISTORY_FAILED: clears in-flight state while keeping has_more = true so retry works.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_HISTORY_FAILED, .channel_id = 201};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_history_has_more(s, 201));
  CT_CHECK(ccord_session_request_history_before(s, 201, 5000, &err) == CCORD_OK);
  CT_CHECK(f->history_before_request_count == 2);

  // Even if Gateway WebSocket state transitions to CONNECTING or FAILED after authentication (self_id != 0),
  // channel history, pagination, and profile requests continue to work over REST, and profile errors allow retry.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTING};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ERROR, .status = CCORD_ERR_IO, .user_id = 43, .text = "profile timeout"};
  CT_CHECK(ccord_session_pump(s) == 2);
  CT_CHECK(ccord_session_state(s) == CCORD_CONN_CONNECTING);
  CT_CHECK(ccord_session_select_channel(s, 901, &err) == CCORD_OK);
  CT_CHECK(f->last_requested_history_channel == 901);
  CT_CHECK(f->profile_request_count == 2); // retried after CCORD_NET_ERROR cleared profile_requested for user 43

  // Live MESSAGE_CREATE on a DM channel not yet in the store auto-creates the DM channel and sorts it to the top of DMs!
  ccord_user new_dm_user = {.id = 99, .name = "Zoe", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_message new_dm_msg = {.id = 99000, .channel_id = 999, .author_id = 99, .timestamp_ms = 500, .content = "hi in dm"};
  f->ev[f->n++] = (ccord_net_event){
    .type = CCORD_NET_MESSAGE_CREATE,
    .guild_id = CCORD_ID_NONE,
    .channel_id = 999,
    .messages = &new_dm_msg,
    .message_count = 1,
    .users = &new_dm_user,
    .user_count = 1,
  };
  CT_CHECK(ccord_session_pump(s) == 1);
  const ccord_channel *top_dm = ccord_store_channel_at(store, CCORD_DM_GUILD_ID, 0);
  CT_CHECK(top_dm != nullptr && top_dm->id == 999 && top_dm->last_message_id == 99000);
  CT_CHECK(top_dm != nullptr && strcmp(top_dm->name, "Zoe") == 0 && top_dm->user_id == 99);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_forum_channel_session_flow(void)
{
  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 7, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;

  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);

  // Mock transport seeds #design-forum (id = 3115) in guild 2001 with 5 tags and 5 posts
  CT_CHECK(ccord_store_channel_is_forum(store, 3115));
  const ccord_forum_meta *meta = ccord_store_forum_meta(store, 3115);
  CT_CHECK(meta != nullptr && meta->tag_count == 5);
  size_t nposts = 0;
  const ccord_forum_post *posts = ccord_store_forum_posts(store, 3115, &nposts);
  CT_CHECK(nposts == 5 && posts != nullptr);

  // Selecting the forum channel requests forum posts instead of message history
  CT_CHECK(ccord_session_select_channel(s, 3115, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  const ccord_forum_state *fstate = ccord_store_forum_state(store, 3115);
  CT_CHECK(fstate != nullptr && fstate->loaded && !fstate->loading);

  // Creating a new forum post appends the post, opens its thread channel, and selects it
  ccord_id tags[2] = {4101, 4103};
  CT_CHECK(ccord_session_create_forum_post(s, 3115, "New RFC Post", 12,
                                           "Starter body content", 20,
                                           tags, 2, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  posts = ccord_store_forum_posts(store, 3115, &nposts);
  CT_CHECK(nposts == 6);
  ccord_id created_tid = ccord_store_selected_channel(store);
  CT_CHECK(created_tid != 3115 && created_tid != CCORD_ID_NONE);
  const ccord_channel *tch = ccord_store_channel(store, created_tid);
  CT_CHECK(tch != nullptr && tch->parent_id == 3115 && strcmp(tch->name, "New RFC Post") == 0);
  CT_CHECK(ccord_store_message_count(store, created_tid) >= 1);

  // Opening an existing seeded forum post (3152) materialises it and loads its starter + reply messages
  CT_CHECK(ccord_session_open_forum_post(s, 3152, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_selected_channel(store) == 3152);
  CT_CHECK(ccord_store_message_count(store, 3152) == 4);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// A refused action is queued for the UI with its text; the caller owns the text, the queue is bounded, and a
// failure nobody collected does not leak.
static void test_action_failures(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND, .channel_id = 22,
                                    .status = CCORD_ERR_PERMISSION, .text = "You don't have permission to do that.",
                                    .draft = "my long message", .draft_len = 15};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_PIN, .channel_id = 22,
                                    .message_id = 9, .status = CCORD_ERR_NOT_FOUND, .text = "It no longer exists."};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_DELETE, .channel_id = 22,
                                    .message_id = 10, .status = CCORD_ERR_PROTOCOL};  // no reason text: falls back
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  ccord_action_failure out;
  CT_CHECK(!ccord_session_pop_failure(s, &out));  // nothing yet
  CT_CHECK(ccord_session_pump(s) == 3);

  CT_CHECK(ccord_session_pop_failure(s, &out));
  CT_CHECK(out.action == CCORD_ACTION_SEND && out.channel_id == 22 && out.status == CCORD_ERR_PERMISSION);
  CT_CHECK(strcmp(out.reason, "You don't have permission to do that.") == 0);
  CT_CHECK(out.draft && out.draft_len == 15 && memcmp(out.draft, "my long message", 15) == 0);
  CT_CHECK(out.draft != f->ev[0].draft);  // a copy: the transport's buffer is only valid until the next poll
  ccord_action_failure_free(&out);
  CT_CHECK(out.draft == nullptr);

  CT_CHECK(ccord_session_pop_failure(s, &out));  // oldest first
  CT_CHECK(out.action == CCORD_ACTION_PIN && out.message_id == 9 && out.draft == nullptr);
  CT_CHECK(ccord_session_pop_failure(s, &out));
  CT_CHECK(out.action == CCORD_ACTION_DELETE && out.reason[0] != '\0');
  CT_CHECK(!ccord_session_pop_failure(s, &out));
  CT_CHECK(!ccord_session_pop_failure(nullptr, &out));
  CT_CHECK(!ccord_session_pop_failure(s, nullptr));
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Far more failures than the queue holds: it stays bounded, keeps the newest, and frees what it drops.
static void test_action_failures_are_bounded(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  for (int round = 0; round < 3; round++) {  // 3 pumps x 12 events: the fake holds 16
    f->n = f->i = 0;
    for (int i = 0; i < 12; i++) {
      f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND,
                                        .channel_id = 22, .message_id = (ccord_id)(round * 12 + i + 1),
                                        .status = CCORD_ERR_IO, .text = "Network error.",
                                        .draft = "text", .draft_len = 4};
    }
    CT_CHECK(ccord_session_pump(s) == 12);
  }
  size_t popped = 0;
  ccord_id last = 0;
  ccord_action_failure out;
  while (ccord_session_pop_failure(s, &out)) {
    popped++;
    last = out.message_id;
    ccord_action_failure_free(&out);
  }
  CT_CHECK(popped == 8);   // SESSION_MAX_FAILURES
  CT_CHECK(last == 36);    // the newest survived
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static const ccord_member *find_member(const ccord_store *st, ccord_id guild, ccord_id user)
{
  for (size_t i = 0; i < ccord_store_member_count(st, guild); i++) {
    const ccord_member *m = ccord_store_member_at(st, guild, i);
    if (m && m->user_id == user) return m;
  }
  return nullptr;
}

// Server administration through the session: invalid requests never reach the transport, valid ones change nothing
// until the server answers, the answer is what changes the store, and a refusal changes nothing and is reported.
static void test_session_admin(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 7, .big_channel_messages = 50, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);

  const ccord_guild *g0 = ccord_store_guild(store, 2001);
  const ccord_guild *owned = ccord_store_guild(store, 2002);   // the mock gives this one an owner other than us
  CT_CHECK(g0 && owned && owned->owner_id != CCORD_ID_NONE && owned->owner_id != ccord_store_self_id(store));
  ccord_id self = ccord_store_self_id(store);
  ccord_id other = 0;
  for (size_t i = 0; i < ccord_store_member_count(store, 2001); i++) {
    const ccord_member *m = ccord_store_member_at(store, 2001, i);
    if (m && m->user_id != self && m->user_id != owned->owner_id) {
      other = m->user_id;
      break;
    }
  }
  CT_CHECK(other != 0);
  ccord_id chan = 3111;   // a text channel of server 2001
  CT_CHECK(ccord_store_channel(store, chan) && ccord_store_channel(store, chan)->guild_id == 2001);
  ccord_admin_request r = {0};

  // ---- rejected before the transport is involved ----
  CT_CHECK(ccord_session_admin(nullptr, &r, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_admin(s, nullptr, &err) == CCORD_ERR_INVALID);
  memset(&r, 0, sizeof r);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_INVALID);   // op NONE
  CT_CHECK(ccord_admin_kick(&r, 9999, other, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_NOT_FOUND);   // unknown server
  CT_CHECK(ccord_admin_delete_channel(&r, 2001, 424242, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_NOT_FOUND);   // unknown channel
  CT_CHECK(ccord_admin_delete_channel(&r, 2002, chan, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_INVALID);     // channel belongs to another server
  CT_CHECK(ccord_admin_kick(&r, 2001, self, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_INVALID);     // yourself
  CT_CHECK(ccord_admin_ban(&r, 2002, owned->owner_id, 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);  // the owner cannot be banned
  CT_CHECK(ccord_admin_timeout(&r, 2002, owned->owner_id, 1000, 60000, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == 0);   // none of the above reached the transport

  // ---- accepted: nothing changes until the server answers ----
  size_t nchan = ccord_store_channel_count(store, 2001);
  CT_CHECK(ccord_admin_create_channel(&r, 2001, "ops", 0, 3110, nullptr, "why not", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == 1);
  CT_CHECK(ccord_mock_last_admin(tr) && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_CREATE_CHANNEL);
  CT_CHECK(ccord_store_channel_count(store, 2001) == nchan);   // no local invention
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_channel_count(store, 2001) == nchan + 1);
  const ccord_channel *made = ccord_store_channel(store, 8000);   // the id the server assigned
  CT_CHECK(made && strcmp(made->name, "ops") == 0 && made->parent_id == 3110);

  ccord_admin_channel_edit e = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  e.name = "operations";
  CT_CHECK(ccord_admin_edit_channel(&r, 2001, 8000, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_channel(store, 8000)->name, "ops") == 0);   // not yet
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(strcmp(ccord_store_channel(store, 8000)->name, "operations") == 0);

  CT_CHECK(ccord_admin_create_thread(&r, 2001, chan, "planning", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_channel(store, 8001) && ccord_store_channel(store, 8001)->parent_id == chan);

  CT_CHECK(ccord_admin_delete_channel(&r, 2001, 8000, "done", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_store_channel(store, 8000) != nullptr);   // still there until Discord confirms
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_channel(store, 8000) == nullptr);

  // A rename must not look like "the server lost all its channels".
  size_t before = ccord_store_channel_count(store, 2001);
  CT_CHECK(before > 0);
  CT_CHECK(ccord_admin_edit_guild(&r, 2001, "Renamed HQ", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_store_guild(store, 2001)->name, "Renamed HQ") != 0);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(strcmp(ccord_store_guild(store, 2001)->name, "Renamed HQ") == 0);
  CT_CHECK(ccord_store_channel_count(store, 2001) == before);
  CT_CHECK(ccord_store_member_count(store, 2001) > 1);

  // Members: nickname, timeout (and lifting it), then removal.
  CT_CHECK(ccord_admin_set_nick(&r, 2001, other, self, "Ace", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(strcmp(find_member(store, 2001, other)->nick, "Ace") == 0);
  CT_CHECK(ccord_admin_set_nick(&r, 2001, other, self, "", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(find_member(store, 2001, other)->nick[0] == '\0');   // cleared nickname sticks

  CT_CHECK(ccord_admin_timeout(&r, 2001, other, 1794312000000LL, 600000, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(find_member(store, 2001, other)->timeout_until_ms == 0);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(find_member(store, 2001, other)->timeout_until_ms == 1794312600000LL);
  CT_CHECK(ccord_admin_timeout(&r, 2001, other, 1794312000000LL, 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(find_member(store, 2001, other)->timeout_until_ms == 0);   // lifted timeout sticks

  size_t members = ccord_store_member_count(store, 2001);
  CT_CHECK(ccord_admin_kick(&r, 2001, other, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_store_member_count(store, 2001) == members);   // not yet
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_member_count(store, 2001) == members - 1 && !find_member(store, 2001, other));
  ccord_action_failure out;
  CT_CHECK(!ccord_session_pop_failure(s, &out));   // all of that succeeded: nothing to report

  // ---- refused by the server: nothing changes, the user is told why ----
  ccord_mock_set_refuse_writes(tr, true);
  size_t admin_before = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_admin_delete_channel(&r, 2001, chan, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);   // the call itself is accepted; the refusal is asynchronous
  CT_CHECK(ccord_mock_admin_count(tr) == admin_before + 1);
  (void)pump_all(s);
  CT_CHECK(ccord_store_channel(store, chan) != nullptr);    // still there
  CT_CHECK(ccord_session_pop_failure(s, &out));
  CT_CHECK(out.action == CCORD_ACTION_ADMIN && out.admin_op == CCORD_ADMIN_DELETE_CHANNEL);
  CT_CHECK(out.channel_id == chan && out.guild_id == 2001);
  CT_CHECK(out.status == CCORD_ERR_PERMISSION && strstr(out.reason, "Missing Permissions") != nullptr);
  ccord_action_failure_free(&out);

  size_t members2 = ccord_store_member_count(store, 2001);
  ccord_id victim = 0;
  for (size_t i = 0; i < ccord_store_member_count(store, 2001) && !victim; i++) {
    const ccord_member *m = ccord_store_member_at(store, 2001, i);
    if (m && m->user_id != self && m->user_id != owned->owner_id) victim = m->user_id;
  }
  CT_CHECK(victim != 0);
  CT_CHECK(ccord_admin_ban(&r, 2001, victim, 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  (void)pump_all(s);
  CT_CHECK(ccord_store_member_count(store, 2001) == members2);   // a refused ban removes nobody
  CT_CHECK(ccord_session_pop_failure(s, &out));
  CT_CHECK(out.admin_op == CCORD_ADMIN_BAN && out.user_id == victim);
  ccord_action_failure_free(&out);
  CT_CHECK(!ccord_session_pop_failure(s, &out));

  // ---- notification settings are the user's own: no permission applies, but the target has to make sense ----
  ccord_admin_notify_edit ne = CCORD_ADMIN_NOTIFY_EDIT_INIT;
  ne.muted = 1;
  size_t before_notify = ccord_mock_admin_count(tr);
  ne.channel_id = chan;   // a channel of server 2001 ...
  CT_CHECK(ccord_admin_set_notify(&r, 2002, &ne, 1000, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_INVALID);   // ... is not one of server 2002's
  CT_CHECK(ccord_admin_set_notify(&r, CCORD_DM_GUILD_ID, &ne, 1000, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_INVALID);   // nor one of the direct messages
  ne.channel_id = 424242;
  CT_CHECK(ccord_admin_set_notify(&r, 2001, &ne, 1000, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_NOT_FOUND);   // a channel nobody has
  ne.channel_id = CCORD_ID_NONE;
  CT_CHECK(ccord_admin_set_notify(&r, 9999, &ne, 1000, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_NOT_FOUND);   // a server we are not in
  CT_CHECK(ccord_mock_admin_count(tr) == before_notify);                // none of those reached the transport

  // The direct messages are addressed by the pseudo id and need no server record of their own.
  CT_CHECK(ccord_admin_set_notify(&r, CCORD_DM_GUILD_ID, &ne, 1000, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == before_notify + 1);
  CT_CHECK(ccord_mock_last_admin(tr) && ccord_mock_last_admin(tr)->op == CCORD_ADMIN_SET_NOTIFY &&
           ccord_mock_last_admin(tr)->guild_id == CCORD_DM_GUILD_ID);
  (void)pump_all(s);
  ccord_notify_scope dm_row;
  CT_CHECK(ccord_store_notify_scope(store, CCORD_DM_GUILD_ID, CCORD_ID_NONE, &dm_row) && dm_row.muted);

  // A channel of the server: accepted, and the store only changes when the answer comes back.
  ne.channel_id = chan;
  CT_CHECK(!ccord_store_channel_muted(store, chan, 0));
  CT_CHECK(ccord_admin_set_notify(&r, 2001, &ne, 1000, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_channel_muted(store, chan, 0));
  (void)pump_all(s);
  CT_CHECK(ccord_store_channel_muted(store, chan, 0));
  CT_CHECK(!ccord_session_pop_failure(s, &out));

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static size_t g_perm_admin_calls;
static ccord_status perm_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *err)
{
  (void)t; (void)req;
  g_perm_admin_calls++;
  ccord_error_clear(err);
  return CCORD_OK;
}

// Roles, member roles and channel overwrites arrive with the snapshot and change through role / member / channel
// events; the permission engine answers from them, and an admin request aimed at someone it can never touch is
// stopped before it reaches the transport (while one that merely might be refused is left to Discord).
static void test_session_permission_data(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, HELPER = 2, PEER = 3, LOWLY = 4, OWNER = 9, GUILD = 10, CHAN = 22, THREAD = 23 };
  ccord_user users[5] = {
    {.id = SELF, .name = "You", .status_text = ""}, {.id = HELPER, .name = "Helper", .status_text = ""},
    {.id = PEER, .name = "Peer", .status_text = ""}, {.id = LOWLY, .name = "Lowly", .status_text = ""},
    {.id = OWNER, .name = "Owner", .status_text = ""},
  };
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ", .owner_id = OWNER}};
  ccord_channel chans[1] = {{.id = CHAN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""}};
  ccord_member members[5] = {
    {.guild_id = GUILD, .user_id = SELF, .role_name = "Members"}, {.guild_id = GUILD, .user_id = HELPER, .role_name = "Members"},
    {.guild_id = GUILD, .user_id = PEER, .role_name = "Members"}, {.guild_id = GUILD, .user_id = LOWLY, .role_name = "Members"},
    {.guild_id = GUILD, .user_id = OWNER, .role_name = "Members"},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 5, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 1, .members = members, .member_count = 5};
  const uint64_t base = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY;
  ccord_role roles[3] = {
    {.id = GUILD, .guild_id = GUILD, .name = "@everyone", .permissions = base},
    {.id = 200, .guild_id = GUILD, .name = "Mods", .position = 3,
     .permissions = CCORD_PERM_KICK_MEMBERS | CCORD_PERM_BAN_MEMBERS | CCORD_PERM_MODERATE_MEMBERS},
    {.id = 201, .guild_id = GUILD, .name = "Helpers", .position = 1, .permissions = CCORD_PERM_ADD_REACTIONS},
  };
  ccord_id mods_only[1] = {200}, helpers_only[1] = {201};
  ccord_member_roles mroles[4] = {
    {.guild_id = GUILD, .user_id = SELF, .role_ids = mods_only, .count = 1},
    {.guild_id = GUILD, .user_id = HELPER, .role_ids = helpers_only, .count = 1},
    {.guild_id = GUILD, .user_id = PEER, .role_ids = mods_only, .count = 1},
    {.guild_id = GUILD, .user_id = LOWLY, .role_ids = nullptr, .count = 0},
  };
  ccord_overwrite read_only[1] = {{.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_SEND_MESSAGES}};
  ccord_channel_overwrites ow[1] = {{.channel_id = CHAN, .items = read_only, .count = 1}};

  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  static ccord_transport_vtable perm_vt;
  perm_vt = fake_vt;
  perm_vt.admin_action = perm_admin_action;
  f->base.vt = &perm_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap,
                                    .roles = roles, .role_count = 3, .member_roles = mroles, .member_role_count = 4,
                                    .overwrites = ow, .overwrite_count = 1};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);

  // What the snapshot said is what the store now knows.
  CT_CHECK(ccord_store_roles_known(store, GUILD));
  size_t nroles = 0;
  const ccord_role *table = ccord_store_roles(store, GUILD, &nroles);
  CT_CHECK(table && nroles == 3 && table[0].id == 200);   // highest first
  const ccord_id *ids = nullptr;
  size_t nids = 0;
  CT_CHECK(ccord_store_member_roles(store, GUILD, SELF, &ids, &nids) && nids == 1 && ids[0] == 200);
  CT_CHECK(ccord_store_member_roles(store, GUILD, LOWLY, &ids, &nids) && nids == 0);   // known to have none
  CT_CHECK(!ccord_store_member_roles(store, GUILD, OWNER, &ids, &nids));               // never reported: unknown
  size_t now = 0;
  CT_CHECK(ccord_store_channel_overwrites(store, CHAN, &now) != nullptr && now == 1);
  uint64_t self_guild = ccord_store_permissions(store, GUILD, CCORD_ID_NONE, SELF, 0);
  CT_CHECK((self_guild & CCORD_PERM_BAN_MEMBERS) != 0 && (self_guild & CCORD_PERM_SEND_MESSAGES) != 0);
  uint64_t self_chan = ccord_store_permissions(store, GUILD, CHAN, SELF, 0);
  CT_CHECK((self_chan & CCORD_PERM_SEND_MESSAGES) == 0 && (self_chan & CCORD_PERM_VIEW_CHANNEL) != 0);   // read-only here
  CT_CHECK(ccord_store_can_act_on(store, CCORD_PERM_ACT_KICK, GUILD, SELF, LOWLY) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_act_on(store, CCORD_PERM_ACT_KICK, GUILD, SELF, HELPER) == CCORD_PERM_ALLOWED);
  CT_CHECK(ccord_store_can_act_on(store, CCORD_PERM_ACT_KICK, GUILD, SELF, PEER) == CCORD_PERM_TARGET_HIGHER);   // same rank
  CT_CHECK(ccord_store_can_act_on(store, CCORD_PERM_ACT_BAN, GUILD, SELF, OWNER) == CCORD_PERM_TARGET_OWNER);

  // The pre-check: what can never work stops here, what might work goes to the transport.
  ccord_admin_request r;
  CT_CHECK(ccord_admin_kick(&r, GUILD, OWNER, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION && strstr(err.message, "owner") != nullptr);
  CT_CHECK(ccord_admin_ban(&r, GUILD, PEER, 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION && strstr(err.message, "below yours") != nullptr);
  CT_CHECK(ccord_admin_timeout(&r, GUILD, PEER, 1000, 60000, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_kick(&r, GUILD, SELF, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_INVALID);
  CT_CHECK(g_perm_admin_calls == 0);
  CT_CHECK(ccord_admin_kick(&r, GUILD, HELPER, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == 1);
  // Mods hold no Manage Nicknames here, yet that is not stopped locally: our picture of roles can lag, so a missing
  // permission is Discord's to rule on (and it will answer with a failure notice).
  CT_CHECK(ccord_admin_set_nick(&r, GUILD, PEER, SELF, "Pal", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == 2);

  // Role changes arrive as events. Dropping Mods' ban right and raising Helpers above Mods changes who may act.
  ccord_role mods_v2 = {.id = 200, .guild_id = GUILD, .name = "Mods", .position = 3,
                        .permissions = CCORD_PERM_KICK_MEMBERS | CCORD_PERM_MANAGE_CHANNELS};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = GUILD, .roles = &mods_v2, .role_count = 1,
                                    .role_id = 200};
  ccord_role helpers_v2 = {.id = 201, .guild_id = GUILD, .name = "Helpers", .position = 5, .permissions = CCORD_PERM_ADD_REACTIONS};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = GUILD, .roles = &helpers_v2, .role_count = 1,
                                    .role_id = 201};
  ccord_role fresh = {.id = 202, .guild_id = GUILD, .name = "New", .position = 2, .permissions = CCORD_PERM_ADD_REACTIONS};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = GUILD, .roles = &fresh, .role_count = 1,
                                    .role_id = 202};
  CT_CHECK(ccord_session_pump(s) == 3);
  self_guild = ccord_store_permissions(store, GUILD, CCORD_ID_NONE, SELF, 0);
  CT_CHECK((self_guild & CCORD_PERM_BAN_MEMBERS) == 0 && (self_guild & CCORD_PERM_MANAGE_CHANNELS) != 0);
  CT_CHECK(ccord_store_can_act_on(store, CCORD_PERM_ACT_KICK, GUILD, SELF, HELPER) == CCORD_PERM_TARGET_HIGHER);
  CT_CHECK(ccord_store_role(store, GUILD, 202) != nullptr);
  CT_CHECK(ccord_admin_kick(&r, GUILD, HELPER, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION && g_perm_admin_calls == 2);

  // A deleted role is gone, and with it from every member; deleting one we never had is not an error.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_DELETE, .guild_id = GUILD, .role_id = 201};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_DELETE, .guild_id = GUILD, .role_id = 777};
  CT_CHECK(ccord_session_pump(s) == 2);
  CT_CHECK(ccord_store_role(store, GUILD, 201) == nullptr);
  CT_CHECK(ccord_store_member_roles(store, GUILD, HELPER, &ids, &nids) && nids == 0);
  CT_CHECK(ccord_store_can_act_on(store, CCORD_PERM_ACT_KICK, GUILD, SELF, HELPER) == CCORD_PERM_ALLOWED);

  // GUILD_MEMBER_UPDATE: a member gains a role. A member event without role ids leaves what is known alone.
  ccord_id lowly_roles[1] = {202};
  ccord_member_roles mr_lowly = {.guild_id = GUILD, .user_id = LOWLY, .role_ids = lowly_roles, .count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = GUILD, .guild_members = &members[3],
                                    .guild_member_count = 1, .member_roles = &mr_lowly, .member_role_count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = GUILD, .guild_members = &members[2],
                                    .guild_member_count = 1};
  CT_CHECK(ccord_session_pump(s) == 2);
  CT_CHECK(ccord_store_member_roles(store, GUILD, LOWLY, &ids, &nids) && nids == 1 && ids[0] == 202);
  CT_CHECK(ccord_store_member_roles(store, GUILD, PEER, &ids, &nids) && nids == 1 && ids[0] == 200);

  // CHANNEL_UPDATE with overwrites replaces them (here: sending is allowed again); without them nothing changes.
  ccord_overwrite open_up[1] = {{.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_SEND_MESSAGES}};
  ccord_channel_overwrites ow2[1] = {{.channel_id = CHAN, .items = open_up, .count = 1}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_CHANNEL_UPSERT, .guild_id = GUILD, .channel = &chans[0],
                                    .channel_id = CHAN, .overwrites = ow2, .overwrite_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK((ccord_store_permissions(store, GUILD, CHAN, SELF, 0) & CCORD_PERM_SEND_MESSAGES) != 0);
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_CHANNEL_UPSERT, .guild_id = GUILD, .channel = &chans[0],
                                    .channel_id = CHAN};
  ccord_overwrite closed[1] = {{.id = GUILD, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL}};
  ccord_channel_overwrites ow3[1] = {{.channel_id = 4242, .items = closed, .count = 1}};   // a channel we do not have
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_CHANNEL_UPSERT, .guild_id = GUILD, .channel = &chans[0],
                                    .channel_id = CHAN, .overwrites = ow3, .overwrite_count = 1};
  CT_CHECK(ccord_session_pump(s) == 2);
  CT_CHECK((ccord_store_permissions(store, GUILD, CHAN, SELF, 0) & CCORD_PERM_SEND_MESSAGES) != 0);   // unchanged
  CT_CHECK(ccord_store_channel_overwrites(store, 4242, &now) == nullptr);

  // A thread is governed by its parent's overwrites.
  ccord_channel thread = {.id = THREAD, .guild_id = GUILD, .parent_id = CHAN, .kind = CCORD_CHANNEL_TEXT,
                          .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD, .name = "t", .topic = ""};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_CHANNEL_UPSERT, .guild_id = GUILD, .channel = &thread,
                                    .channel_id = THREAD};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK((ccord_store_permissions(store, GUILD, THREAD, SELF, 0) & CCORD_PERM_SEND_MESSAGES) != 0);

  // Joining a server brings its own table, our roles in it and its channels' overwrites in one GUILD_UPSERT.
  ccord_guild joined = {.id = 11, .name = "Joined"};
  ccord_channel joined_chans[1] = {{.id = 31, .guild_id = 11, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = ""}};
  ccord_role joined_roles[2] = {
    {.id = 11, .guild_id = 11, .name = "@everyone", .permissions = base},
    {.id = 300, .guild_id = 11, .name = "Boss", .position = 1, .permissions = CCORD_PERM_ADMINISTRATOR},
  };
  ccord_id boss[1] = {300};
  ccord_member_roles joined_mr[1] = {{.guild_id = 11, .user_id = SELF, .role_ids = boss, .count = 1}};
  ccord_overwrite hidden[1] = {{.id = 11, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL}};
  ccord_channel_overwrites joined_ow[1] = {{.channel_id = 31, .items = hidden, .count = 1}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_UPSERT, .guild = &joined, .guild_id = 11,
                                    .guild_channels = joined_chans, .guild_channel_count = 1,
                                    .roles = joined_roles, .role_count = 2, .member_roles = joined_mr, .member_role_count = 1,
                                    .overwrites = joined_ow, .overwrite_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_roles_known(store, 11) && ccord_store_role(store, 11, 300) != nullptr);
  CT_CHECK(ccord_store_member_roles(store, 11, SELF, &ids, &nids) && nids == 1 && ids[0] == 300);
  CT_CHECK(ccord_store_channel_overwrites(store, 31, &now) != nullptr && now == 1);
  CT_CHECK(ccord_perm_has(ccord_store_permissions(store, 11, 31, SELF, 0), CCORD_PERM_BAN_MEMBERS));   // administrator
  CT_CHECK(ccord_store_roles_known(store, GUILD));   // the other server is untouched

  // Leaving the server forgets its permission data.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_DELETE, .guild_id = GUILD};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(!ccord_store_roles_known(store, GUILD) && ccord_store_channel_overwrites(store, CHAN, &now) == nullptr);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Server Settings: what the answers do to the store, and which role requests are stopped before they leave.
static void test_session_server_settings(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ADA = 2, OWNER = 9, GUILD = 10, GUILD2 = 11, CHAN = 22, MANAGERS = 200, MODS = 201, HELPERS = 202, BOTROLE = 203 };
  ccord_user users[3] = {
    {.id = SELF, .name = "You", .status_text = ""}, {.id = ADA, .name = "Ada", .status_text = ""},
    {.id = OWNER, .name = "Owner", .status_text = ""},
  };
  ccord_guild guilds[2] = {{.id = GUILD, .name = "HQ", .owner_id = OWNER}, {.id = GUILD2, .name = "Other", .owner_id = OWNER,
                                                                            .self_permissions = CCORD_PERM_MANAGE_ROLES}};
  ccord_channel chans[1] = {{.id = CHAN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""}};
  ccord_member members[4] = {
    {.guild_id = GUILD, .user_id = SELF, .role_name = "Members"}, {.guild_id = GUILD, .user_id = ADA, .role_name = "Members"},
    {.guild_id = GUILD2, .user_id = SELF, .role_name = "Members"}, {.guild_id = GUILD2, .user_id = ADA, .role_name = "Members"},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 2,
                         .channels = chans, .channel_count = 1, .members = members, .member_count = 4};
  const uint64_t base = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY;
  // SELF holds "Managers" (position 5): may manage roles and bans, but is neither owner nor administrator.
  ccord_role roles[5] = {
    {.id = GUILD, .guild_id = GUILD, .name = "@everyone", .permissions = base},
    {.id = MANAGERS, .guild_id = GUILD, .name = "Managers", .position = 5,
     .permissions = CCORD_PERM_MANAGE_ROLES | CCORD_PERM_BAN_MEMBERS | CCORD_PERM_VIEW_AUDIT_LOG | CCORD_PERM_KICK_MEMBERS},
    {.id = MODS, .guild_id = GUILD, .name = "Mods", .position = 3, .permissions = CCORD_PERM_KICK_MEMBERS},
    {.id = HELPERS, .guild_id = GUILD, .name = "Helpers", .position = 1, .permissions = CCORD_PERM_ADD_REACTIONS},
    {.id = BOTROLE, .guild_id = GUILD, .name = "Bot", .position = 2, .managed = true},
  };
  ccord_id mine[1] = {MANAGERS};
  ccord_id adas[1] = {HELPERS};
  ccord_member_roles mroles[2] = {
    {.guild_id = GUILD, .user_id = SELF, .role_ids = mine, .count = 1},
    {.guild_id = GUILD, .user_id = ADA, .role_ids = adas, .count = 1},
  };
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  static ccord_transport_vtable vt;
  vt = fake_vt;
  vt.admin_action = perm_admin_action;
  f->base.vt = &vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap, .roles = roles, .role_count = 5,
                                    .member_roles = mroles, .member_role_count = 2};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_permissions_exact(store, GUILD, SELF));
  CT_CHECK(!ccord_store_permissions_exact(store, GUILD2, SELF));   // no roles known there

  // ---- the answers Discord gives land in the store ----
  ccord_ban bans[1] = {{.user_id = 70, .name = "troll", .reason = "spam"}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_BAN_LIST, .guild_id = GUILD, .bans = bans, .ban_count = 1};
  ccord_invite inv[1] = {{.code = "abc", .channel_id = CHAN, .channel_name = "general", .inviter_id = SELF, .inviter_name = "You"}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_INVITE_LIST, .guild_id = GUILD, .invites = inv, .invite_count = 1};
  ccord_audit_entry log[1] = {{.id = 5, .user_id = SELF, .user_name = "You", .action = 22, .target_id = 70,
                               .target_name = "troll", .reason = "", .detail = ""}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_AUDIT_LOG, .guild_id = GUILD, .audit = log, .audit_count = 1};
  CT_CHECK(ccord_session_pump(s) == 3);
  size_t n = 0;
  CT_CHECK(ccord_store_bans_loaded(store, GUILD) && ccord_store_bans(store, GUILD, &n) && n == 1);
  CT_CHECK(ccord_store_invites(store, GUILD, &n) && n == 1 && ccord_store_audit(store, GUILD, &n) && n == 1);

  ccord_invite more[1] = {{.code = "def", .channel_id = CHAN, .channel_name = "general", .inviter_id = ADA, .inviter_name = "Ada"}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_INVITE_UPSERT, .guild_id = GUILD, .invites = more, .invite_count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_INVITE_DELETE, .guild_id = GUILD, .invite_code = "abc"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_BAN_REMOVE, .guild_id = GUILD, .user_id = 70};
  CT_CHECK(ccord_session_pump(s) == 3);
  const ccord_invite *iv = ccord_store_invites(store, GUILD, &n);
  CT_CHECK(n == 1 && iv && strcmp(iv[0].code, "def") == 0);
  CT_CHECK(ccord_store_bans_loaded(store, GUILD) && ccord_store_bans(store, GUILD, &n) == nullptr && n == 0);
  // Answers for a server we are not in are refused quietly, not stored.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_BAN_LIST, .guild_id = 999, .bans = bans, .ban_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1 && !ccord_store_bans_loaded(store, 999));

  // A role given or taken: applied to what is known; a member whose roles are unknown stays unknown.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MEMBER_ROLE_CHANGE, .guild_id = GUILD, .user_id = ADA, .role_id = MODS,
                                    .role_added = true};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MEMBER_ROLE_CHANGE, .guild_id = GUILD, .user_id = OWNER, .role_id = MODS,
                                    .role_added = true};
  CT_CHECK(ccord_session_pump(s) == 2);
  const ccord_id *ids = nullptr;
  size_t nids = 0;
  CT_CHECK(ccord_store_member_roles(store, GUILD, ADA, &ids, &nids) && nids == 2 && ids[0] == HELPERS && ids[1] == MODS);
  CT_CHECK(!ccord_store_member_roles(store, GUILD, OWNER, &ids, &nids));
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MEMBER_ROLE_CHANGE, .guild_id = GUILD, .user_id = ADA, .role_id = HELPERS,
                                    .role_added = false};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_member_roles(store, GUILD, ADA, &ids, &nids) && nids == 1 && ids[0] == MODS);

  // A reorder (or a new role pushing the others up) delivers several roles at once; every one is applied.
  ccord_role moved[2] = {
    {.id = MODS, .guild_id = GUILD, .name = "Mods", .position = 1, .permissions = CCORD_PERM_KICK_MEMBERS},
    {.id = HELPERS, .guild_id = GUILD, .name = "Helpers", .position = 3, .permissions = CCORD_PERM_ADD_REACTIONS},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = GUILD, .roles = moved, .role_count = 2,
                                    .role_id = MODS};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_role(store, GUILD, MODS)->position == 1 && ccord_store_role(store, GUILD, HELPERS)->position == 3);
  moved[0].position = 3;   // put things back for the checks below
  moved[1].position = 1;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = GUILD, .roles = moved, .role_count = 2,
                                    .role_id = MODS};
  CT_CHECK(ccord_session_pump(s) == 1);

  // ---- the pre-check: what can never work stops here ----
  size_t calls = g_perm_admin_calls;
  ccord_admin_request r;
  ccord_admin_role_edit e = CCORD_ADMIN_ROLE_EDIT_INIT;
  e.color = 5;
  // Own top role and anything above it: no (the role hierarchy).
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, MANAGERS, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION && strstr(err.message, "below your highest role") != nullptr);
  CT_CHECK(ccord_admin_delete_role(&r, GUILD, MANAGERS, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(g_perm_admin_calls == calls);
  // A role below it: goes out.
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, MODS, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  CT_CHECK(ccord_admin_delete_role(&r, GUILD, HELPERS, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  // Handing out permissions you do not have yourself: no.
  ccord_admin_role_edit grant = CCORD_ADMIN_ROLE_EDIT_INIT;
  grant.permissions = (int64_t)(CCORD_PERM_KICK_MEMBERS | CCORD_PERM_MANAGE_GUILD);
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, MODS, &grant, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION && strstr(err.message, "permissions you have yourself") != nullptr);
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &grant, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);
  grant.permissions = (int64_t)CCORD_PERM_KICK_MEMBERS;   // one you hold: fine
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, MODS, &grant, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  CT_CHECK(ccord_admin_create_role(&r, GUILD, &grant, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  CT_CHECK(ccord_admin_create_role(&r, GUILD, nullptr, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  // Dropping a permission someone else's role has and you lack is fine: only *giving* is limited.
  ccord_admin_role_edit drop = CCORD_ADMIN_ROLE_EDIT_INIT;
  drop.permissions = 0;
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, MODS, &drop, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  // Member roles: a role above yours or a bot's role cannot be handed out.
  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, ADA, MANAGERS, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, ADA, BOTROLE, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION && strstr(err.message, "bot or integration") != nullptr);
  CT_CHECK(ccord_admin_add_member_role(&r, GUILD, ADA, HELPERS, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  CT_CHECK(ccord_admin_remove_member_role(&r, GUILD, ADA, MODS, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  CT_CHECK(ccord_admin_remove_member_role(&r, GUILD, ADA, MANAGERS, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);
  // A reorder is refused as soon as it touches a role that is not below yours.
  ccord_id ro[2] = {MODS, MANAGERS};
  int32_t rp[2] = {2, 3};
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, ro, rp, 2, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_reorder_roles(&r, GUILD, ro, rp, 1, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  // Reads and the lists never need a local verdict.
  CT_CHECK(ccord_admin_list_bans(&r, GUILD, &err) == CCORD_OK && ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_list_audit_log(&r, GUILD, CCORD_ID_NONE, 0, &err) == CCORD_OK && ccord_session_admin(s, &r, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_unban(&r, GUILD, 70, nullptr, &err) == CCORD_OK && ccord_session_admin(s, &r, &err) == CCORD_OK);
  calls += 3;
  CT_CHECK(g_perm_admin_calls == calls);
  // Where our picture of the roles is incomplete, everything is left to Discord (it has the final word).
  CT_CHECK(ccord_admin_edit_role(&r, GUILD2, MANAGERS, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  // The same when only our own roles are missing: with a role table but no idea what we hold, nothing is ruled out
  // locally (every role would otherwise look like it outranks us).
  ccord_role table2[2] = {
    {.id = GUILD2, .guild_id = GUILD2, .name = "@everyone", .permissions = base},
    {.id = 300, .guild_id = GUILD2, .name = "Other", .position = 2},
  };
  CT_CHECK(ccord_store_set_guild_roles(store, GUILD2, table2, 2, &err) == CCORD_OK);
  CT_CHECK(!ccord_store_permissions_exact(store, GUILD2, SELF));
  CT_CHECK(ccord_admin_edit_role(&r, GUILD2, 300, &e, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  // A bit a role already has may stay on it even if we lack it ourselves; only newly given bits are checked.
  ccord_role helpers_plus = {.id = HELPERS, .guild_id = GUILD, .name = "Helpers", .position = 1,
                             .permissions = CCORD_PERM_ADD_REACTIONS | CCORD_PERM_MANAGE_GUILD};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ROLE_UPSERT, .guild_id = GUILD, .roles = &helpers_plus, .role_count = 1,
                                    .role_id = HELPERS};
  CT_CHECK(ccord_session_pump(s) == 1);
  ccord_admin_role_edit keep = CCORD_ADMIN_ROLE_EDIT_INIT;
  keep.permissions = (int64_t)(CCORD_PERM_ADD_REACTIONS | CCORD_PERM_MANAGE_GUILD | CCORD_PERM_KICK_MEMBERS);
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, HELPERS, &keep, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_OK && g_perm_admin_calls == ++calls);
  keep.permissions = (int64_t)(CCORD_PERM_ADD_REACTIONS | CCORD_PERM_MANAGE_GUILD | CCORD_PERM_ADMINISTRATOR);
  CT_CHECK(ccord_admin_edit_role(&r, GUILD, HELPERS, &keep, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_PERMISSION && g_perm_admin_calls == calls);
  // A server we are not in has nothing to ask about.
  CT_CHECK(ccord_admin_list_bans(&r, 999, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &r, &err) == CCORD_ERR_NOT_FOUND && g_perm_admin_calls == calls);

  // Leaving the server forgets the lists too.
  CT_CHECK(ccord_store_audit_loaded(store, GUILD));
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_DELETE, .guild_id = GUILD};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(!ccord_store_audit_loaded(store, GUILD) && !ccord_store_invites_loaded(store, GUILD));
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Notification settings: rows arrive with the snapshot and as live updates; they decide which new messages queue a
// desktop notification / sound. Where the user never chose, behaviour is what it always was (direct messages and
// @mentions only).

static size_t g_voice_join_calls;
static ccord_status counting_join_voice(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  (void)t; (void)guild_id; (void)channel_id;
  g_voice_join_calls++;
  ccord_error_clear(err);
  return CCORD_OK;
}

static void test_session_voice_join_permissions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;

  enum {
    SELF = 1, PEER = 2, OWNER = 9,
    G_EXACT = 10, G_UNKNOWN = 11,
    VC_OPEN = 21, VC_LOCKED = 22, VC_HIDDEN = 23, VC_FULL = 24, STG_LOCKED = 25, VC_UNKNOWN = 31,
    R_MEMBER = 200, R_MOVER = 201
  };
  ccord_user users[3] = {
    {.id = SELF, .name = "You", .status_text = ""},
    {.id = PEER, .name = "Peer", .status_text = ""},
    {.id = OWNER, .name = "Owner", .status_text = ""},
  };
  ccord_guild guilds[2] = {
    {.id = G_EXACT, .name = "Exact", .owner_id = OWNER},
    {.id = G_UNKNOWN, .name = "Unknown", .owner_id = OWNER},
  };
  ccord_channel chans[6] = {
    {.id = VC_OPEN, .guild_id = G_EXACT, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "Open", .topic = ""},
    {.id = VC_LOCKED, .guild_id = G_EXACT, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "Locked", .topic = ""},
    {.id = VC_HIDDEN, .guild_id = G_EXACT, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "Hidden", .topic = ""},
    {.id = VC_FULL, .guild_id = G_EXACT, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "Full", .topic = "", .user_limit = 1},
    {.id = STG_LOCKED, .guild_id = G_EXACT, .kind = CCORD_CHANNEL_STAGE, .discord_type = 13, .name = "Stage", .topic = ""},
    {.id = VC_UNKNOWN, .guild_id = G_UNKNOWN, .kind = CCORD_CHANNEL_VOICE, .discord_type = 2, .name = "Uncached", .topic = ""},
  };
  ccord_member members[2] = {
    {.guild_id = G_EXACT, .user_id = SELF, .role_name = "Member"},
    {.guild_id = G_EXACT, .user_id = PEER, .role_name = "Member"},
  };
  ccord_voice_state vstates[1] = {
    {.guild_id = G_EXACT, .channel_id = VC_FULL, .user_id = PEER},
  };
  ccord_snapshot snap = {
    .self_id = SELF, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 2,
    .channels = chans, .channel_count = 6, .members = members, .member_count = 2,
    .voice_states = vstates, .voice_state_count = 1,
  };
  ccord_role roles[3] = {
    {.id = G_EXACT, .guild_id = G_EXACT, .name = "@everyone", .permissions = CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CONNECT},
    {.id = R_MEMBER, .guild_id = G_EXACT, .name = "Member", .position = 1, .permissions = 0},
    {.id = R_MOVER, .guild_id = G_EXACT, .name = "Mover", .position = 2, .permissions = CCORD_PERM_MOVE_MEMBERS},
  };
  ccord_id self_roles[1] = {R_MEMBER};
  ccord_member_roles mroles[1] = {{.guild_id = G_EXACT, .user_id = SELF, .role_ids = self_roles, .count = 1}};
  ccord_overwrite deny_conn[1] = {{.id = G_EXACT, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_CONNECT}};
  ccord_overwrite deny_view[1] = {{.id = G_EXACT, .kind = CCORD_OVERWRITE_ROLE, .deny = CCORD_PERM_VIEW_CHANNEL}};
  ccord_channel_overwrites ow[3] = {
    {.channel_id = VC_LOCKED, .items = deny_conn, .count = 1},
    {.channel_id = VC_HIDDEN, .items = deny_view, .count = 1},
    {.channel_id = STG_LOCKED, .items = deny_conn, .count = 1},
  };

  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  static ccord_transport_vtable vt;
  vt = fake_vt;
  vt.join_voice = counting_join_voice;
  f->base.vt = &vt;
  g_voice_join_calls = 0;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap,
                                    .roles = roles, .role_count = 3, .member_roles = mroles, .member_role_count = 1,
                                    .overwrites = ow, .overwrite_count = 3};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_permissions_exact(store, G_EXACT, SELF));
  CT_CHECK(!ccord_store_permissions_exact(store, G_UNKNOWN, SELF));

  // Missing CONNECT (voice & stage) and missing VIEW_CHANNEL are blocked before hitting transport.
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_LOCKED, &err) == CCORD_ERR_PERMISSION && g_voice_join_calls == 0);
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, STG_LOCKED, &err) == CCORD_ERR_PERMISSION && g_voice_join_calls == 0);
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_HIDDEN, &err) == CCORD_ERR_PERMISSION && g_voice_join_calls == 0);

  // Full channel (user_limit == 1 occupied by PEER) is blocked without MOVE_MEMBERS.
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_FULL, &err) == CCORD_ERR_PERMISSION && g_voice_join_calls == 0);
  CT_CHECK(strstr(err.message, "full") != nullptr);

  // Open channel succeeds; unknown-permissions guild fails open.
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_OPEN, &err) == CCORD_OK && g_voice_join_calls == 1);
  CT_CHECK(ccord_session_join_voice(s, G_UNKNOWN, VC_UNKNOWN, &err) == CCORD_OK && g_voice_join_calls == 2);

  // Granting MOVE_MEMBERS allows joining the full channel.
  ccord_id mover_roles[2] = {R_MEMBER, R_MOVER};
  CT_CHECK(ccord_store_set_member_roles(store, G_EXACT, SELF, mover_roles, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_FULL, &err) == CCORD_OK && g_voice_join_calls == 3);
  CT_CHECK(ccord_store_set_member_roles(store, G_EXACT, SELF, self_roles, 1, &err) == CCORD_OK);

  // Timed-out member cannot join even an open voice channel.
  ccord_member timed_out = {.guild_id = G_EXACT, .user_id = SELF, .role_name = "Member",
                            .timeout_until_ms = 4102444800000LL};   // year 2100
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = G_EXACT,
                                    .guild_members = &timed_out, .guild_member_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_OPEN, &err) == CCORD_ERR_PERMISSION && g_voice_join_calls == 3);
  CT_CHECK(strstr(err.message, "timed out") != nullptr);
  ccord_member cleared = {.guild_id = G_EXACT, .user_id = SELF, .role_name = "Member", .timeout_until_ms = 0};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = G_EXACT,
                                    .guild_members = &cleared, .guild_member_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_OPEN, &err) == CCORD_OK && g_voice_join_calls == 4);

  // Pending membership screening blocks joining voice.
  ccord_member pending = {.guild_id = G_EXACT, .user_id = SELF, .role_name = "Member", .pending = true};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = G_EXACT,
                                    .guild_members = &pending, .guild_member_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_join_voice(s, G_EXACT, VC_OPEN, &err) == CCORD_ERR_PERMISSION && g_voice_join_calls == 4);
  CT_CHECK(strstr(err.message, "screening") != nullptr);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}


int main(void)
{
  test_flow();
  test_create_rejects_null();
  test_live_event_pump();
  test_session_features();
  test_snapshot_initial_history_and_fallback();
  test_history_error_retry_and_author_upserts();
  test_pagination_dedup_history_failed_and_cross_guild_select();
  test_forum_channel_session_flow();
  test_action_failures();
  test_action_failures_are_bounded();
  test_session_admin();
  test_session_permission_data();
  test_session_server_settings();
  test_session_voice_join_permissions();
  return CT_RESULT("test_session_core");
}
