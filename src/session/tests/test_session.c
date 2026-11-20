// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/model/rich.h>
#include <ccord/net/mock_transport.h>
#include <ccord/session/session.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/listsize.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>
#include <ccord/store/unread.h>

#include "ccord_test.h"

static uint64_t g_now = 1000;

static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

static size_t pump_all(ccord_session *s)
{
  size_t total = 0;
  for (int i = 0; i < 8; i++) {
    size_t n = ccord_session_pump(s);
    if (n == 0) break;
    total += n;
  }
  return total;
}

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

typedef struct fake_transport {
  ccord_transport base;
  ccord_net_event ev[64];
  size_t n, i;
  ccord_channel channel;
  ccord_id last_requested_history_channel;
  uint32_t history_request_count;
  uint32_t history_before_request_count;
  ccord_id last_selected_guild;
  ccord_id last_selected_channel_guild;
  ccord_id last_selected_channel_id;
  uint32_t select_channel_count;
  uint32_t profile_request_count;
  uint32_t members_request_count, last_member_chunks;   // request_members
  ccord_id last_member_guild, last_member_channel;
  bool members_fail;
  // M0-3 send recording (legacy and nonce hooks)
  uint32_t send_count, nonce_send_count;
  ccord_id last_send_channel;
  char last_send_text[CCORD_SEND_MAX_BYTES + 1];
  char last_send_nonce[32];
  bool nonce_send_fail;          // the nonce hook refuses synchronously
} fake_transport;

static ccord_status fake_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}

static void fake_disconnect(ccord_transport *t) { (void)t; }

static ccord_status fake_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->last_requested_history_channel = channel_id;
  f->history_request_count++;
  (void)limit;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_send(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->send_count++;
  f->last_send_channel = channel_id;
  memcpy(f->last_send_text, utf8, len);
  f->last_send_text[len] = '\0';
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_send_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                    const char *nonce, size_t nonce_len, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->nonce_send_count++;
  f->last_send_channel = channel_id;
  memcpy(f->last_send_text, utf8, len);
  f->last_send_text[len] = '\0';
  memcpy(f->last_send_nonce, nonce, nonce_len);
  f->last_send_nonce[nonce_len] = '\0';
  if (f->nonce_send_fail) return ccord_error_set(err, CCORD_ERR_IO, "fake: send refused");
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool fake_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_transport *f = (fake_transport *)t;
  if (f->i >= f->n) return false;
  *out = f->ev[f->i++];
  return true;
}

static void fake_destroy(ccord_transport *t)
{
  free(t);
}

static ccord_status fake_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id, uint32_t limit,
                                        ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  (void)channel_id;
  (void)before_id;
  (void)limit;
  f->history_before_request_count++;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_select_guild(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->last_selected_guild = guild_id;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_select_channel(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->last_selected_guild = guild_id;
  f->last_selected_channel_guild = guild_id;
  f->last_selected_channel_id = channel_id;
  f->select_channel_count++;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  (void)user_id;
  f->profile_request_count++;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_request_members(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                         ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->members_request_count++;
  f->last_member_guild = guild_id;
  f->last_member_channel = channel_id;
  f->last_member_chunks = chunks;
  if (f->members_fail) return ccord_error_set(err, CCORD_ERR_RANGE, "fake: outbound limit");
  ccord_error_clear(err);
  return CCORD_OK;
}

static const ccord_transport_vtable fake_vt = {
  .name = "fake",
  .connect = fake_connect,
  .disconnect = fake_disconnect,
  .request_history = fake_history,
  .request_history_before = fake_history_before,
  .select_guild = fake_select_guild,
  .select_channel = fake_select_channel,
  .request_profile = fake_request_profile,
  .request_members = fake_request_members,
  .send_message = fake_send,
  .poll = fake_poll,
  .destroy = fake_destroy,
};

// M0-3: same fake, plus the nonce hook (recorded, optionally refused synchronously).
static const ccord_transport_vtable fake_nonce_vt = {
  .name = "fake-nonce",
  .connect = fake_connect,
  .disconnect = fake_disconnect,
  .request_history = fake_history,
  .request_history_before = fake_history_before,
  .select_guild = fake_select_guild,
  .select_channel = fake_select_channel,
  .request_profile = fake_request_profile,
  .request_members = fake_request_members,
  .send_message = fake_send,
  .send_message_with_nonce = fake_send_nonce,
  .poll = fake_poll,
  .destroy = fake_destroy,
};

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
static size_t drain_notifications(ccord_session *s, ccord_id *channels, size_t cap)
{
  size_t n = 0;
  ccord_notification note;
  while (ccord_session_pop_notification(s, &note)) {
    if (n < cap) channels[n] = note.channel_id;
    n++;
  }
  return n;
}

static void test_session_notify_settings(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, GUILD = 10, LOBBY = 20, CAT = 21, CHILD = 22, LOOSE = 23, MUTED = 24, DMCH = 40 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[6] = {
    {.id = LOBBY, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0},
    {.id = CAT, .guild_id = GUILD, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = "", .position = 1},
    {.id = CHILD, .guild_id = GUILD, .parent_id = CAT, .kind = CCORD_CHANNEL_TEXT, .name = "child", .topic = "", .position = 2},
    {.id = LOOSE, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 3},
    {.id = MUTED, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "muted", .topic = "", .position = 4},
    {.id = DMCH, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "", .user_id = ANN},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 6};
  ccord_notify_scope rows[3] = {
    {.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT},
    {.guild_id = GUILD, .channel_id = MUTED, .level = CCORD_NOTIFY_INHERIT, .muted = true},
    {.guild_id = CCORD_DM_GUILD_ID, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT},
  };
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap, .notify = rows, .notify_count = 3};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_notify_count(store) == 3);
  CT_CHECK(ccord_store_channel_muted(store, MUTED, 0) && !ccord_store_channel_muted(store, LOOSE, 0));
  CT_CHECK(ccord_store_selected_channel(store) == LOBBY);

  ccord_message m1 = {.id = 101, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 1, .content = "hello"};
  ccord_message m2 = {.id = 102, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 2, .content = "hey <@1>"};
  ccord_message m3 = {.id = 103, .channel_id = MUTED, .author_id = ANN, .timestamp_ms = 3, .content = "hey <@1> in muted"};
  ccord_message m4 = {.id = 104, .channel_id = DMCH, .author_id = ANN, .timestamp_ms = 4, .content = "psst"};
  ccord_message m5 = {.id = 105, .channel_id = CHILD, .author_id = ANN, .timestamp_ms = 5, .content = "hey <@1> in child"};
  ccord_id got[8];
  #define SEND(msg) do { f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = (msg).channel_id == DMCH ? CCORD_DM_GUILD_ID : GUILD, \
                                                           .channel_id = (msg).channel_id, .messages = &(msg), .message_count = 1}; } while (0)

  // Nothing chosen: a plain message is quiet; a mention and a direct message notify; a muted channel is silent
  // even for a mention (a mute is a mute), though the badge counts keep running.
  SEND(m1); SEND(m2); SEND(m3); SEND(m4);
  CT_CHECK(ccord_session_pump(s) == 4);
  size_t n = drain_notifications(s, got, 8);
  CT_CHECK(n == 2 && got[0] == LOOSE && got[1] == DMCH);
  const ccord_channel *c = ccord_store_channel(store, MUTED);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);   // still counted: unmuting shows the truth

  // The user sets "Nothing" on the category: its channel goes quiet for mentions too, others are unaffected.
  ccord_notify_scope hq[3] = {
    {.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT},
    {.guild_id = GUILD, .channel_id = CAT, .level = CCORD_NOTIFY_NOTHING},
    {.guild_id = GUILD, .channel_id = MUTED, .level = CCORD_NOTIFY_INHERIT, .muted = true},
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = hq, .notify_count = 3};
  SEND(m5);
  ccord_message m6 = {.id = 106, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 6, .content = "again <@1>"};
  SEND(m6);
  CT_CHECK(ccord_session_pump(s) == 3);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == LOOSE);

  // "All Messages" chosen on the server notifies for every message (that is what choosing it means).
  ccord_notify_scope all[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = all, .notify_count = 1};
  ccord_message m7 = {.id = 107, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 7, .content = "just chatting"};
  ccord_message m8 = {.id = 108, .channel_id = MUTED, .author_id = ANN, .timestamp_ms = 8, .content = "just chatting"};
  SEND(m7); SEND(m8);
  CT_CHECK(ccord_session_pump(s) == 3);
  n = drain_notifications(s, got, 8);
  // The new row set replaced the old one: #muted's mute is gone with it, so both notify under "All Messages".
  CT_CHECK(n == 2 && got[0] == LOOSE && got[1] == MUTED);
  CT_CHECK(!ccord_store_channel_muted(store, MUTED, 0));

  // A timed mute that has ended no longer mutes: the clock decides, not a stored flag.
  ccord_notify_scope timed[2] = {
    {.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL},
    {.guild_id = GUILD, .channel_id = LOOSE, .level = CCORD_NOTIFY_INHERIT, .muted = true, .mute_until_ms = 1000},   // 1970
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = timed, .notify_count = 2};
  ccord_message m10 = {.id = 110, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = 10, .content = "after the mute"};
  SEND(m10);
  CT_CHECK(ccord_session_pump(s) == 2);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == LOOSE);

  // Zero rows put a server back to defaults; rows for a server we are not in, or nonsense, are refused quietly.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = nullptr, .notify_count = 0};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_notify_count(store) == 1);                    // only the direct-message row is left
  ccord_notify_scope stray[1] = {{.guild_id = 999, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL, .muted = true}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = stray, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_notify_count(store) == 1);                    // a row for another server under this one is rejected
  #undef SEND
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// When the user has not chosen an override level on a guild or channel, the server's own
// default_message_notifications setting (0 = All Messages, 1 = Only @mentions) governs plain messages.
static void test_session_guild_default_notifications(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, G_ALL = 10, G_MENT = 11, CH_LOBBY = 20, CH_ALL = 21, CH_MENT = 22 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[2] = {
    {.id = G_ALL, .name = "AllDefault", .has_guild_settings = true, .default_message_notifications = 0},
    {.id = G_MENT, .name = "MentionsDefault", .has_guild_settings = true, .default_message_notifications = 1},
  };
  ccord_channel chans[3] = {
    {.id = CH_LOBBY, .guild_id = G_ALL, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0},
    {.id = CH_ALL, .guild_id = G_ALL, .kind = CCORD_CHANNEL_TEXT, .name = "chat", .topic = "", .position = 1},
    {.id = CH_MENT, .guild_id = G_MENT, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "", .position = 0},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 2,
                         .channels = chans, .channel_count = 3};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) {
    ccord_store_destroy(store);
    ccord_bus_destroy(bus);
    return;
  }
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) {
    free(f);
    ccord_store_destroy(store);
    ccord_bus_destroy(bus);
    return;
  }
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_selected_channel(store) == CH_LOBBY);

  ccord_id got[8];
  #define SEND_G(gid, msg) do { f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = (gid), \
                                                                  .channel_id = (msg).channel_id, .messages = &(msg), .message_count = 1}; } while (0)

  // No user override rows: G_ALL notifies on plain messages, G_MENT only notifies on mentions.
  ccord_message m1 = {.id = 101, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 1, .content = "plain in all"};
  ccord_message m2 = {.id = 102, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 2, .content = "plain in ment"};
  ccord_message m3 = {.id = 103, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 3, .content = "ping <@1>"};
  SEND_G(G_ALL, m1);
  SEND_G(G_MENT, m2);
  SEND_G(G_MENT, m3);
  CT_CHECK(ccord_session_pump(s) == 3);
  size_t n = drain_notifications(s, got, 8);
  CT_CHECK(n == 2 && got[0] == CH_ALL && got[1] == CH_MENT);

  // Explicit INHERIT row still defers to the guild's default_message_notifications.
  ccord_notify_scope inh[1] = {{.guild_id = G_ALL, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = G_ALL, .notify = inh, .notify_count = 1};
  ccord_message m4 = {.id = 104, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 4, .content = "still all"};
  SEND_G(G_ALL, m4);
  CT_CHECK(ccord_session_pump(s) == 2);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == CH_ALL);

  // Explicit user override (Mentions on G_ALL, All Messages on G_MENT) beats the guild default.
  ccord_notify_scope ov_ment[1] = {{.guild_id = G_ALL, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_MENTIONS}};
  ccord_notify_scope ov_all[1] = {{.guild_id = G_MENT, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = G_ALL, .notify = ov_ment, .notify_count = 1};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = G_MENT, .notify = ov_all, .notify_count = 1};
  ccord_message m5 = {.id = 105, .channel_id = CH_ALL, .author_id = ANN, .timestamp_ms = 5, .content = "now quiet"};
  ccord_message m6 = {.id = 106, .channel_id = CH_MENT, .author_id = ANN, .timestamp_ms = 6, .content = "now noisy"};
  SEND_G(G_ALL, m5);
  SEND_G(G_MENT, m6);
  CT_CHECK(ccord_session_pump(s) == 4);
  n = drain_notifications(s, got, 8);
  CT_CHECK(n == 1 && got[0] == CH_MENT);

  #undef SEND_G
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Mentions are what Discord says they are: the user's id in the mention list (a reply that pings is one), @everyone / @here,
// and a role the user holds. The server's "suppress" switches drop the broad ones, for the notification and the badge alike.
static void test_session_structured_mentions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, BOB = 3, GUILD = 10, LOBBY = 20, LOOSE = 23, DMCH = 40 };
  ccord_user users[3] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""},
                         {.id = BOB, .name = "Bob", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[3] = {
    {.id = LOBBY, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0},
    {.id = LOOSE, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 1},
    {.id = DMCH, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "", .user_id = ANN},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 3};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);

  ccord_id got[8];
  ccord_id ids = 200;
  #define SEND_RICH(rich_ptr, text) do { \
    ++ids; \
    ccord_message mm = {.id = ids, .channel_id = LOOSE, .author_id = ANN, .timestamp_ms = (int64_t)ids, .content = (text), .rich = (rich_ptr)}; \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = LOOSE, .messages = &mm, .message_count = 1}; \
    CT_CHECK(ccord_session_pump(s) == 1); \
  } while (0)

  // A plain message is quiet.
  SEND_RICH(nullptr, "hello");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  const ccord_channel *c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 0);

  // A direct message counts as a mention of the user whatever it says.
  ++ids;
  ccord_message dm = {.id = ids, .channel_id = DMCH, .author_id = ANN, .timestamp_ms = (int64_t)ids, .content = "psst"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = CCORD_DM_GUILD_ID, .channel_id = DMCH,
                                    .messages = &dm, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  const ccord_channel *dc = ccord_store_channel(store, DMCH);
  CT_CHECK(dc && dc->unread_count == 1 && dc->mention_count == 1);

  // The user's id in the mention list notifies, with no tag in the text (a reply that pings looks like this).
  ccord_id me[1] = {SELF};
  ccord_message_rich ping = {.present = CCORD_RICH_ALL, .mention_users = me, .mention_user_count = 1};
  SEND_RICH(&ping, "no tag in this text");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 1);

  // Somebody else being mentioned is not a mention of the user.
  ccord_id other[2] = {BOB, ANN};
  ccord_message_rich not_me = {.present = CCORD_RICH_ALL, .mention_users = other, .mention_user_count = 2};
  SEND_RICH(&not_me, "hey <@3>");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 1);

  // @everyone / @here notifies and counts, unless the server suppresses it.
  ccord_message_rich all = {.present = CCORD_RICH_ALL, .mention_everyone = true};
  SEND_RICH(&all, "@everyone");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 2);

  // A role ping: only when the user is known to hold the role.
  ccord_id r500[1] = {500};
  ccord_message_rich role = {.present = CCORD_RICH_ALL, .mention_roles = r500, .mention_role_count = 1};
  SEND_RICH(&role, "@mods");
  CT_CHECK(drain_notifications(s, got, 8) == 0);   // the user's roles are not known yet: nothing is claimed
  ccord_id mine[1] = {500};
  CT_CHECK(ccord_store_set_member_roles(store, GUILD, SELF, mine, 1, &err) == CCORD_OK);
  SEND_RICH(&role, "@mods");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 3);
  ccord_id r501[1] = {501};
  ccord_message_rich other_role = {.present = CCORD_RICH_ALL, .mention_roles = r501, .mention_role_count = 1};
  SEND_RICH(&other_role, "@someone-else's role");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 3);

  // Suppress @everyone: the ping neither notifies nor counts; role pings still do.
  ccord_notify_scope sup_every[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT, .suppress_everyone = true}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = sup_every, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  SEND_RICH(&all, "@everyone again");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 3);
  SEND_RICH(&role, "@mods again");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 4);

  // Suppress roles too: the role ping goes quiet; a direct mention still gets through, and so does everyone if allowed again.
  ccord_notify_scope sup_role[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_INHERIT, .suppress_roles = true}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = sup_role, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  SEND_RICH(&role, "@mods once more");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 4);
  SEND_RICH(&ping, "direct");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  SEND_RICH(&all, "@everyone is allowed here");
  CT_CHECK(drain_notifications(s, got, 8) == 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 6);

  // Under "Nothing" even a direct mention is silent; the badge keeps counting.
  ccord_notify_scope nothing[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_NOTHING}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = nothing, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  SEND_RICH(&ping, "direct but silent");
  CT_CHECK(drain_notifications(s, got, 8) == 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->mention_count == 7);

  // Under "All Messages" every message notifies, and the notification says whether it was a mention.
  ccord_notify_scope every[1] = {{.guild_id = GUILD, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL}};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_NOTIFY_SETTINGS, .guild_id = GUILD, .notify = every, .notify_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  ccord_notification note;
  SEND_RICH(nullptr, "plain chat");
  CT_CHECK(ccord_session_pop_notification(s, &note) && !note.is_mention && !note.is_dm && note.channel_id == LOOSE &&
           note.author_id == ANN && note.message_id == ids);
  CT_CHECK(!ccord_session_pop_notification(s, &note));
  SEND_RICH(&ping, "direct");
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.is_mention);
  SEND_RICH(&all, "@everyone");
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.is_mention);
  SEND_RICH(&role, "@mods");
  CT_CHECK(ccord_session_pop_notification(s, &note) && note.is_mention);
  SEND_RICH(&other_role, "somebody else's role");
  CT_CHECK(ccord_session_pop_notification(s, &note) && !note.is_mention);
  // Whatever the level, the user's own messages (sent from another device, or the echo of a send) never notify.
  ++ids;
  ccord_message own = {.id = ids, .channel_id = LOOSE, .author_id = SELF, .timestamp_ms = (int64_t)ids, .content = "my own words",
                       .rich = &ping};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = LOOSE, .messages = &own,
                                    .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(!ccord_session_pop_notification(s, &note));
  #undef SEND_RICH
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static struct {
  ccord_id ch[256], msg[256];
  size_t n, fail_at;   // acknowledgement number `fail_at` (0 based) and every later one is refused
} g_ack;

static ccord_status mark_read_ack(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  (void)t;
  if (g_ack.n >= g_ack.fail_at || g_ack.n >= 256) return ccord_error_set(err, CCORD_ERR_IO, "ack refused");
  g_ack.ch[g_ack.n] = channel_id;
  g_ack.msg[g_ack.n] = message_id;
  g_ack.n++;
  ccord_error_clear(err);
  return CCORD_OK;
}

// "Mark As Read" sends one acknowledgement per channel that has something unread, at most CCORD_MARK_READ_MAX at a
// time, up to the newest message, and clears the counts of the ones that went out.
static void test_session_mark_read(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, HQ = 10, OTHER = 11, CAT = 20, KID1 = 21, KID2 = 22, LOOSE = 23, QUIET = 24, FAR = 25, DMCH = 40, BULK = 100, NBULK = 60 };
  ccord_user users[1] = {{.id = SELF, .name = "You", .status_text = ""}};
  ccord_guild guilds[2] = {{.id = HQ, .name = "HQ"}, {.id = OTHER, .name = "Other"}};
  ccord_channel chans[7 + NBULK] = {
    {.id = CAT, .guild_id = HQ, .kind = CCORD_CHANNEL_CATEGORY, .name = "TEXT", .topic = "", .position = 0},
    {.id = KID1, .guild_id = HQ, .parent_id = CAT, .kind = CCORD_CHANNEL_TEXT, .name = "kid1", .topic = "", .position = 1, .last_message_id = 5001},
    {.id = KID2, .guild_id = HQ, .parent_id = CAT, .kind = CCORD_CHANNEL_TEXT, .name = "kid2", .topic = "", .position = 2, .last_message_id = 5002},
    {.id = LOOSE, .guild_id = HQ, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 3, .last_message_id = 5003},
    {.id = QUIET, .guild_id = HQ, .kind = CCORD_CHANNEL_TEXT, .name = "quiet", .topic = "", .position = 4, .last_message_id = 5004},
    {.id = FAR, .guild_id = OTHER, .kind = CCORD_CHANNEL_TEXT, .name = "far", .topic = "", .position = 0, .last_message_id = 5005},
    {.id = DMCH, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = "Ann", .topic = "", .last_message_id = 5040},
  };
  for (ccord_id i = 0; i < NBULK; i++) {
    chans[7 + i] = (ccord_channel){.id = BULK + i, .guild_id = HQ, .kind = CCORD_CHANNEL_TEXT, .name = "bulk", .topic = "",
                                   .position = (int32_t)(10 + i), .last_message_id = 7000 + i};
  }
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 1, .guilds = guilds, .guild_count = 2,
                         .channels = chans, .channel_count = 7 + NBULK};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  static ccord_transport_vtable vt;
  vt = fake_vt;
  vt.ack_message = mark_read_ack;
  f->base.vt = &vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  memset(&g_ack, 0, sizeof g_ack);
  g_ack.fail_at = SIZE_MAX;
  size_t acked = 99;
  #define COUNTS(id, u, m) CT_CHECK(ccord_store_set_channel_counts(store, (id), (u), (m), &err) == CCORD_OK)
  #define UNREAD(id) (ccord_store_channel(store, (id))->unread_count + ccord_store_channel(store, (id))->mention_count)

  // Nothing unread: nothing is sent.
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 0 && g_ack.n == 0);

  // One channel: just that one, up to its newest message, and only its counts clear.
  COUNTS(KID1, 2, 0); COUNTS(LOOSE, 0, 1); COUNTS(FAR, 1, 0); COUNTS(DMCH, 3, 0);
  CT_CHECK(ccord_session_mark_read(s, HQ, KID1, &acked, &err) == CCORD_OK && acked == 1);
  CT_CHECK(g_ack.n == 1 && g_ack.ch[0] == KID1 && g_ack.msg[0] == 5001);
  CT_CHECK(UNREAD(KID1) == 0 && UNREAD(LOOSE) == 1 && UNREAD(FAR) == 1);
  CT_CHECK(ccord_session_mark_read(s, HQ, KID1, &acked, &err) == CCORD_OK && acked == 0 && g_ack.n == 1);   // already read

  // A category: the channels inside it, not its neighbours.
  COUNTS(KID1, 1, 0); COUNTS(KID2, 0, 4);
  CT_CHECK(ccord_session_mark_read(s, HQ, CAT, &acked, &err) == CCORD_OK && acked == 2 && g_ack.n == 3);
  CT_CHECK(g_ack.msg[1] == 5001 && g_ack.msg[2] == 5002 && UNREAD(KID1) == 0 && UNREAD(KID2) == 0 && UNREAD(LOOSE) == 1);

  // A whole server: what is left in it, and nothing of another server or the direct messages.
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 1 && g_ack.n == 4);
  CT_CHECK(g_ack.ch[3] == LOOSE && g_ack.msg[3] == 5003 && UNREAD(LOOSE) == 0 && UNREAD(FAR) == 1 && UNREAD(DMCH) == 3);
  CT_CHECK(ccord_session_mark_read(s, CCORD_DM_GUILD_ID, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 1);
  CT_CHECK(g_ack.n == 5 && g_ack.ch[4] == DMCH && g_ack.msg[4] == 5040 && UNREAD(DMCH) == 0);

  // Requests that make no sense are refused before anything is sent, and report nothing sent.
  acked = 99;
  CT_CHECK(ccord_session_mark_read(s, 999, CCORD_ID_NONE, &acked, &err) == CCORD_ERR_NOT_FOUND && acked == 0);
  acked = 99;
  CT_CHECK(ccord_session_mark_read(s, HQ, 9999, &acked, &err) == CCORD_ERR_NOT_FOUND && acked == 0);
  CT_CHECK(ccord_session_mark_read(s, HQ, FAR, &acked, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_mark_read(s, CCORD_DM_GUILD_ID, KID1, &acked, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_mark_read(nullptr, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, nullptr, &err) == CCORD_OK);   // `acked` is optional
  CT_CHECK(g_ack.n == 5);

  // A busy server: one call sends at most CCORD_MARK_READ_MAX; the rest stay unread for the next call.
  for (ccord_id i = 0; i < NBULK; i++) COUNTS(BULK + i, 1, 0);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == CCORD_MARK_READ_MAX);
  CT_CHECK(g_ack.n == 5 + CCORD_MARK_READ_MAX);
  CT_CHECK(ccord_store_read_targets(store, HQ, CCORD_ID_NONE, 0, nullptr, 0) == NBULK - CCORD_MARK_READ_MAX);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == NBULK - CCORD_MARK_READ_MAX);
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 0);

  // A refusal part-way: the ones that went out are reported and the others stay unread; a refusal of the very first is an error.
  COUNTS(KID1, 1, 0); COUNTS(KID2, 1, 0); COUNTS(LOOSE, 1, 0);
  size_t base = g_ack.n;
  g_ack.fail_at = base + 1u;
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_OK && acked == 1 && g_ack.n == base + 1u);
  CT_CHECK(ccord_store_read_targets(store, HQ, CCORD_ID_NONE, 0, nullptr, 0) == 2 && err.message[0] == '\0');
  g_ack.fail_at = g_ack.n;
  CT_CHECK(ccord_session_mark_read(s, HQ, CCORD_ID_NONE, &acked, &err) == CCORD_ERR_IO && acked == 0);
  CT_CHECK(strstr(err.message, "ack refused") != nullptr);
  CT_CHECK(ccord_store_read_targets(store, HQ, CCORD_ID_NONE, 0, nullptr, 0) == 2);
  #undef COUNTS
  #undef UNREAD
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Where the user has read up to decides what is unread: a stale acknowledgement from another device leaves what is newer
// unread, the user's own messages are read by definition, and the open channel counts as read only while it is being read.
static void test_session_read_position(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, GUILD = 10, LOBBY = 20, LOOSE = 23 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[2] = {
    {.id = LOBBY, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = "", .position = 0,
     .last_message_id = 700, .last_read_id = 700},
    {.id = LOOSE, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "loose", .topic = "", .position = 1,
     .last_message_id = 900, .last_read_id = 850, .unread_count = 3, .mention_count = 1},
  };
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 2};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  static ccord_transport_vtable vt;
  vt = fake_vt;
  vt.ack_message = mark_read_ack;
  f->base.vt = &vt;
  memset(&g_ack, 0, sizeof g_ack);
  g_ack.fail_at = SIZE_MAX;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  const ccord_channel *c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 850 && c->unread_count == 3 && c->mention_count == 1);
  CT_CHECK(ccord_store_selected_channel(store) == LOBBY);

  #define ACK_EVENT(chan, upto, mentions) do { \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_ACK, .channel_id = (chan), .message_id = (upto), .mention_count = (mentions)}; \
    CT_CHECK(ccord_session_pump(s) == 1); } while (0)
  ccord_id next_id = 900;
  #define ARRIVES(chan, who, text, rich_ptr) do { \
    ++next_id; \
    ccord_message mm = {.id = next_id, .channel_id = (chan), .author_id = (who), .timestamp_ms = (int64_t)next_id, .content = (text), .rich = (rich_ptr)}; \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = (chan), .messages = &mm, .message_count = 1}; \
    CT_CHECK(ccord_session_pump(s) == 1); } while (0)

  // Another device read up to 880 and says one mention is still waiting: that one stays, and what is newer is unread.
  ACK_EVENT(LOOSE, 880, 1);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 880 && c->unread_count == 1 && c->mention_count == 1);
  // The same position without the mention clears it: the count comes from the acknowledgement, not from what was there.
  ACK_EVENT(LOOSE, 880, 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 880 && c->unread_count == 1 && c->mention_count == 0);
  // ... and up to the newest message: all read, whatever mention count came with it.
  ACK_EVENT(LOOSE, 900, 5);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->last_read_id == 900 && c->unread_count == 0 && c->mention_count == 0);
  // An acknowledgement that names no message means the newest one.
  ARRIVES(LOOSE, ANN, "one", nullptr);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1 && c->last_read_id == 900);
  ACK_EVENT(LOOSE, CCORD_ID_NONE, 0);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 0 && c->last_read_id == 901);
  // An acknowledgement of a channel we do not know is ignored.
  ACK_EVENT(9999, 5, 0);

  // The user's own message is read by definition, wherever it was written, and it moves the position to itself.
  ARRIVES(LOOSE, ANN, "two", nullptr);
  ARRIVES(LOOSE, ANN, "three", nullptr);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 0);
  ccord_id me[1] = {SELF};
  ccord_message_rich ping = {.present = CCORD_RICH_ALL, .mention_users = me, .mention_user_count = 1};
  ARRIVES(LOOSE, SELF, "from my phone", &ping);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0 && c->last_read_id == next_id);
  ccord_notification note;
  CT_CHECK(!ccord_session_pop_notification(s, &note));

  // The acknowledgement the client sends moves the position too, and an older one leaves the newer messages unread.
  ARRIVES(LOOSE, ANN, "four", nullptr);
  ARRIVES(LOOSE, ANN, "five", &ping);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 2 && c->mention_count == 1);
  CT_CHECK(ccord_session_ack_message(s, LOOSE, next_id - 1, &err) == CCORD_OK);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1 && c->last_read_id == next_id - 1);
  CT_CHECK(ccord_session_ack_message(s, LOOSE, next_id, &err) == CCORD_OK);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 0 && c->mention_count == 0 && c->last_read_id == next_id);
  CT_CHECK(g_ack.n == 2 && g_ack.ch[1] == LOOSE && g_ack.msg[1] == next_id);

  // The open channel: until the UI says otherwise it counts as read; away or scrolled up it does not.
  ARRIVES(LOBBY, ANN, "hello lobby", nullptr);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 0);
  ccord_session_set_reading(s, CCORD_ID_NONE);
  ARRIVES(LOBBY, ANN, "anyone here?", &ping);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);
  // Back and looking at it: new messages there are read at once, and another channel is not the one being read.
  ccord_session_set_reading(s, LOBBY);
  ARRIVES(LOBBY, ANN, "welcome back", nullptr);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 1 && c->mention_count == 1);
  ARRIVES(LOOSE, ANN, "elsewhere", nullptr);
  c = ccord_store_channel(store, LOOSE);
  CT_CHECK(c && c->unread_count == 1);
  ccord_session_set_reading(s, LOOSE);   // switching what is read: the other one is no longer
  ARRIVES(LOBBY, ANN, "lobby again", nullptr);
  c = ccord_store_channel(store, LOBBY);
  CT_CHECK(c && c->unread_count == 2);
  ccord_session_set_reading(nullptr, LOOSE);   // harmless
  #undef ACK_EVENT
  #undef ARRIVES
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Seeing a message's author says nothing new about the member: the nickname and roles the list already has stay.
static void test_session_author_sighting_keeps_member(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, BOB = 3, GUILD = 10, CHAN = 20 };
  ccord_user users[3] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""},
                         {.id = BOB, .name = "Bob", .status_text = ""}};
  ccord_guild guilds[1] = {{.id = GUILD, .name = "HQ"}};
  ccord_channel chans[1] = {{.id = CHAN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = ""}};
  ccord_member members[2] = {{.guild_id = GUILD, .user_id = ANN, .role_name = "Mods", .role_color = 0x336699, .role_rank = 3},
                             {.guild_id = GUILD, .user_id = SELF, .role_name = "Members", .role_rank = 255}};
  snprintf(members[0].nick, sizeof members[0].nick, "Annie");
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 3, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 1, .members = members, .member_count = 2};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  const ccord_member *m = ccord_store_find_member(store, GUILD, ANN);
  CT_CHECK(m && strcmp(m->nick, "Annie") == 0 && strcmp(m->role_name, "Mods") == 0);
  uint64_t lists = ccord_store_lists_revision(store);

  // Ann speaks: nothing about her changes, so the lists have no reason to be rebuilt.
  ccord_message hello = {.id = 101, .channel_id = CHAN, .author_id = ANN, .timestamp_ms = 1, .content = "hello"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &hello, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  m = ccord_store_find_member(store, GUILD, ANN);
  CT_CHECK(m && strcmp(m->nick, "Annie") == 0 && strcmp(m->role_name, "Mods") == 0 && m->role_color == 0x336699);
  CT_CHECK(ccord_store_lists_revision(store) == lists);

  // The same with the member the message carries (roles resolved, nickname as Discord sent it): that is information.
  ccord_member carried = {.guild_id = GUILD, .user_id = ANN, .role_name = "Mods", .role_color = 0x336699, .role_rank = 3};
  snprintf(carried.nick, sizeof carried.nick, "Annie B.");
  ccord_message again = {.id = 102, .channel_id = CHAN, .author_id = ANN, .timestamp_ms = 2, .content = "again"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &again,
                                    .message_count = 1, .guild_members = &carried, .guild_member_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  m = ccord_store_find_member(store, GUILD, ANN);
  CT_CHECK(m && strcmp(m->nick, "Annie B.") == 0 && strcmp(m->role_name, "Mods") == 0);
  CT_CHECK(ccord_store_lists_revision(store) > lists);
  lists = ccord_store_lists_revision(store);

  // Someone the list has not heard of joins it by speaking.
  CT_CHECK(ccord_store_find_member(store, GUILD, BOB) == nullptr);
  ccord_message from_bob = {.id = 103, .channel_id = CHAN, .author_id = BOB, .timestamp_ms = 3, .content = "hi all"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &from_bob, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  m = ccord_store_find_member(store, GUILD, BOB);
  CT_CHECK(m && strcmp(m->role_name, "Members") == 0 && m->role_rank == 255 && m->nick[0] == '\0');
  CT_CHECK(ccord_store_lists_revision(store) > lists);
  lists = ccord_store_lists_revision(store);
  ccord_message bob_again = {.id = 104, .channel_id = CHAN, .author_id = BOB, .timestamp_ms = 4, .content = "me again"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .guild_id = GUILD, .channel_id = CHAN, .messages = &bob_again, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_lists_revision(store) == lists);
  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// The member list of a big server arrives a hundred positions at a time: the session asks for the next piece when the
// interface says so, once the server has said how big the list is, one piece at a time, never faster than once a second,
// and stops when everything has come.
static void test_session_member_list_paging(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  enum { SELF = 1, ANN = 2, GUILD = 10, CHAN = 20, CHAN2 = 21, GUILD2 = 11, CHAN3 = 30 };
  ccord_user users[2] = {{.id = SELF, .name = "You", .status_text = ""}, {.id = ANN, .name = "Ann", .status_text = ""}};
  ccord_guild guilds[2] = {{.id = GUILD, .name = "HQ"}, {.id = GUILD2, .name = "Annex"}};
  ccord_channel chans[3] = {{.id = CHAN, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "lobby", .topic = ""},
                            {.id = CHAN2, .guild_id = GUILD, .kind = CCORD_CHANNEL_TEXT, .name = "other", .topic = ""},
                            {.id = CHAN3, .guild_id = GUILD2, .kind = CCORD_CHANNEL_TEXT, .name = "annex", .topic = ""}};
  ccord_member members[2] = {{.guild_id = GUILD, .user_id = ANN, .role_name = "Members", .role_rank = 255},
                             {.guild_id = GUILD, .user_id = SELF, .role_name = "Members", .role_rank = 255}};
  ccord_snapshot snap = {.self_id = SELF, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 2,
                         .channels = chans, .channel_count = 3, .members = members, .member_count = 2};
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_pump(s) >= 1);
  CT_CHECK(ccord_store_selected_guild(store) == GUILD && ccord_store_selected_channel(store) == CHAN);
  const ccord_member_group groups[2] = {{.kind = CCORD_MEMBER_GROUP_ONLINE, .count = 40},
                                        {.kind = CCORD_MEMBER_GROUP_OFFLINE, .count = 360}};
  // A list update: `total` members in the list, `synced` positions reached by the SYNC in it, `added` new members in it.
  uint64_t next_uid = 1000;
  #define UPDATE(guild, total, synced, added) do { \
    ccord_member new_members[8]; ccord_user new_users[8]; char new_names[8][24]; \
    const size_t added_ = (added); \
    for (size_t k_ = 0; k_ < added_; k_++) { \
      snprintf(new_names[k_], sizeof new_names[k_], "User %llu", (unsigned long long)next_uid); \
      new_users[k_] = (ccord_user){.id = next_uid, .name = new_names[k_], .status_text = ""}; \
      new_members[k_] = (ccord_member){.guild_id = (guild), .user_id = next_uid, .role_name = "Members", .role_rank = 255}; \
      next_uid++; \
    } \
    f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_GUILD_MEMBERS, .guild_id = (guild), .guild_members = new_members, \
      .guild_member_count = added_, .users = new_users, .user_count = added_, .member_total = (total), .member_online = 40, \
      .member_groups = groups, .member_group_count = 2, .member_synced_to = (synced)}; \
    CT_CHECK(ccord_session_pump(s) == 1); } while (0)

  // Nobody asks without a session, nor before the server has said how big the list is.
  CT_CHECK(!ccord_session_more_members(nullptr, 1000));
  CT_CHECK(!ccord_session_more_members(s, 1000) && f->members_request_count == 0);
  UPDATE(GUILD, 400, 100, 0);   // the first update: the list holds 400, 40 online
  uint32_t total = 0, online = 0, n = 0;
  CT_CHECK(ccord_store_member_list_size(store, GUILD, &total, &online) && total == 400 && online == 40);
  CT_CHECK(ccord_store_member_group_size(store, GUILD, CCORD_MEMBER_GROUP_OFFLINE, 0, &n) && n == 360);
  // The first request: the second hundred (the first came with opening the channel).
  CT_CHECK(ccord_session_more_members(s, 500));   // (the clock may start anywhere: the first request is not held back)
  CT_CHECK(f->members_request_count == 1 && f->last_member_chunks == 2 && f->last_member_guild == GUILD && f->last_member_channel == CHAN);
  // A piece is awaited: not before it arrives, however often the interface asks (the wait ends after eight seconds).
  CT_CHECK(!ccord_session_more_members(s, 1001) && !ccord_session_more_members(s, 1499) && !ccord_session_more_members(s, 2000));
  CT_CHECK(!ccord_session_more_members(s, 8499) && f->members_request_count == 1);
  // An update that is not the answer (a presence change in what is held, a SYNC of the part held already) does not end it.
  UPDATE(GUILD, 400, 0, 0);
  UPDATE(GUILD, 400, 100, 0);
  CT_CHECK(!ccord_session_more_members(s, 3000) && !ccord_session_more_members(s, 5000) && f->members_request_count == 1);
  UPDATE(GUILD, 400, 200, 5);   // the answer: a SYNC reaching the end of the second hundred
  CT_CHECK(ccord_store_member_count(store, GUILD) == 7);
  // Not faster than once a second, though.
  CT_CHECK(ccord_session_more_members(s, 9000) && f->members_request_count == 2 && f->last_member_chunks == 3);
  UPDATE(GUILD, 400, 300, 5);
  CT_CHECK(!ccord_session_more_members(s, 9500) && f->members_request_count == 2);   // too soon
  CT_CHECK(ccord_session_more_members(s, 10000) && f->members_request_count == 3 && f->last_member_chunks == 4);
  UPDATE(GUILD, 400, 400, 5);
  // A request that could not be sent (outbound limit) is not retried at once either, and does not count as asked for.
  f->members_fail = true;
  CT_CHECK(!ccord_session_more_members(s, 11000) && f->members_request_count == 4);
  CT_CHECK(!ccord_session_more_members(s, 11500) && f->members_request_count == 4);
  f->members_fail = false;
  CT_CHECK(ccord_session_more_members(s, 12000) && f->members_request_count == 5 && f->last_member_chunks == 5);   // the same piece
  // An answer for another server's list does not end the wait.
  UPDATE(GUILD2, 9, 500, 0);
  CT_CHECK(!ccord_session_more_members(s, 14000) && f->members_request_count == 5);
  UPDATE(GUILD, 400, 500, 5);
  CT_CHECK(ccord_store_member_count(store, GUILD) == 22);
  // Everything the server said the list holds is held: nothing more to ask for.
  UPDATE(GUILD, 22, 500, 0);
  CT_CHECK(!ccord_session_more_members(s, 20000) && !ccord_session_more_members(s, 30000) && f->members_request_count == 5);
  // The list grows again (a bigger total): more is asked for.
  UPDATE(GUILD, 40, 500, 0);
  CT_CHECK(ccord_session_more_members(s, 40000) && f->members_request_count == 6 && f->last_member_chunks == 6);
  UPDATE(GUILD, 400, 600, 5);

  // Another channel's list starts at the top again, so its second hundred is asked for.
  CT_CHECK(ccord_session_select_channel(s, CHAN2, &err) == CCORD_OK);
  CT_CHECK(ccord_session_more_members(s, 50000) && f->members_request_count == 7 && f->last_member_chunks == 2 && f->last_member_channel == CHAN2);
  // A server that never answers is asked again after eight seconds.
  CT_CHECK(!ccord_session_more_members(s, 57999) && f->members_request_count == 7);
  CT_CHECK(ccord_session_more_members(s, 58000) && f->members_request_count == 8 && f->last_member_chunks == 3);
  // A piece that added nobody: the list has no more, and nobody asks again (until the list is opened afresh).
  UPDATE(GUILD, 400, 300, 0);
  CT_CHECK(!ccord_session_more_members(s, 70000) && !ccord_session_more_members(s, 90000) && f->members_request_count == 8);
  CT_CHECK(ccord_session_select_channel(s, CHAN, &err) == CCORD_OK);
  CT_CHECK(ccord_session_more_members(s, 100000) && f->members_request_count == 9 && f->last_member_chunks == 2);
  // A server whose list has not been heard from yet: nothing to ask for (and the conversations have no list at all).
  CT_CHECK(ccord_session_select_channel(s, CHAN3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_selected_guild(store) == GUILD2);
  CT_CHECK(ccord_store_member_list_size(store, GUILD2, &total, nullptr) && total == 9);   // (told by the update above)
  CT_CHECK(ccord_session_more_members(s, 110000) && f->members_request_count == 10 && f->last_member_guild == GUILD2);
  CT_CHECK(ccord_session_select_guild(s, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(!ccord_session_more_members(s, 120000) && f->members_request_count == 10);
  // However long the list, a list is only paged so far: the cap on the chunks (the server is asked for each of them once).
  CT_CHECK(ccord_session_select_channel(s, CHAN, &err) == CCORD_OK);
  UPDATE(GUILD, 9000, 100, 1);
  size_t asked = 0;
  uint64_t t = 200000;
  for (; asked < 2u * CCORD_MEMBER_CHUNKS_MAX && ccord_session_more_members(s, t); t += 1000) {
    asked++;
    UPDATE(GUILD, 9000, f->last_member_chunks * CCORD_MEMBER_CHUNK, 1);
  }
  CT_CHECK(asked == CCORD_MEMBER_CHUNKS_MAX - 1u && f->last_member_chunks == CCORD_MEMBER_CHUNKS_MAX);
  CT_CHECK(!ccord_session_more_members(s, t + 100000));
  #undef UPDATE

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_polls_components_and_modals(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 31, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_id ch = ccord_store_selected_channel(store); // 3111 in guild 2001
  CT_CHECK(ch == 3111);

  ccord_message_rich r_poll = {
    .present = CCORD_RICH_ALL,
    .poll = {
      .present = true,
      .question = "Preferred build tool?",
      .answer_count = 2,
      .answers = {
        {.id = 1, .text = "Ninja", .votes = 3},
        {.id = 2, .text = "Make", .votes = 1},
      },
      .multiselect = false,
      .finalized = false,
    },
  };
  ccord_id poll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1011, "", &r_poll, &poll_mid, &err) == CCORD_OK);
  pump_all(s);

  // Vote does NOT optimistically mutate store before pump_all.
  uint32_t a1 = 1, a2 = 2, multi[2] = {1, 2};
  CT_CHECK(ccord_session_vote_poll(s, ch, poll_mid, &a1, 1, &err) == CCORD_OK);
  const ccord_message *pm = ccord_store_find_message(store, ch, poll_mid);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[0].votes == 3 && !pm->rich->poll.answers[0].me_voted);
  pump_all(s);
  pm = ccord_store_find_message(store, ch, poll_mid);
  CT_CHECK(pm && pm->rich && pm->rich->poll.answers[0].votes == 4 && pm->rich->poll.answers[0].me_voted);

  // Switching answer updates both counts cleanly after pump.
  CT_CHECK(ccord_session_vote_poll(s, ch, poll_mid, &a2, 1, &err) == CCORD_OK);
  pump_all(s);
  pm = ccord_store_find_message(store, ch, poll_mid);
  CT_CHECK(pm && pm->rich->poll.answers[0].votes == 3 && !pm->rich->poll.answers[0].me_voted);
  CT_CHECK(pm && pm->rich->poll.answers[1].votes == 2 && pm->rich->poll.answers[1].me_voted);

  // Session validates single-select, non-poll, unknown channel/message, finalized poll before hitting transport.
  const ccord_message *plain = ccord_store_message_at(store, ch, 0);
  CT_CHECK(plain != nullptr);
  CT_CHECK(ccord_session_vote_poll(s, ch, poll_mid, multi, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_vote_poll(s, ch, plain->id, &a1, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_vote_poll(s, 999999, poll_mid, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_vote_poll(s, ch, 999999, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);

  r_poll.poll.finalized = true;
  ccord_id closed_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1011, "", &r_poll, &closed_mid, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_vote_poll(s, ch, closed_mid, &a1, 1, &err) == CCORD_ERR_INVALID);

  // Component interaction auto-populates guild_id for guild channels and clears guild_id for DM channels;
  // modal events queue in FIFO order via ccord_session_pop_interaction_modal.
  ccord_component btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Form", .custom_id = "modal:first"};
  ccord_component_row row = {.components = &btn, .component_count = 1};
  ccord_message_rich r_comp = {.present = CCORD_RICH_ALL, .application_id = 55501, .components = &row, .component_row_count = 1};
  ccord_id g_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1011, "Guild form", &r_comp, &g_mid, &err) == CCORD_OK);
  pump_all(s);

  CT_CHECK(ccord_session_open_dm(s, 1002, &err) == CCORD_OK);
  pump_all(s);
  ccord_id dm_ch = ccord_store_channel_at(store, CCORD_DM_GUILD_ID, 0)->id;
  ccord_id dm_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, dm_ch, 1011, "DM form", &r_comp, &dm_mid, &err) == CCORD_OK);
  pump_all(s);

  ccord_interaction_request ireq1 = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 55501,
    .channel_id = ch, // guild_id intentionally left 0 -> session fills 2001
    .message_id = g_mid,
    .component_type = CCORD_COMP_BUTTON,
    .custom_id = "modal:first",
  };
  ccord_interaction_request ireq2 = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 55501,
    .guild_id = CCORD_DM_GUILD_ID, // DM pseudo-guild -> session clears to CCORD_ID_NONE
    .channel_id = dm_ch,
    .message_id = dm_mid,
    .component_type = CCORD_COMP_BUTTON,
    .custom_id = "modal:second",
  };
  CT_CHECK(ccord_session_send_interaction(s, &ireq1, &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_interaction(s, &ireq2, &err) == CCORD_OK);
  pump_all(s);

  ccord_interaction_modal md = {0};
  CT_CHECK(ccord_session_pop_interaction_modal(s, &md));
  CT_CHECK(strcmp(md.custom_id, "first") == 0 && md.guild_id == 2001 && md.channel_id == ch);
  CT_CHECK(ccord_session_pop_interaction_modal(s, &md));
  CT_CHECK(strcmp(md.custom_id, "second") == 0 && md.guild_id == CCORD_ID_NONE && md.channel_id == dm_ch);
  CT_CHECK(!ccord_session_pop_interaction_modal(s, &md));

  // Invalid interaction calls fail before transport.
  ccord_interaction_request bad = ireq1;
  bad.channel_id = 999999;
  CT_CHECK(ccord_session_send_interaction(s, &bad, &err) == CCORD_ERR_NOT_FOUND);
  bad = ireq1;
  bad.application_id = 0;
  CT_CHECK(ccord_session_send_interaction(s, &bad, &err) == CCORD_ERR_INVALID);
  bad = ireq1;
  bad.custom_id = "";
  CT_CHECK(ccord_session_send_interaction(s, &bad, &err) == CCORD_ERR_INVALID);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_ephemeral_slash_reply_and_uploads(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mc = {.big_channel_messages = 30};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_id ch = 3111; // selected channel (#general in C-Cord HQ)
  ccord_id other_ch = 3113; // unselected channel (#dev in C-Cord HQ)
  CT_CHECK(ccord_session_select_channel(s, ch, &err) == CCORD_OK);
  pump_all(s);

  // Drain any prior notifications.
  ccord_notification notif;
  while (ccord_session_pop_notification(s, &notif)) {}

  // 1. Ephemeral message in an UNSELECTED channel must NOT increment unread/mentions, advance last_message_id, or notify.
  const ccord_channel *oc = ccord_store_channel(store, other_ch);
  CT_CHECK(oc != nullptr);
  ccord_id prev_last = oc->last_message_id;
  uint32_t prev_unread = oc->unread_count, prev_mentions = oc->mention_count;
  ccord_id self_id = ccord_store_self_id(store);
  ccord_message_rich erich = {
    .present = CCORD_RICH_ALL, .type = 20, .flags = CCORD_MSGFLAG_EPHEMERAL,
    .mention_users = &self_id, .mention_user_count = 1,
  };
  ccord_id ephem_other = 0;
  CT_CHECK(ccord_mock_post_rich_message(tr, other_ch, 1011, "Secret <@1001>", &erich, &ephem_other, &err) == CCORD_OK);
  pump_all(s);
  oc = ccord_store_channel(store, other_ch);
  CT_CHECK(oc->last_message_id == prev_last && oc->unread_count == prev_unread && oc->mention_count == prev_mentions);
  CT_CHECK(!ccord_session_pop_notification(s, &notif));

  // 2. Ephemeral message in SELECTED channel (/ping) is stored locally, does NOT ACK, and dismisses locally.
  CT_CHECK(ccord_session_ensure_slash_commands(s, 2001, ch, 1000));
  // Dedup while in flight:
  CT_CHECK(!ccord_session_ensure_slash_commands(s, 2001, ch, 1050));
  pump_all(s);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_SLASH_INDEX) == 1);
  // Cached within TTL:
  CT_CHECK(!ccord_session_ensure_slash_commands(s, 2001, ch, 2000));
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_SLASH_INDEX) == 1);
  size_t ncmds = 0;
  const ccord_slash_command *cmds = ccord_store_slash_commands(store, 2001, &ncmds);
  CT_CHECK(cmds != nullptr && ncmds == 7);

  size_t ack0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  ccord_interaction_request ping_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .channel_id = ch,
    .command_id = 9101, .command_name = "ping", .focused_option = -1,
  };
  CT_CHECK(ccord_session_send_interaction(s, &ping_req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack0);
  const ccord_message *last_m = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1u);
  CT_CHECK(last_m && ccord_message_is_ephemeral(last_m));
  ccord_id ephem_id = last_m->id;
  CT_CHECK(ccord_session_dismiss_ephemeral(s, ch, ephem_id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_find_message(store, ch, ephem_id) == nullptr);

  // 3. Slash autocomplete populates choices and bumps seq; clear resets count.
  uint64_t seq0 = 0;
  size_t nch0 = 99;
  (void)ccord_session_slash_choices(s, &nch0, &seq0);
  CT_CHECK(nch0 == 0);
  const char *oname = "subject", *oval = "vul";
  uint8_t okind = CCORD_SLASH_OPT_STRING;
  ccord_interaction_request ac_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = 1011, .channel_id = ch,
    .command_id = 9103, .command_name = "topic",
    .option_names = &oname, .option_kinds = &okind, .option_values = &oval, .option_count = 1,
    .focused_option = 0,
  };
  CT_CHECK(ccord_session_send_interaction(s, &ac_req, &err) == CCORD_OK);
  pump_all(s);
  uint64_t seq1 = 0;
  size_t nch1 = 0;
  const ccord_slash_choice *choices = ccord_session_slash_choices(s, &nch1, &seq1);
  CT_CHECK(seq1 > seq0 && nch1 == 1 && choices && strcmp(choices[0].value, "vulkan") == 0);
  ccord_session_clear_slash_choices(s);
  (void)ccord_session_slash_choices(s, &nch1, &seq1);
  CT_CHECK(nch1 == 0);

  // 4. send_reply_ex (mention_author false) and send_message_with_files (multi-file + spoiler).
  const ccord_message *target = nullptr;
  for (size_t i = 0; i < ccord_store_message_count(store, ch); i++) {
    const ccord_message *cand = ccord_store_message_at(store, ch, i);
    if (cand && cand->author_id != self_id && !ccord_message_is_ephemeral(cand)) { target = cand; break; }
  }
  CT_CHECK(target != nullptr);
  CT_CHECK(ccord_session_send_reply_ex(s, ch, target->id, false, "quiet reply", 11, &err) == CCORD_OK);
  pump_all(s);
  last_m = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1u);
  CT_CHECK(last_m && last_m->rich && last_m->rich->mention_user_count == 0);

  const uint8_t bytes[4] = {1, 2, 3, 4};
  ccord_upload_file ufiles[2] = {
    {.filename = "one.png", .data = bytes, .size = 4, .spoiler = false},
    {.filename = "two.png", .data = bytes, .size = 4, .spoiler = true},
  };
  CT_CHECK(ccord_session_send_message_with_files(s, ch, target->id, false, "attached", 8, ufiles, 2, &err) == CCORD_OK);
  pump_all(s);
  last_m = ccord_store_message_at(store, ch, ccord_store_message_count(store, ch) - 1u);
  CT_CHECK(last_m && last_m->rich && last_m->rich->attachment_count == 2 &&
           !last_m->rich->attachments[0].spoiler && last_m->rich->attachments[1].spoiler &&
           last_m->rich->mention_user_count == 0);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_search_pins_threads_and_reactions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mc = {.big_channel_messages = 120, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_id ch = 3111; // #general in guild 2001
  CT_CHECK(ccord_session_select_channel(s, ch, &err) == CCORD_OK);
  pump_all(s);

  // 1. Structured search with name resolution (`in:dev`, `from:me`) and offset pagination.
  CT_CHECK(ccord_session_send_message(s, 3113, "needle-in-dev", 13, &err) == CCORD_OK);
  pump_all(s);
  const char *q1 = "in:dev from:me needle-in-dev";
  CT_CHECK(ccord_session_search_messages_page(s, 2001, CCORD_ID_NONE, q1, strlen(q1), 0, &err) == CCORD_OK);
  CT_CHECK(ccord_session_search_loading(s));
  pump_all(s);
  CT_CHECK(!ccord_session_search_loading(s) && ccord_session_search_total_results(s) == 1 &&
           ccord_session_search_result_count(s) == 1 &&
           ccord_session_search_result_at(s, 0)->channel_id == 3113);

  const char *q2 = "in";
  CT_CHECK(ccord_session_search_messages_page(s, 2001, CCORD_ID_NONE, q2, strlen(q2), 25, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_session_search_offset(s) == 25 && ccord_session_search_total_results(s) > 25 &&
           ccord_session_search_result_count(s) > 0);

  // 2. Pins pagination: request_pins + request_more_pins guard & dedupe.
  CT_CHECK(ccord_session_request_pins(s, ch, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel_pins_loaded(store, ch));
  // Without has_more, request_more_pins is a clean no-op (zero network calls); invalid id returns CCORD_ERR_INVALID.
  CT_CHECK(ccord_session_request_more_pins(s, CCORD_ID_NONE, &err) == CCORD_ERR_INVALID);
  size_t pc_pre = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS);
  CT_CHECK(ccord_session_request_more_pins(s, ch, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS) == pc_pre);

  // Seed has_more cursor in store and verify request_more_pins + in-flight dedupe.
  const ccord_message *m0 = ccord_store_message_at(store, ch, 0);
  CT_CHECK(m0 != nullptr);
  ccord_id m0_id = m0->id;
  ccord_message pm = {
    .id = m0_id, .channel_id = ch, .author_id = m0->author_id,
    .timestamp_ms = m0->timestamp_ms, .content = "pinned seed", .pinned = true,
  };
  CT_CHECK(ccord_store_set_pins_page(store, ch, &pm, 1, false, true, "9999999999999999999", &err) == CCORD_OK);
  size_t pc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS);
  CT_CHECK(ccord_session_request_more_pins(s, ch, &err) == CCORD_OK);
  CT_CHECK(ccord_session_pins_loading(s, ch));
  CT_CHECK(ccord_session_request_more_pins(s, ch, &err) == CCORD_OK); // deduped while in flight!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PINS) == pc0 + 1);
  pump_all(s);
  CT_CHECK(!ccord_session_pins_loading(s, ch));

  // 3. Create thread from message -> starter rich->thread linked -> open_thread -> join/leave/archive.
  ccord_admin_request req;
  CT_CHECK(ccord_admin_create_thread_from_message(&req, 2001, ch, m0_id, "Session Thread", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_message *st_msg = ccord_store_find_message(store, ch, m0_id);
  CT_CHECK(st_msg && st_msg->rich && st_msg->rich->thread.id != CCORD_ID_NONE);
  ccord_id tid = st_msg->rich->thread.id;

  CT_CHECK(ccord_session_open_thread(s, 2001, ch, tid, "Session Thread", &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_selected_channel(store) == tid);

  CT_CHECK(ccord_admin_leave_thread(&req, 2001, tid, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_store_channel(store, tid)->thread_joined);

  CT_CHECK(ccord_admin_join_thread(&req, 2001, tid, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel(store, tid)->thread_joined);

  CT_CHECK(ccord_admin_archive_thread(&req, 2001, tid, true, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel(store, tid)->thread_archived);

  // 4. Reaction users fetch, in-flight dedupe, and cache hit.
  CT_CHECK(ccord_session_toggle_reaction(s, ch, m0_id, "\xF0\x9F\x9A\x80", 4, &err) == CCORD_OK);
  pump_all(s);
  size_t ruc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_REACTION_USERS);
  CT_CHECK(ccord_session_request_reaction_users(s, ch, m0_id, "\xF0\x9F\x9A\x80", 50, &err) == CCORD_OK);
  CT_CHECK(ccord_session_request_reaction_users(s, ch, m0_id, "\xF0\x9F\x9A\x80", 50, &err) == CCORD_OK); // deduped!
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_REACTION_USERS) == ruc0 + 1);
  pump_all(s);
  ccord_id out_c = 0, out_m = 0;
  const char *out_e = nullptr;
  size_t out_n = 0;
  bool out_l = true;
  const ccord_user *rusers = ccord_session_reaction_users(s, &out_c, &out_m, &out_e, &out_n, &out_l);
  CT_CHECK(!out_l && out_c == ch && out_m == m0_id && out_e && strcmp(out_e, "\xF0\x9F\x9A\x80") == 0 &&
           rusers != nullptr && out_n >= 1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_m3b_folders_reorder_invites_and_discovery(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mcfg = {.seed = 41, .big_channel_messages = 10};
  ccord_transport *tr = ccord_mock_transport_create(&mcfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(bus && store && tr && s);
  if (!bus || !store || !tr || !s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // 1. Guild folder layout via session admin.
  ccord_admin_request req;
  ccord_guild_folder_item fitems[2] = {
    {.guild_id = 2002, .folder_id = 404, .folder_name = "Code", .folder_color = 0x5865F2},
    {.guild_id = 2001, .folder_id = 404, .folder_name = "Code", .folder_color = 0x5865F2},
  };
  CT_CHECK(ccord_admin_set_guild_folders(&req, fitems, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_guild_at(store, 0)->id == 2002 && ccord_store_guild_at(store, 0)->folder_id == 404 &&
           strcmp(ccord_store_guild_at(store, 0)->folder_name, "Code") == 0);
  CT_CHECK(ccord_store_guild_at(store, 1)->id == 2001 && ccord_store_guild_at(store, 1)->folder_id == 404);

  // 2. Channel reorder allowed on 2001 (user is owner), refused locally on 2003 (Pixel Garden: exact perms lack Manage Channels).
  ccord_id cids[2] = {3112, 3111};
  int32_t cpos[2] = {0, 1};
  ccord_id cpars[2] = {3110, 3110};
  uint8_t chas[2] = {1, 1};
  CT_CHECK(ccord_admin_reorder_channels(&req, 2001, cids, cpos, cpars, chas, 2, "reorder", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_channel(store, 3112)->position == 0 && ccord_store_channel(store, 3111)->position == 1);

  ccord_id pcids[1] = {3311};
  int32_t pcpos[1] = {5};
  ccord_id pcpars[1] = {0};
  uint8_t pchas[1] = {0};
  CT_CHECK(ccord_admin_reorder_channels(&req, 2003, pcids, pcpos, pcpars, pchas, 1, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);

  // 3. Invite preview, clear, accept (auto-selects joined server), and create server (auto-selects created server).
  bool loading = false;
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  (void)ccord_session_invite_preview(s, &loading);
  CT_CHECK(loading);
  pump_all(s);
  const ccord_invite_preview *prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003 && strcmp(prev->guild_name, "Systems Programming Study") == 0);
  ccord_session_clear_invite_preview(s);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);

  CT_CHECK(ccord_admin_accept_invite(&req, "c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_selected_guild(store) == 9003 && ccord_store_guild(store, 9003) != nullptr);

  CT_CHECK(ccord_admin_create_guild(&req, "Session Lab", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_guild *sel_g = ccord_store_guild(store, ccord_store_selected_guild(store));
  CT_CHECK(sel_g && strcmp(sel_g->name, "Session Lab") == 0);

  // 4. Server Discovery: in-flight dedupe, 60 s TTL cache hit, TTL expiry, force bypass, and category/query change.
  size_t dc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY);
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000, false, &err) == CCORD_OK);
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100100, false, &err) == CCORD_OK); // in-flight dedupe
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 1);
  pump_all(s);

  size_t cnt = 0;
  uint32_t tot = 0, off = 99;
  const ccord_discover_guild *dg = ccord_session_discovery_results(s, &cnt, &tot, &off, &loading);
  CT_CHECK(!loading && dg && cnt >= 5 && tot >= 5 && off == 0);

  // Within 60 s TTL: 0 extra calls!
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000 + CCORD_DISCOVERY_TTL_MS - 1, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 1);

  // At 60 s TTL boundary: refetches!
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000 + CCORD_DISCOVERY_TTL_MS, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 2);
  pump_all(s);

  // Force = true bypasses fresh cache.
  CT_CHECK(ccord_session_request_discovery(s, "", 0, 0, 100000 + CCORD_DISCOVERY_TTL_MS + 10, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 3);
  pump_all(s);

  // Different category/query triggers fresh call.
  CT_CHECK(ccord_session_request_discovery(s, "vulkan", 4, 0, 100000 + CCORD_DISCOVERY_TTL_MS + 20, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_DISCOVERY) == dc0 + 4);
  pump_all(s);
  dg = ccord_session_discovery_results(s, &cnt, &tot, &off, &loading);
  CT_CHECK(!loading && cnt == 1 && dg[0].guild_id == 9004);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_m3c_group_dms_requests_profiles_and_custom_status(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config mcfg = {.seed = 43, .big_channel_messages = 10};
  ccord_transport *tr = ccord_mock_transport_create(&mcfg, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(bus && store && tr && s);
  if (!bus || !store || !tr || !s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  ccord_session_set_reading(s, 3111); // reading #general, not DM requests

  // 1. Profile request TTL cache (60 s), in-flight dedupe, TTL expiry, force bypass, guild_id scope change.
  size_t pc0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE);
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2001, 200000, false, &err) == CCORD_OK);
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2001, 200050, false, &err) == CCORD_OK); // in-flight dedupe
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 1);
  pump_all(s);
  const ccord_user_profile *prof = ccord_store_user_profile(store, 1002);
  CT_CHECK(prof && prof->user_id == 1002 && prof->connection_count >= 2 && prof->mutual_guild_count >= 1);

  // Within 60 s TTL for same (user_id, guild_id): 0 extra calls!
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2001, 200000 + CCORD_PROFILE_TTL_MS - 1, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 1);

  // Different guild_id scope -> refetches!
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2002, 200000 + 500, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 2);
  pump_all(s);

  // At 60 s TTL boundary -> refetches!
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2002, 200500 + CCORD_PROFILE_TTL_MS, false, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 3);
  pump_all(s);

  // Force = true bypasses fresh cache.
  CT_CHECK(ccord_session_request_profile_ex(s, 1002, 2002, 200500 + CCORD_PROFILE_TTL_MS + 10, true, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_PROFILE) == pc0 + 4);
  pump_all(s);

  // 2. Custom status + pending presence in the same frame preserves both!
  CT_CHECK(ccord_session_set_presence(s, CCORD_PRESENCE_IDLE, "Away", &err) == CCORD_OK);
  CT_CHECK(ccord_session_set_custom_status(s, "\xF0\x9F\x8E\xAF", 0, "Deep work", 1900000000000LL, &err) == CCORD_OK);
  pump_all(s);
  const ccord_user *me = ccord_store_user(store, 1001);
  CT_CHECK(me && me->presence == CCORD_PRESENCE_IDLE && strcmp(me->custom_status_text, "Deep work") == 0 &&
           me->custom_status_expires_ms == 1900000000000LL);

  // Clear custom status while keeping IDLE presence.
  CT_CHECK(ccord_session_set_custom_status(s, "", 0, "", 0, &err) == CCORD_OK);
  pump_all(s);
  me = ccord_store_user(store, 1001);
  CT_CHECK(me && me->presence == CCORD_PRESENCE_IDLE && me->custom_status_text[0] == '\0');

  // 3. Group DM creation auto-selects the new Group DM; non-owner kick refused with CCORD_ERR_PERMISSION.
  ccord_admin_request req;
  ccord_id r2[2] = {1002, 1003};
  CT_CHECK(ccord_admin_create_group_dm(&req, r2, 2, "Systems Sync", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  ccord_id gdm_id = ccord_store_selected_channel(store);
  const ccord_channel *gdm = ccord_store_channel(store, gdm_id);
  CT_CHECK(gdm && gdm->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM && strcmp(gdm->name, "Systems Sync") == 0);

  // Insert a Group DM owned by someone else (1002): removing 1003 is refused locally, removing self (1001) is allowed!
  ccord_channel foreign_gdm = {
    .id = 888500, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM, .name = "Ada's Group", .topic = "",
    .user_id = 1002, .owner_id = 1002, .recipient_count = 2, .recipient_ids = {1002, 1003},
  };
  CT_CHECK(ccord_store_upsert_channel(store, &foreign_gdm, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_remove_dm_recipient(&req, 888500, 1003, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);

  // 4. Unaccepted message requests: spam suppresses notifications, sending/replying/typing refused until accepted!
  ccord_notification notif;
  while (ccord_session_pop_notification(s, &notif)) {
  }
  ccord_id req_cid = 0, spam_cid = 0;
  CT_CHECK(ccord_mock_push_message_request(tr, 1008, "Hi, quick question!", false, &req_cid, &err) == CCORD_OK);
  pump_all(s);
  // Non-spam message request produces a DM notification; spam message request does NOT!
  CT_CHECK(ccord_session_pop_notification(s, &notif) && notif.channel_id == req_cid && notif.is_dm);
  CT_CHECK(!ccord_session_pop_notification(s, &notif));

  CT_CHECK(ccord_mock_push_message_request(tr, 1009, "Win free crypto", true, &spam_cid, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_session_pop_notification(s, &notif)); // spam suppressed!

  // Viewing an unaccepted message request does NOT send ACK, and composer actions return CCORD_ERR_PERMISSION.
  size_t ack0 = ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK);
  CT_CHECK(ccord_session_select_channel(s, req_cid, &err) == CCORD_OK);
  ccord_session_set_reading(s, req_cid);
  pump_all(s);
  CT_CHECK(ccord_mock_call_count(tr, CCORD_MOCK_CALL_ACK) == ack0);
  CT_CHECK(ccord_session_send_message(s, req_cid, "hello", 5, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_session_send_typing(s, req_cid, &err) == CCORD_ERR_PERMISSION);
  const ccord_message *first_m = ccord_store_message_at(store, req_cid, 0);
  CT_CHECK(first_m != nullptr);
  if (first_m) {
    CT_CHECK(ccord_session_send_reply(s, req_cid, first_m->id, "hi", 2, &err) == CCORD_ERR_PERMISSION);
  }

  // Accepting the message request unlocks sending!
  CT_CHECK(ccord_admin_accept_message_request(&req, req_cid, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(!ccord_store_channel(store, req_cid)->is_message_request);
  CT_CHECK(ccord_session_send_message(s, req_cid, "Happy to chat!", 14, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_message_count(store, req_cid) == 2);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_m4a_guild_settings_safety_vanity_and_ownership(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 43, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // 1. Local validation on CCORD_ADMIN_EDIT_GUILD before hitting transport:
  // system channel must exist in this guild and be plain text (not voice 3121, forum 3116, thread, or foreign 3211).
  size_t a0 = ccord_mock_admin_count(tr);
  ccord_admin_request req;
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_system_channel = true;
  ge.system_channel_id = 999999;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_NOT_FOUND);

  ge.system_channel_id = 3211; // foreign guild 2002
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  ge.system_channel_id = 3121; // voice channel
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  ge.system_channel_id = 3115; // forum channel (#design-forum)
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  // AFK channel must exist in this guild and be non-stage voice (not text 3111, foreign, or stage).
  ge = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_afk_channel = true;
  ge.afk_channel_id = 999999;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_NOT_FOUND);

  ge.afk_channel_id = 3111; // text channel
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  ccord_channel stage_ch = {
    .id = 3199, .guild_id = 2001, .kind = CCORD_CHANNEL_VOICE,
    .discord_type = CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE, .name = "Stage", .topic = "",
  };
  CT_CHECK(ccord_store_upsert_channel(store, &stage_ch, &err) == CCORD_OK);
  ge.afk_channel_id = 3199;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_admin_count(tr) == a0); // none of those invalid requests reached Discord!

  // Valid extended edit + incident actions + vanity URL flow through session into store.
  ge = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_system_channel = true;
  ge.system_channel_id = 3112;
  ge.has_afk_channel = true;
  ge.afk_channel_id = 3121;
  ge.afk_timeout = 900;
  ge.verification_level = 2;
  ge.explicit_content_filter = 1;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_set_incident_actions(&req, 2001, 1800005000000LL, 0, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_set_vanity_url(&req, 2001, "ccord-session", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_guild *g2001 = ccord_store_guild(store, 2001);
  CT_CHECK(g2001 && g2001->system_channel_id == 3112 && g2001->afk_channel_id == 3121 &&
           g2001->afk_timeout == 900 && g2001->verification_level == 2 && g2001->explicit_content_filter == 1 &&
           g2001->invites_disabled_until_ms == 1800005000000LL &&
           strcmp(g2001->vanity_url_code, "ccord-session") == 0);

  // 2. Owner guards in session: create an owned server (owner_id == 1001).
  CT_CHECK(ccord_admin_create_guild(&req, "Session Owned", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  ccord_id owned_gid = ccord_store_selected_guild(store);
  CT_CHECK(owned_gid != 2001 && ccord_store_guild(store, owned_gid)->owner_id == 1001);

  size_t a1 = ccord_mock_admin_count(tr);
  // Owner cannot leave their own server; cannot transfer ownership to self.
  CT_CHECK(ccord_admin_leave_guild(&req, owned_gid, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  req.op = CCORD_ADMIN_TRANSFER_OWNERSHIP;
  req.guild_id = owned_gid;
  req.user_id = 1001;
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  // Non-owner (on guild 2002 where owner_id == 1002 != 1001) cannot delete or transfer ownership.
  ccord_guild g2002_owned = {.id = 2002, .name = "Lua Lounge", .owner_id = 1002};
  CT_CHECK(ccord_store_upsert_guild(store, &g2002_owned, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_delete_guild(&req, 2002, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_transfer_ownership(&req, 2002, 1003, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == a1);

  // Owner CAN delete owned_gid; non-owner CAN leave 2003.
  CT_CHECK(ccord_admin_delete_guild(&req, owned_gid, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_leave_guild(&req, 2003, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_guild(store, owned_gid) == nullptr && ccord_store_guild(store, 2003) == nullptr);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_m4b_expressions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_transport *tr = ccord_mock_transport_create(nullptr, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // SNAPSHOT populated guild 2001 expressions, without admin_loaded or uploader IDs.
  CT_CHECK(ccord_store_guild_emoji_count(store, 2001) >= 2 && !ccord_store_guild_emojis_admin_loaded(store, 2001));
  CT_CHECK(ccord_store_guild_sticker_count(store, 2001) >= 2 && !ccord_store_guild_stickers_admin_loaded(store, 2001));
  CT_CHECK(ccord_store_guild_soundboard_count(store, 2001) >= 2 && !ccord_store_guild_soundboard_admin_loaded(store, 2001));

  ccord_admin_request req;
  CT_CHECK(ccord_admin_list_emojis(&req, 2001, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_list_stickers(&req, 2001, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_list_soundboard(&req, 2001, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_guild_emojis_admin_loaded(store, 2001) &&
           ccord_store_guild_stickers_admin_loaded(store, 2001) &&
           ccord_store_guild_soundboard_admin_loaded(store, 2001));

  // Find one emoji uploaded by self (1001) and one uploaded by Ada (1002).
  ccord_id own_em = CCORD_ID_NONE, ada_em = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_emoji_count(store, 2001); i++) {
    const ccord_emoji *e = ccord_store_guild_emoji_at(store, 2001, i);
    if (e->user_id == 1001) own_em = e->id;
    else if (e->user_id == 1002) ada_em = e->id;
  }
  CT_CHECK(own_em != CCORD_ID_NONE && ada_em != CCORD_ID_NONE);

  // Create + rename + delete across emoji, sticker, soundboard as Admin.
  CT_CHECK(ccord_admin_create_emoji(&req, 2001, "sess_em", "data:image/png;base64,iVBORw==", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  CT_CHECK(ccord_admin_create_sticker(&req, 2001, "Sess Stick", "", "ok", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_create_soundboard(&req, 2001, "Sess Sound", "data:audio/ogg;base64,T2dnUw==", 75,
                                         CCORD_ID_NONE, "🔔", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_emoji *created_em = ccord_store_find_emoji_by_name(store, "sess_em");
  const ccord_sticker *created_st = ccord_store_find_sticker_by_name(store, "Sess Stick");
  CT_CHECK(created_em && created_st);

  // Now restrict self on guild 2001 to CREATE_EXPRESSIONS only (exact permissions, non-owner):
  // can edit own_em, CANNOT edit or delete ada_em!
  ccord_role roles[2] = {
    {.id = 2001, .guild_id = 2001, .name = "@everyone", .permissions = CCORD_PERM_VIEW_CHANNEL, .position = 0},
    {.id = 2901, .guild_id = 2001, .name = "Creator", .permissions = CCORD_PERM_CREATE_EXPRESSIONS, .position = 1},
  };
  ccord_id self_roles[1] = {2901};
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(store, 2001, 1001, self_roles, 1, &err) == CCORD_OK);

  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_admin_edit_emoji(&req, 2001, ada_em, "hijack_ada", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_delete_emoji(&req, 2001, ada_em, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0);   // blocked locally before hitting transport!

  // Own expression edit IS allowed by session precheck.
  CT_CHECK(ccord_admin_edit_emoji(&req, 2001, own_em, "my_own_ok", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(strcmp(ccord_store_find_emoji(store, own_em)->name, "my_own_ok") == 0);

  // Remove CREATE_EXPRESSIONS too -> list/create/edit all blocked locally!
  roles[1].permissions = 0;
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles, 2, &err) == CCORD_OK);
  size_t adm1 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_admin_list_emojis(&req, 2001, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_create_emoji(&req, 2001, "nope_em", "data:image/png;base64,iVBORw==", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// ---- Expression data: REST answers reach the session, failures fail the blob, and the picker is retryable ----

typedef struct fake_expr_transport {
  ccord_transport base;
  bool queued;
  bool fail_next;
  ccord_expr_kind kind;
  char arg[64];
  char body[8192];
} fake_expr_transport;

static ccord_status fake_expr_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool fake_expr_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_expr_transport *f = (fake_expr_transport *)t;
  if (!f->queued) return false;
  memset(out, 0, sizeof *out);
  out->type = CCORD_NET_EXPRESSION_DATA;
  out->expr_kind = f->kind;
  out->expr_arg = f->arg;
  out->expr_body = f->fail_next ? nullptr : f->body;
  out->expr_body_len = f->fail_next ? 0 : strlen(f->body);
  out->status = f->fail_next ? CCORD_ERR_IO : CCORD_OK;
  f->queued = false;
  return true;
}

static void fake_expr_destroy(ccord_transport *t)
{
  free(t);
}

static ccord_status fake_expr_request(ccord_transport *t, ccord_expr_kind kind, const char *arg, size_t arg_len,
                                      ccord_error *err)
{
  fake_expr_transport *f = (fake_expr_transport *)t;
  if (kind >= CCORD_EXPR_KIND_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "fake: bad expression kind");
  size_t n = arg ? arg_len : 0;
  if (n >= sizeof f->arg) n = sizeof f->arg - 1;
  if (n) memcpy(f->arg, arg, n);
  f->arg[n] = '\0';
  f->kind = kind;
  f->queued = true;
  return CCORD_OK;
}

static const ccord_transport_vtable fake_expr_vt = {
  .name = "fake-expr",
  .connect = fake_expr_connect,
  .poll = fake_expr_poll,
  .destroy = fake_expr_destroy,
  .request_expression_data = fake_expr_request,
};

static void test_session_expression_data_failure(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_expr_transport *ft = calloc(1, sizeof *ft);
  CT_CHECK(bus && store && ft);
  if (!bus || !store || !ft) return;
  ft->base.vt = &fake_expr_vt;
  ft->fail_next = true;
  ccord_session *s = ccord_session_create(bus, store, &ft->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  // A queued fetch is PENDING; the transport failing must turn it FAILED (the old silent drop left it PENDING).
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_TRENDING, "", false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_PENDING);
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_FAILED);
  CT_CHECK(ccord_store_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", nullptr) == nullptr);

  // FAILED is retryable: the next successful answer reaches the store and parses.
  ft->fail_next = false;
  (void)snprintf(ft->body, sizeof ft->body,
                 "[{\"url\":\"https://tenor.com/view/retry-gif-1\","
                 "\"gif_src\":\"https://media.discordapp.net/attachments/1/retry.gif\","
                 "\"width\":111,\"height\":222}]");
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_TRENDING, "", false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_PENDING);
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_READY);
  size_t blob_len = 0;
  const char *blob = ccord_store_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", &blob_len);
  CT_CHECK(blob && blob_len == strlen(ft->body) && strcmp(blob, ft->body) == 0);
  CT_CHECK(ccord_store_gif_trending_count(store) == 1);
  const ccord_gif_item *g = ccord_store_gif_trending_at(store, 0);
  CT_CHECK(g && strcmp(g->url, "https://tenor.com/view/retry-gif-1") == 0);
  CT_CHECK(g && strcmp(g->src, "https://media.discordapp.net/attachments/1/retry.gif") == 0);
  CT_CHECK(g && g->width == 111 && g->height == 222);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Minimal protobuf/base64 writer so the test can hand the session a FrecencyUserSettings answer with
// maximum-length URLs (the picker's page URL/src buffers are 512 bytes).
static size_t test_pb_varint(uint8_t *p, uint64_t v)
{
  size_t n = 0;
  do {
    p[n] = (uint8_t)((v & 0x7fu) | (v > 0x7fu ? 0x80u : 0u));
    v >>= 7;
    n++;
  } while (v != 0);
  return n;
}

static size_t test_pb_bytes(uint8_t *p, unsigned field, const void *data, size_t len)
{
  size_t n = test_pb_varint(p, ((uint64_t)field << 3u) | 2u);
  n += test_pb_varint(p + n, len);
  memcpy(p + n, data, len);
  return n + len;
}

static size_t test_pb_uint(uint8_t *p, unsigned field, uint64_t v)
{
  size_t n = test_pb_varint(p, (uint64_t)field << 3u);
  return n + test_pb_varint(p + n, v);
}

static size_t test_b64(const uint8_t *in, size_t len, char *out)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0, i = 0;
  for (; i + 2 < len; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16u) | ((uint32_t)in[i + 1] << 8u) | in[i + 2];
    out[o++] = tbl[(v >> 18u) & 63u];
    out[o++] = tbl[(v >> 12u) & 63u];
    out[o++] = tbl[(v >> 6u) & 63u];
    out[o++] = tbl[v & 63u];
  }
  if (i < len) {
    uint32_t v = (uint32_t)in[i] << 16u;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8u;
    out[o++] = tbl[(v >> 18u) & 63u];
    out[o++] = tbl[(v >> 12u) & 63u];
    out[o++] = i + 1 < len ? tbl[(v >> 6u) & 63u] : '=';
    out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

static void test_session_expression_settings_512_url(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_expr_transport *ft = calloc(1, sizeof *ft);
  CT_CHECK(bus && store && ft);
  if (!bus || !store || !ft) return;
  ft->base.vt = &fake_expr_vt;
  ccord_session *s = ccord_session_create(bus, store, &ft->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  char url[CCORD_URL_MAX_BYTES + 1], src[CCORD_URL_MAX_BYTES + 1];
  size_t plen = (size_t)snprintf(url, sizeof url, "https://tenor.com/view/");
  memset(url + plen, 'u', CCORD_URL_MAX_BYTES - plen);
  url[CCORD_URL_MAX_BYTES] = '\0';
  plen = (size_t)snprintf(src, sizeof src, "https://media.discordapp.net/attachments/1/");
  memset(src + plen, 's', CCORD_URL_MAX_BYTES - plen);
  src[CCORD_URL_MAX_BYTES] = '\0';

  uint8_t vg[2048], entry[4096], gifs[8192], raw[8192];
  char b64[12000];
  size_t vn = 0;
  vn += test_pb_bytes(vg + vn, 2, src, CCORD_URL_MAX_BYTES);
  vn += test_pb_uint(vg + vn, 3, 720);
  vn += test_pb_uint(vg + vn, 4, 480);
  vn += test_pb_uint(vg + vn, 5, 1);
  size_t en = 0;
  en += test_pb_bytes(entry + en, 1, url, CCORD_URL_MAX_BYTES);
  en += test_pb_bytes(entry + en, 2, vg, vn);
  size_t gn = 0;
  gn += test_pb_bytes(gifs + gn, 1, entry, en);
  size_t rn = 0;
  rn += test_pb_bytes(raw + rn, 2, gifs, gn);
  char *body = ft->body;
  int hn = snprintf(body, sizeof ft->body, "{\"settings\":\"");
  size_t bn = test_b64(raw, rn, b64);
  CT_CHECK(hn > 0 && (size_t)hn + bn + 3 <= sizeof ft->body);
  memcpy(body + (size_t)hn, b64, bn);
  memcpy(body + (size_t)hn + bn, "\"}", 3);

  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_SETTINGS_PROTO, "", false, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, "") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 1);
  const ccord_gif_item *fav = ccord_store_favorite_gif_at(store, 0);
  CT_CHECK(fav && strlen(fav->url) == CCORD_URL_MAX_BYTES && strcmp(fav->url, url) == 0);
  CT_CHECK(fav && strlen(fav->src) == CCORD_URL_MAX_BYTES && strcmp(fav->src, src) == 0);
  CT_CHECK(fav && fav->width == 720 && fav->height == 480);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_expression_catalog_and_favorites(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_transport *tr = ccord_mock_transport_create(nullptr, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  if (!s) return;

  // settings-proto/2: FrecencyUserSettings favorite GIFs populate the store with page URL, media src, dimensions.
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, "") == CCORD_EXPR_BLOB_NONE);
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_SETTINGS_PROTO, "", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, "") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 2);
  // Newest first: the seeded ship-it favorite (order 2) leads wumpus (order 1), as Discord draws them.
  const ccord_gif_item *fav0 = ccord_store_favorite_gif_at(store, 0);
  const ccord_gif_item *fav1 = ccord_store_favorite_gif_at(store, 1);
  CT_CHECK(fav0 && strcmp(fav0->url, "https://tenor.com/view/ccord-ship-it-gif-1002") == 0 && fav0->order == 2);
  CT_CHECK(fav0 && strcmp(fav0->src, "https://media.discordapp.net/attachments/2001/gif_ship_it.gif") == 0);
  CT_CHECK(fav0 && fav0->width == 320 && fav0->height == 220);
  CT_CHECK(fav1 && strcmp(fav1->url, "https://tenor.com/view/wumpus-wave-gif-1001") == 0 && fav1->order == 1);
  CT_CHECK(fav1 && strcmp(fav1->src, "https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif") == 0);
  CT_CHECK(fav1 && fav1->width == 320 && fav1->height == 240);

  // Applying the same proto again neither duplicates nor drops an entry.
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_SETTINGS_PROTO, "", true, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 2);

  // /gifs/trending carries categories and a first trending page; both are ingested.
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_CATEGORIES, "", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_CATEGORIES, "") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_gif_category_count(store) == 8);
  const ccord_gif_category *cat = ccord_store_gif_category_at(store, 0);
  CT_CHECK(cat && strcmp(cat->name, "Applause") == 0);
  CT_CHECK(ccord_store_gif_trending_count(store) == 6);
  const ccord_gif_item *trend = ccord_store_gif_trending_at(store, 0);
  CT_CHECK(trend && strcmp(trend->url, "https://tenor.com/view/wumpus-wave-gif-1001") == 0);

  // /gifs/trending-gifs (bare array) replaces the list; /gifs/search (bare array) fills its own list.
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_TRENDING, "", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_gif_trending_count(store) == 6);
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_SEARCH, "ship it", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_SEARCH, "ship it") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_gif_search_count(store) == 4);
  const ccord_gif_item *hit = ccord_store_gif_search_at(store, 0);
  CT_CHECK(hit && strstr(hit->url, "ship-it") != nullptr && hit->width == 320 && hit->height == 240);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Store parsing of the GIF route shapes: object-wrapped or bare arrays, and an empty trending answer that must
// not wipe the fallback page already carried by /gifs/trending.
static void test_store_gif_payload_shapes(void)
{
  ccord_error err;
  ccord_store *store = ccord_store_create(nullptr, &err);
  CT_CHECK(store != nullptr);
  if (!store) return;

  const char *wrapped =
    "{\"results\":[{\"url\":\"https://tenor.com/view/wrapped-gif-9\","
    "\"src\":\"https://media.discordapp.net/wrapped.gif\",\"width\":64,\"height\":48}]}";
  CT_CHECK(ccord_store_set_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", wrapped, strlen(wrapped), true, &err) ==
           CCORD_OK);
  CT_CHECK(ccord_store_gif_trending_count(store) == 1);
  CT_CHECK(strcmp(ccord_store_gif_trending_at(store, 0)->url, "https://tenor.com/view/wrapped-gif-9") == 0);
  CT_CHECK(ccord_store_gif_trending_at(store, 0)->width == 64 && ccord_store_gif_trending_at(store, 0)->height == 48);

  // An empty trending-gifs answer keeps the rows already shown.
  CT_CHECK(ccord_store_set_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", "[]", 2, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_gif_trending_count(store) == 1);

  // A bare-array /gifs/trending payload is a valid fallback on a store with no trending-gifs rows yet.
  ccord_store *fallback_store = ccord_store_create(nullptr, &err);
  CT_CHECK(fallback_store != nullptr);
  const char *fallback =
    "[{\"url\":\"https://tenor.com/view/fallback-gif-1\",\"gif_src\":\"https://media.discordapp.net/fallback.gif\","
    "\"width\":32,\"height\":24}]";
  CT_CHECK(ccord_store_set_expr_blob(fallback_store, CCORD_EXPR_GIF_CATEGORIES, "", fallback, strlen(fallback), true,
                                     &err) == CCORD_OK);
  CT_CHECK(ccord_store_gif_trending_count(fallback_store) == 1);
  CT_CHECK(strcmp(ccord_store_gif_trending_at(fallback_store, 0)->url, "https://tenor.com/view/fallback-gif-1") == 0);
  ccord_store_destroy(fallback_store);

  ccord_store_destroy(store);
}

static void test_session_m4c_prune_and_automod(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_transport *tr = ccord_mock_transport_create(nullptr, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // 1. AutoMod CRUD via session + mock transport as Admin on guild 2001.
  ccord_admin_request req;
  CT_CHECK(!ccord_store_automod_loaded(store, 2001));
  CT_CHECK(ccord_admin_list_automod_rules(&req, 2001, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_automod_loaded(store, 2001));
  size_t base_rules = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(store, 2001, &base_rules);
  CT_CHECK(rules && base_rules >= 2);

  const char *kw[2] = {"phish.example", "scamlink"};
  ccord_automod_rule draft = {
    .guild_id = 2001,
    .name = "Session Anti-Phish",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
    .enabled = true,
    .keywords = kw,
    .keyword_count = 2,
    .action_block = true,
    .custom_message = "Blocked by AutoMod",
    .action_timeout = true,
    .timeout_seconds = 120,
  };
  CT_CHECK(ccord_admin_create_automod_rule(&req, 2001, &draft, "m4c test", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  size_t cnt = 0;
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  CT_CHECK(rules && cnt == base_rules + 1);

  ccord_id created_id = CCORD_ID_NONE;
  for (size_t i = 0; i < cnt; i++) {
    if (strcmp(rules[i].name, "Session Anti-Phish") == 0) created_id = rules[i].id;
  }
  CT_CHECK(created_id != CCORD_ID_NONE);

  draft.id = created_id;
  draft.name = "Session Anti-Phish v2";
  draft.enabled = false;
  CT_CHECK(ccord_admin_edit_automod_rule(&req, 2001, created_id, &draft, "disable", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  const ccord_automod_rule *edited = nullptr;
  for (size_t i = 0; i < cnt; i++) {
    if (rules[i].id == created_id) edited = &rules[i];
  }
  CT_CHECK(edited && !edited->enabled && strcmp(edited->name, "Session Anti-Phish v2") == 0);

  CT_CHECK(ccord_admin_delete_automod_rule(&req, 2001, created_id, "cleanup", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  bool still_there = false;
  for (size_t i = 0; i < cnt; i++) {
    if (rules[i].id == created_id) still_there = true;
  }
  CT_CHECK(!still_there && cnt == base_rules);

  // 2. Prune preview + execute via session.
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  ccord_prune_info pi = {0};
  CT_CHECK(ccord_store_prune_info(store, 2001, &pi) && pi.has_result && !pi.executed && pi.days == 7 && pi.pruned >= 1);
  int32_t preview_cnt = pi.pruned;

  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 7, true, nullptr, 0, "inactive sweep", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_prune_info(store, 2001, &pi) && pi.has_result && pi.executed && pi.pruned == preview_cnt);

  // 3. Permission & role-hierarchy prechecks (blocked locally before hitting transport).
  ccord_role roles_cfg[4] = {
    {.id = 2001, .guild_id = 2001, .name = "@everyone", .permissions = CCORD_PERM_VIEW_CHANNEL, .position = 0},
    {.id = 2910, .guild_id = 2001, .name = "Helper", .permissions = 0, .position = 1},
    {.id = 2911, .guild_id = 2001, .name = "ModKick", .permissions = CCORD_PERM_KICK_MEMBERS, .position = 5},
    {.id = 2912, .guild_id = 2001, .name = "Senior", .permissions = CCORD_PERM_MANAGE_GUILD, .position = 10},
  };
  ccord_id self_roles[1] = {2911}; // KICK_MEMBERS at position 5, NO MANAGE_GUILD
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles_cfg, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(store, 2001, 1001, self_roles, 1, &err) == CCORD_OK);

  size_t adm0 = ccord_mock_admin_count(tr);
  // AutoMod requires MANAGE_GUILD -> all blocked locally.
  CT_CHECK(ccord_admin_list_automod_rules(&req, 2001, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_create_automod_rule(&req, 2001, &draft, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  CT_CHECK(rules && cnt >= 1);
  CT_CHECK(ccord_admin_edit_automod_rule(&req, 2001, rules[0].id, &rules[0], nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_delete_automod_rule(&req, 2001, rules[0].id, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);

  // Prune include_roles hierarchy: peer role (2911, pos 5) and higher role (2912, pos 10) blocked!
  const ccord_id peer_role[1] = {2911};
  const ccord_id higher_role[1] = {2912};
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, peer_role, 1, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 7, true, higher_role, 1, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0);

  // Lower role (2910, pos 1 < 5) IS allowed with KICK_MEMBERS.
  const ccord_id lower_role[1] = {2910};
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, lower_role, 1, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);

  // Without KICK_MEMBERS: prune preview & execute both blocked locally!
  roles_cfg[2].permissions = 0;
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles_cfg, 4, &err) == CCORD_OK);
  size_t adm1 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 7, true, nullptr, 0, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// M0-5: invite previews are reused for 60 s, identical asks within 15 s are deduplicated, and revoke/accept
// invalidate the cached copy. The proxy for "was the transport called" is the mock's admin-request counter.
static void test_session_m0_invite_preview_cache(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 53, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_admin_request req;
  bool loading = true;
  size_t calls = ccord_mock_admin_count(tr);

  // The first preview reaches the transport and leaves the view loading.
  g_now = 100000;
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 1);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && loading);

  // An identical ask while the first is still in flight is dropped (15 s guard).
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 14999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 1);
  pump_all(s);
  const ccord_invite_preview *prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003 && strcmp(prev->code, "c23-study") == 0);

  // Clearing the view does not evict the cache: 59999 ms after the fetch is still a hit.
  ccord_session_clear_invite_preview(s);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 59999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 1);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003 && strcmp(prev->code, "c23-study") == 0);

  // At exactly 60000 ms the entry expires and is fetched again.
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 60000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 2);
  pump_all(s);

  // A different code is a different key, and the first code still hits its own entry afterwards.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/vulkan-lab", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 60001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 3);
  pump_all(s);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9004);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 60002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 3);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003);

  // The in-flight guard ends exactly at 15000 ms.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/synthwave", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 70000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 4);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 84999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 4);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 85000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 5);
  pump_all(s);

  // A failed preview releases the guard so the next ask retries at once.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/no-such-code", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 90000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 6);
  pump_all(s);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 90001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 7);   // not blocked by the failed attempt
  pump_all(s);

  // Revoking the previewed code invalidates its entry.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/cCordHQ", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 100000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 8);
  pump_all(s);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 2001);
  CT_CHECK(ccord_admin_revoke_invite(&req, 2001, "cCordHQ", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 100001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 9);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/cCordHQ", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 100002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 10);   // cache was invalidated
  pump_all(s);

  // Accepting a previewed code invalidates it too.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/pixel-cinema", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 110000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 11);
  pump_all(s);
  CT_CHECK(ccord_admin_accept_invite(&req, "pixel-cinema", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 110001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 12);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/pixel-cinema", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 110002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 13);   // cache was invalidated

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// M0-3: pending local echo, nonce-matched confirmation, failure marking without draft restore, retry with the
// SAME nonce, and per-nonce correlation with two sends in flight. Counts never move for the echo itself.
static void test_session_m0_nonce_echo_and_retry(void)
{
  ccord_error err;
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
  ccord_id ch = ccord_store_selected_channel(store);
  CT_CHECK(ch != CCORD_ID_NONE && ccord_store_history_loaded(store, ch));
  size_t before = ccord_store_message_count(store, ch);
  const ccord_channel *c0 = ccord_store_channel(store, ch);
  CT_CHECK(c0 && c0->unread_count == 0 && c0->mention_count == 0);
  ccord_id newest_before_echo = c0 ? c0->last_message_id : CCORD_ID_NONE;

  // 1. The pending echo appears at once: local id, pending state, nonce, own author; counts stay put.
  CT_CHECK(ccord_session_send_message(s, ch, "pending text", 12, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 1);
  const ccord_message *pe = ccord_store_message_at(store, ch, before);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  CT_CHECK(pe && (pe->id & (1ULL << 63)) != 0 && pe->author_id == ccord_store_self_id(store));
  CT_CHECK(pe && strcmp(pe->content, "pending text") == 0 && pe->nonce[0] != '\0');
  char nonce1[32] = "";
  ccord_id echo1 = pe ? pe->id : CCORD_ID_NONE;
  if (pe) memcpy(nonce1, pe->nonce, sizeof nonce1);
  const ccord_channel *c1 = ccord_store_channel(store, ch);
  CT_CHECK(c1 && c1->unread_count == 0 && c1->mention_count == 0);
  // The echo sorts after confirmed history (it is the last row) but is never the channel's newest real message.
  CT_CHECK(c1 && c1->last_message_id == newest_before_echo && c1->last_message_id != echo1);
  // Only FAILED echoes are retryable: a pending one is NOT_FOUND.
  CT_CHECK(ccord_session_retry_message(s, ch, echo1, &err) == CCORD_ERR_NOT_FOUND);

  // 2. The nonce-matched MESSAGE_CREATE replaces the echo (no duplicate) and keeps the nonce.
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 1);
  CT_CHECK(ccord_store_find_message(store, ch, echo1) == nullptr);
  const ccord_message *cm = ccord_store_message_at(store, ch, before);
  CT_CHECK(cm && cm->send_state == CCORD_MSG_SEND_CONFIRMED && (cm->id & (1ULL << 63)) == 0);
  CT_CHECK(cm && strcmp(cm->content, "pending text") == 0 && strcmp(cm->nonce, nonce1) == 0);
  ccord_id confirmed_id = cm ? cm->id : CCORD_ID_NONE;
  CT_CHECK(ccord_session_retry_message(s, ch, confirmed_id, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_retry_message(s, ch, (1ULL << 63) | 999999u, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_retry_message(s, 424242, echo1, &err) == CCORD_ERR_NOT_FOUND);

  // 3. Someone else's message counts as unread while reading elsewhere; a refused send fails its echo
  //    without touching those counts and without queueing a draft restore.
  ccord_id guild = ccord_store_selected_guild(store);
  ccord_id other = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, guild, i);
    if (c->kind == CCORD_CHANNEL_TEXT && c->id != ch) { other = c->id; break; }
  }
  CT_CHECK(other != CCORD_ID_NONE);
  ccord_session_set_reading(s, other);
  ccord_id foreign_id = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1002, "foreign", nullptr, &foreign_id, &err) == CCORD_OK);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_channel *c2 = ccord_store_channel(store, ch);
  CT_CHECK(c2 && c2->unread_count == 1 && c2->mention_count == 0);

  ccord_mock_set_refuse_writes(tr, true);
  CT_CHECK(ccord_session_send_message(s, ch, "will fail", 9, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 3);
  const ccord_message *fe = ccord_store_message_at(store, ch, before + 2);
  CT_CHECK(fe && fe->send_state == CCORD_MSG_SEND_PENDING && strcmp(fe->content, "will fail") == 0);
  char nonce2[32] = "";
  ccord_id echo2 = fe ? fe->id : CCORD_ID_NONE;
  if (fe) memcpy(nonce2, fe->nonce, sizeof nonce2);
  CT_CHECK(strcmp(nonce2, nonce1) != 0);   // a fresh nonce per send
  const ccord_channel *c3 = ccord_store_channel(store, ch);
  CT_CHECK(c3 && c3->unread_count == 1 && c3->mention_count == 0);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_message *fm = ccord_store_find_message(store, ch, echo2);
  CT_CHECK(fm && fm->send_state == CCORD_MSG_SEND_FAILED && strcmp(fm->content, "will fail") == 0);
  CT_CHECK(fm && fm->id == echo2 && strcmp(fm->nonce, nonce2) == 0);
  const ccord_channel *c4 = ccord_store_channel(store, ch);
  CT_CHECK(c4 && c4->unread_count == 1 && c4->mention_count == 0);   // the foreign unread survives the failure
  CT_CHECK(c4 && c4->last_message_id == foreign_id);   // the failed echo is not the newest real message
  ccord_action_failure af;
  CT_CHECK(!ccord_session_pop_failure(s, &af));   // the failed bubble holds the draft; no composer restore

  // Mark As Read acknowledges the newest CONFIRMED message, not the failed echo: the foreign unread clears and the
  // echo row survives untouched, ready for retry.
  CT_CHECK(ccord_store_set_channel_counts(store, ch, 1, 1, &err) == CCORD_OK);   // a mention rides the foreign unread
  size_t acked = 0;
  CT_CHECK(ccord_session_mark_read(s, guild, ch, &acked, &err) == CCORD_OK && acked == 1);
  const ccord_channel *c4r = ccord_store_channel(store, ch);
  CT_CHECK(c4r && c4r->unread_count == 0 && c4r->mention_count == 0);
  CT_CHECK(c4r && c4r->last_read_id == foreign_id);
  CT_CHECK(c4r && c4r->last_message_id == foreign_id);
  fm = ccord_store_find_message(store, ch, echo2);
  CT_CHECK(fm && fm->send_state == CCORD_MSG_SEND_FAILED && strcmp(fm->content, "will fail") == 0);

  // 4. Retry resubmits the SAME nonce, re-pends the echo, and the confirmation replaces it.
  ccord_mock_set_refuse_writes(tr, false);
  CT_CHECK(ccord_session_retry_message(s, ch, echo2, &err) == CCORD_OK);
  const ccord_message *re = ccord_store_find_message(store, ch, echo2);
  CT_CHECK(re && re->send_state == CCORD_MSG_SEND_PENDING && strcmp(re->nonce, nonce2) == 0);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 3);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_find_message(store, ch, echo2) == nullptr);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 3);
  const ccord_message *rc = ccord_store_message_at(store, ch, before + 2);
  CT_CHECK(rc && rc->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(rc->content, "will fail") == 0);
  CT_CHECK(rc && strcmp(rc->nonce, nonce2) == 0);   // the original nonce went out again, end to end
  const ccord_channel *c5 = ccord_store_channel(store, ch);
  CT_CHECK(c5 && c5->unread_count == 0);   // own confirmed message marks the channel read (existing semantics)

  // 5. Two sends in flight: each refusal fails exactly its own echo, and retrying one leaves the other failed.
  ccord_mock_set_refuse_writes(tr, true);
  CT_CHECK(ccord_session_send_message(s, ch, "first", 5, &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_message(s, ch, "second", 6, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 5);
  const ccord_message *e1 = ccord_store_message_at(store, ch, before + 3);
  const ccord_message *e2 = ccord_store_message_at(store, ch, before + 4);
  CT_CHECK(e1 && e2 && strcmp(e1->content, "first") == 0 && strcmp(e2->content, "second") == 0);
  char n_second[32] = "";
  ccord_id id_first = e1 ? e1->id : CCORD_ID_NONE, id_second = e2 ? e2->id : CCORD_ID_NONE;
  if (e2) memcpy(n_second, e2->nonce, sizeof n_second);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_message *f1 = ccord_store_find_message(store, ch, id_first);
  const ccord_message *f2 = ccord_store_find_message(store, ch, id_second);
  CT_CHECK(f1 && f1->send_state == CCORD_MSG_SEND_FAILED && strcmp(f1->content, "first") == 0);
  CT_CHECK(f2 && f2->send_state == CCORD_MSG_SEND_FAILED && strcmp(f2->content, "second") == 0);
  CT_CHECK(!ccord_session_pop_failure(s, &af));
  ccord_mock_set_refuse_writes(tr, false);
  CT_CHECK(ccord_session_retry_message(s, ch, id_second, &err) == CCORD_OK);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_find_message(store, ch, id_second) == nullptr);
  f1 = ccord_store_find_message(store, ch, id_first);
  CT_CHECK(f1 && f1->send_state == CCORD_MSG_SEND_FAILED);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 5);
  // The confirmed "second" takes its real-id place; the failed "first" echo still sorts last (local id).
  const ccord_message *lastm = ccord_store_message_at(store, ch, before + 3);
  CT_CHECK(lastm && strcmp(lastm->content, "second") == 0 && strcmp(lastm->nonce, n_second) == 0);
  CT_CHECK(lastm && lastm->send_state == CCORD_MSG_SEND_CONFIRMED);
  const ccord_message *tailm = ccord_store_message_at(store, ch, before + 4);
  CT_CHECK(tailm && tailm->id == id_first && tailm->send_state == CCORD_MSG_SEND_FAILED);

  // 6. Slowmode refusals carry the nonce too: in 3315 (5 s slowmode, self is a plain Member) the second send
  // inside the cooldown fails exactly its own echo, with no composer restore.
  CT_CHECK(ccord_session_send_message(s, 3315, "slow one", 8, &err) == CCORD_OK);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_send_message(s, 3315, "too fast", 8, &err) == CCORD_OK);
  const ccord_message *se = ccord_store_message_at(store, 3315, ccord_store_message_count(store, 3315) - 1u);
  CT_CHECK(se && se->send_state == CCORD_MSG_SEND_PENDING);
  char slow_nonce[32] = "";
  ccord_id slow_echo = se ? se->id : CCORD_ID_NONE;
  if (se) memcpy(slow_nonce, se->nonce, sizeof slow_nonce);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_message *sf = ccord_store_find_message(store, 3315, slow_echo);
  CT_CHECK(sf && sf->send_state == CCORD_MSG_SEND_FAILED && strcmp(sf->nonce, slow_nonce) == 0);
  CT_CHECK(!ccord_session_pop_failure(s, &af));

  // 7. Store-level bookkeeping: an unconfirmed bit-63 echo may sit in history, but it never becomes the channel's
  //    newest message, a Mark As Read target, an unread counted by apply_read, or an unread forgotten by a delete.
  const ccord_id echo_chan = 880001, echo_only_chan = 880002, echo_id = (1ULL << 63) | 7u;
  const ccord_id echo_only_id = (1ULL << 63) | 8u, echo_only_last = (1ULL << 63) | 500u;
  ccord_channel book = {.id = echo_chan, .guild_id = guild, .kind = CCORD_CHANNEL_TEXT, .name = "book", .topic = "",
                        .position = 90};
  ccord_channel book2 = {.id = echo_only_chan, .guild_id = guild, .kind = CCORD_CHANNEL_TEXT, .name = "book2", .topic = "",
                         .position = 91, .last_message_id = echo_only_last};
  CT_CHECK(ccord_store_upsert_channel(store, &book, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(store, &book2, &err) == CCORD_OK);
  ccord_message book_hist[2] = {
    {.id = 8001, .channel_id = echo_chan, .author_id = 1002, .timestamp_ms = 1, .content = "old"},
    {.id = 8002, .channel_id = echo_chan, .author_id = 1002, .timestamp_ms = 2, .content = "new"},
  };
  CT_CHECK(ccord_store_set_history(store, echo_chan, book_hist, 2, &err) == CCORD_OK);
  ccord_message book_echo = {.id = echo_id, .channel_id = echo_chan, .author_id = ccord_store_self_id(store),
                             .timestamp_ms = 3, .content = "pending", .send_state = CCORD_MSG_SEND_PENDING};
  memcpy(book_echo.nonce, "book-nonce", 11);
  CT_CHECK(ccord_store_append_message(store, &book_echo, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, echo_chan) == 3);
  const ccord_message *book_tail = ccord_store_message_at(store, echo_chan, 2);
  CT_CHECK(book_tail && book_tail->id == echo_id);   // it renders after confirmed history ...
  const ccord_channel *bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->last_message_id == 8002);       // ... but is never the newest real message
  // A history re-fetch merges around the echo and must not pick the bit-63 local id as newest either.
  CT_CHECK(ccord_store_set_history(store, echo_chan, book_hist, 2, &err) == CCORD_OK);
  bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->last_message_id == 8002);
  CT_CHECK(ccord_store_set_channel_counts(store, echo_chan, 5, 2, &err) == CCORD_OK);
  ccord_read_target bt[4];
  CT_CHECK(ccord_store_read_targets(store, guild, echo_chan, 0, bt, 4) == 1 && bt[0].message_id == 8002);
  CT_CHECK(ccord_store_apply_read(store, echo_chan, 8001, UINT32_MAX, &err) == CCORD_OK);
  bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->unread_count == 1 && bc->mention_count == 1);   // only confirmed 8002 is unread
  CT_CHECK(ccord_store_set_channel_counts(store, echo_chan, 1, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_messages_bulk(store, echo_chan, &echo_id, 1, &err) == CCORD_OK);
  bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->unread_count == 1 && bc->mention_count == 1);   // deleting the echo forgets no unread
  CT_CHECK(ccord_store_message_count(store, echo_chan) == 2);

  // A channel whose stored pointer is already a bit-63 echo falls back to the newest loaded confirmed message.
  ccord_message book2_hist[2] = {
    {.id = 8101, .channel_id = echo_only_chan, .author_id = 1002, .timestamp_ms = 4, .content = "older"},
    {.id = 8102, .channel_id = echo_only_chan, .author_id = 1002, .timestamp_ms = 5, .content = "newest"},
  };
  CT_CHECK(ccord_store_set_history(store, echo_only_chan, book2_hist, 2, &err) == CCORD_OK);
  ccord_message book2_echo = {.id = echo_only_id, .channel_id = echo_only_chan, .author_id = ccord_store_self_id(store),
                              .timestamp_ms = 6, .content = "pending2", .send_state = CCORD_MSG_SEND_PENDING};
  memcpy(book2_echo.nonce, "book-nonce2", 12);
  CT_CHECK(ccord_store_append_message(store, &book2_echo, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(store, echo_only_chan, 1, 0, &err) == CCORD_OK);
  const ccord_channel *b2 = ccord_store_channel(store, echo_only_chan);
  CT_CHECK(b2 && b2->last_message_id == echo_only_last);   // still the stale bit-63 pointer
  CT_CHECK(ccord_store_read_targets(store, guild, echo_only_chan, 0, bt, 4) == 1 && bt[0].message_id == 8102);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// M0-3 edge paths: hook-less fallback, foreign nonce, synchronous refusal rollback, the legacy failure ring when
// no echo matches, and the local-echo guards on ack/edit/delete/reaction.
static void test_session_m0_nonce_fake_edges(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ"}};
  ccord_channel chans[2] = {{.id = 22, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                             .name = "general", .topic = ""},
                            {.id = 23, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                             .name = "elsewhere", .topic = ""}};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 2};
  CT_CHECK(ccord_store_set_snapshot(store, &snap, &err) == CCORD_OK);
  ccord_message hist[1] = {{.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "a"}};
  CT_CHECK(ccord_store_set_history(store, 22, hist, 1, &err) == CCORD_OK);

  // 1. Hook-less transport: the wrapper falls back to send_message; the session keeps the legacy no-echo path.
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) { free(f); return; }
  CT_CHECK(ccord_transport_send_message_with_nonce(&f->base, 22, "direct", 6, "n-direct", 8, &err) == CCORD_OK);
  CT_CHECK(f->send_count == 1 && f->nonce_send_count == 0);   // the null hook fell back to the legacy send
  CT_CHECK(strcmp(f->last_send_text, "direct") == 0);
  CT_CHECK(ccord_session_send_message(s, 22, "legacy", 6, &err) == CCORD_OK);
  CT_CHECK(f->send_count == 2 && f->nonce_send_count == 0);
  CT_CHECK(ccord_store_message_count(store, 22) == 1);   // no echo without the hook
  // A legacy SEND failure (no nonce) still restores the draft through the failure ring.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND, .channel_id = 22,
                                    .status = CCORD_ERR_PERMISSION, .text = "no", .draft = "legacy", .draft_len = 6};
  CT_CHECK(ccord_session_pump(s) == 1);
  ccord_action_failure af;
  CT_CHECK(ccord_session_pop_failure(s, &af));
  CT_CHECK(af.action == CCORD_ACTION_SEND && af.draft && af.draft_len == 6);
  ccord_action_failure_free(&af);
  ccord_session_destroy(s);

  // 2. Nonce hook: a foreign nonce appends beside the pending echo; the matching nonce then confirms it.
  f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_nonce_vt;
  s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) { free(f); return; }
  CT_CHECK(ccord_session_send_message(s, 22, "mine", 4, &err) == CCORD_OK);
  CT_CHECK(f->nonce_send_count == 1 && f->send_count == 0);
  CT_CHECK(ccord_store_message_count(store, 22) == 2);
  const ccord_message *pe = ccord_store_message_at(store, 22, 1);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  char my_nonce[32] = "";
  ccord_id my_echo = pe ? pe->id : CCORD_ID_NONE;
  if (pe) memcpy(my_nonce, pe->nonce, sizeof my_nonce);
  CT_CHECK(strcmp(f->last_send_nonce, my_nonce) == 0);   // the transport received the echo's nonce
  ccord_message foreign = {.id = 300, .channel_id = 22, .author_id = 2, .timestamp_ms = 5, .content = "foreign"};
  memcpy(foreign.nonce, "not-mine", 9);
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .channel_id = 22,
                                    .messages = &foreign, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_message_count(store, 22) == 3);   // foreign nonce appended, echo untouched
  CT_CHECK(ccord_store_find_message(store, 22, my_echo) != nullptr);
  CT_CHECK(ccord_store_find_message(store, 22, 300) != nullptr);
  ccord_message confirm = {.id = 400, .channel_id = 22, .author_id = 1, .timestamp_ms = 6, .content = "mine"};
  memcpy(confirm.nonce, my_nonce, sizeof confirm.nonce);
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .channel_id = 22,
                                    .messages = &confirm, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_message_count(store, 22) == 3);
  CT_CHECK(ccord_store_find_message(store, 22, my_echo) == nullptr);
  const ccord_message *cm = ccord_store_find_message(store, 22, 400);
  CT_CHECK(cm && cm->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(cm->nonce, my_nonce) == 0);

  // 3. Synchronous refusal: the echo is rolled back and the error returned (the caller keeps the text). The
  // rollback must not eat an unrelated unread: while reading elsewhere, a foreign message made 22 unread first.
  ccord_session_set_reading(s, 23);
  ccord_message foreign2 = {.id = 500, .channel_id = 22, .author_id = 2, .timestamp_ms = 7, .content = "unread maker"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .channel_id = 22,
                                    .messages = &foreign2, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  const ccord_channel *uc = ccord_store_channel(store, 22);
  CT_CHECK(uc && uc->unread_count == 1);
  f->nonce_send_fail = true;
  CT_CHECK(ccord_session_send_message(s, 22, "rolled back", 11, &err) == CCORD_ERR_IO);
  CT_CHECK(ccord_store_message_count(store, 22) == 4);   // no leftover echo
  uc = ccord_store_channel(store, 22);
  CT_CHECK(uc && uc->unread_count == 1);   // the rollback restored the counts the delete disturbed
  CT_CHECK(f->nonce_send_count == 2);
  f->nonce_send_fail = false;

  // 4. A refusal whose nonce matches no echo (history replaced meanwhile) keeps the legacy draft ring.
  CT_CHECK(ccord_session_send_message(s, 22, "orphan", 6, &err) == CCORD_OK);
  pe = ccord_store_message_at(store, 22, ccord_store_message_count(store, 22) - 1u);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  char orphan_nonce[32] = "";
  if (pe) memcpy(orphan_nonce, pe->nonce, sizeof orphan_nonce);
  ccord_id orphan_echo = pe ? pe->id : CCORD_ID_NONE;
  CT_CHECK(ccord_store_delete_message(store, 22, orphan_echo, &err) == CCORD_OK);   // e.g. history reloaded without it
  f->ev[f->n] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND, .channel_id = 22,
                                  .status = CCORD_ERR_PERMISSION, .text = "no", .draft = "orphan", .draft_len = 6};
  memcpy(f->ev[f->n].action_nonce, orphan_nonce, sizeof orphan_nonce);
  f->n++;
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_failure(s, &af));
  CT_CHECK(af.draft && af.draft_len == 6 && memcmp(af.draft, "orphan", 6) == 0);
  ccord_action_failure_free(&af);

  // 5. Local echoes are not server messages: ack is a no-op, edit/delete/reaction refuse.
  CT_CHECK(ccord_session_send_message(s, 22, "guarded", 7, &err) == CCORD_OK);
  pe = ccord_store_message_at(store, 22, ccord_store_message_count(store, 22) - 1u);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  ccord_id gid = pe ? pe->id : CCORD_ID_NONE;
  CT_CHECK(ccord_session_ack_message(s, 22, gid, &err) == CCORD_OK);   // no transport ack (hook is null anyway)
  CT_CHECK(ccord_session_edit_message(s, 22, gid, "x", 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_delete_message(s, 22, gid, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_toggle_reaction(s, 22, gid, "\xf0\x9f\x91\x8d", 4, &err) == CCORD_ERR_INVALID);

  // 6. A retry refused synchronously stays failed with its draft (the next attempt can try again).
  char gnonce[32] = "";
  memcpy(gnonce, pe->nonce, sizeof gnonce);
  f->ev[f->n] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND, .channel_id = 22,
                                  .status = CCORD_ERR_PERMISSION, .text = "no", .draft = "guarded", .draft_len = 7};
  memcpy(f->ev[f->n].action_nonce, gnonce, sizeof gnonce);
  f->n++;
  CT_CHECK(ccord_session_pump(s) == 1);
  const ccord_message *gf = ccord_store_find_message(store, 22, gid);
  CT_CHECK(gf && gf->send_state == CCORD_MSG_SEND_FAILED);
  f->nonce_send_fail = true;
  CT_CHECK(ccord_session_retry_message(s, 22, gid, &err) == CCORD_ERR_IO);
  gf = ccord_store_find_message(store, 22, gid);
  CT_CHECK(gf && gf->send_state == CCORD_MSG_SEND_FAILED && strcmp(gf->content, "guarded") == 0);
  f->nonce_send_fail = false;

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

typedef struct fake_stream_transport {
  ccord_transport base;
  ccord_net_event ev[32];
  size_t n, i;
  uint32_t watch_count, stop_watch_count;
  ccord_id last_watch_guild, last_watch_channel, last_watch_user;
  bool has_rx_frame;
  uint8_t rx_frame[16];
  size_t rx_frame_len;
  bool rx_frame_key;
  int16_t rx_pcm[16];
  size_t rx_pcm_len;
} fake_stream_transport;

static bool fake_stream_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  if (f->i >= f->n) return false;
  *out = f->ev[f->i++];
  return true;
}

static ccord_status fake_stream_watch(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                      ccord_error *err)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  f->watch_count++;
  f->last_watch_guild = guild_id;
  f->last_watch_channel = channel_id;
  f->last_watch_user = user_id;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_stream_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                              ccord_id user_id, ccord_error *err)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  (void)guild_id;
  (void)channel_id;
  (void)user_id;
  f->stop_watch_count++;
  f->has_rx_frame = false;
  f->rx_pcm_len = 0;
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool fake_stream_poll_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  if (!f->has_rx_frame || buf_cap < f->rx_frame_len) return false;
  memcpy(buf, f->rx_frame, f->rx_frame_len);
  *out_len = f->rx_frame_len;
  if (out_ts90k) *out_ts90k = 9000u;
  if (out_keyframe) *out_keyframe = f->rx_frame_key;
  f->has_rx_frame = false;
  return true;
}

static size_t fake_stream_poll_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  if (f->rx_pcm_len == 0 || cap_samples == 0) return 0;
  size_t take = f->rx_pcm_len < cap_samples ? f->rx_pcm_len : cap_samples;
  memcpy(out_pcm, f->rx_pcm, take * sizeof *out_pcm);
  if (take < f->rx_pcm_len) {
    memmove(f->rx_pcm, f->rx_pcm + take, (f->rx_pcm_len - take) * sizeof *f->rx_pcm);
  }
  f->rx_pcm_len -= take;
  return take;
}

static ccord_status fake_stream_noop_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)limit;
  ccord_error_clear(err);
  return CCORD_OK;
}

static const ccord_transport_vtable fake_stream_vt = {
  .name = "fake-stream",
  .connect = fake_connect,
  .disconnect = fake_disconnect,
  .request_history = fake_stream_noop_history,
  .send_message = fake_send,
  .poll = fake_stream_poll,
  .destroy = fake_destroy,
  .watch_stream = fake_stream_watch,
  .stop_watching = fake_stream_stop_watching,
  .poll_stream_frame = fake_stream_poll_frame,
  .poll_stream_pcm = fake_stream_poll_pcm,
};

static void test_session_switch_concurrent_streams(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_stream_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) return;
  f->base.vt = &fake_stream_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_guild g = {.id = 10, .name = "G"};
  ccord_channel chs[2] = {
    {.id = 20, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 21, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "StageVoice", .topic = ""},
  };
  ccord_user users[3] = {
    {.id = 1, .name = "me", .status_text = ""},
    {.id = 2, .name = "alice", .status_text = ""},
    {.id = 3, .name = "bob", .status_text = ""},
  };
  ccord_voice_state voices[3] = {
    {.guild_id = 10, .channel_id = 21, .user_id = 1},
    {.guild_id = 10, .channel_id = 21, .user_id = 2, .self_stream = true},
    {.guild_id = 10, .channel_id = 21, .user_id = 3, .self_stream = true},
  };
  ccord_snapshot snap = {
    .self_id = 1,
    .guilds = &g, .guild_count = 1,
    .channels = chs, .channel_count = 2,
    .users = users, .user_count = 3,
  };
  ccord_stream_signal create_a = {
    .kind = CCORD_STREAM_SIG_CREATE, .stream_key = "guild:10:21:2",
    .guild_id = 10, .channel_id = 21, .user_id = 2, .owner_id = 2, .viewer_count = 3,
  };
  ccord_stream_signal create_b = {
    .kind = CCORD_STREAM_SIG_CREATE, .stream_key = "guild:10:21:3",
    .guild_id = 10, .channel_id = 21, .user_id = 3, .owner_id = 3, .paused = true, .viewer_count = 5,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTED};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &voices[0]};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &voices[1]};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &voices[2]};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &create_a};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &create_b};
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) == 7);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);
  CT_CHECK(ccord_store_stream_find(store, 21, 2) != nullptr);
  CT_CHECK(ccord_store_stream_find(store, 21, 3) != nullptr);

  // Watch stream A (user 2); re-watching A is an idempotent no-op that preserves queued RX media.
  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 2, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 1 && f->stop_watch_count == 0 && f->last_watch_user == 2);
  const ccord_watched_stream *ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 2 && !ws->paused);

  memcpy(f->rx_frame, "\x00\x00\x00\x01\x65\xaa", 6);
  f->rx_frame_len = 6;
  f->rx_frame_key = true;
  f->has_rx_frame = true;
  f->rx_pcm[0] = 1234;
  f->rx_pcm[1] = -1234;
  f->rx_pcm_len = 2;

  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 2, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 1 && f->has_rx_frame && f->rx_pcm_len == 2);

  // Switch directly from stream A (user 2) to stream B (user 3):
  // - drains stale RX media from A
  // - transitions watched target + paused state cleanly to B without calling stop_watching
  // - preserves BOTH A and B in ccord_store even when Gateway echoes viewer-unsubscribe STREAM_DELETE for A
  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 3, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 2 && f->stop_watch_count == 0 && f->last_watch_user == 3);
  CT_CHECK(!f->has_rx_frame && f->rx_pcm_len == 0);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 3 && ws->paused);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);

  ccord_stream_signal unsub_a = {
    .kind = CCORD_STREAM_SIG_DELETE, .stream_key = "guild:10:21:2",
    .guild_id = 10, .channel_id = 21, .user_id = 2, .owner_id = 2,
    .deleted = true, .reason = "stream_stopped",
  };
  ccord_stream_signal ssu_b = {
    .kind = CCORD_STREAM_SIG_SERVER_UPDATE, .stream_key = "guild:10:21:3",
    .guild_id = 10, .channel_id = 21, .user_id = 3, .owner_id = 3,
    .server_update = true, .token = "REDACTED_TEST_TOKEN", .endpoint = "wss://media.example",
  };
  ccord_stream_signal upd_a = {
    .kind = CCORD_STREAM_SIG_UPDATE, .stream_key = "guild:10:21:2",
    .guild_id = 10, .channel_id = 21, .user_id = 2, .owner_id = 2,
    .paused = true, .viewer_count = 7,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &unsub_a};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &ssu_b};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &upd_a};
  CT_CHECK(pump_all(s) == 3);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);
  CT_CHECK(ccord_store_stream_find(store, 21, 2) != nullptr);
  CT_CHECK(ccord_store_stream_find(store, 21, 3) != nullptr);
  CT_CHECK(ccord_store_stream_viewer_count(store, 21, 2) == 7);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 3 && ws->paused);

  // Switch back from B to A: both remain in ccord_store and A's updated pause state is reflected.
  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 2, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 3 && f->stop_watch_count == 0 && f->last_watch_user == 2);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 2 && ws->paused);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);

  // Broadcaster B actually stops streaming (self_stream=false + STREAM_DELETE): B is removed, A stays watched.
  ccord_voice_state b_stopped = {.guild_id = 10, .channel_id = 21, .user_id = 3, .self_stream = false};
  ccord_stream_signal del_b = {
    .kind = CCORD_STREAM_SIG_DELETE, .stream_key = "guild:10:21:3",
    .guild_id = 10, .channel_id = 21, .user_id = 3, .owner_id = 3,
    .deleted = true, .reason = "stream_stopped",
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &b_stopped};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &del_b};
  CT_CHECK(pump_all(s) == 2);
  CT_CHECK(ccord_store_stream_list_count(store) == 1);
  CT_CHECK(ccord_store_stream_find(store, 21, 2) != nullptr);
  CT_CHECK(ccord_store_stream_find(store, 21, 3) == nullptr);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 2);

  // Explicit stop_watching tears down the active viewer state.
  CT_CHECK(ccord_session_stop_watching(s, &err) == CCORD_OK);
  CT_CHECK(f->stop_watch_count == 1);
  CT_CHECK(ccord_session_watched_stream(s) == nullptr);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

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
  .request_history = fake_stream_noop_history,
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

static bool bytes_contain(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t needle_len)
{
  if (needle_len == 0) return true;
  if (!hay || hay_len < needle_len) return false;
  for (size_t i = 0; i + needle_len <= hay_len; i++) {
    if (memcmp(hay + i, needle, needle_len) == 0) return true;
  }
  return false;
}

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
  test_session_notify_settings();
  test_session_guild_default_notifications();
  test_session_structured_mentions();
  test_session_mark_read();
  test_session_read_position();
  test_session_author_sighting_keeps_member();
  test_session_member_list_paging();
  test_session_polls_components_and_modals();
  test_session_ephemeral_slash_reply_and_uploads();
  test_session_search_pins_threads_and_reactions();
  test_session_m0_invite_preview_cache();
  test_session_m3b_folders_reorder_invites_and_discovery();
  test_session_m3c_group_dms_requests_profiles_and_custom_status();
  test_session_m4a_guild_settings_safety_vanity_and_ownership();
  test_session_m4b_expressions();
  test_session_expression_data_failure();
  test_session_expression_settings_512_url();
  test_session_expression_catalog_and_favorites();
  test_store_gif_payload_shapes();
  test_session_m4c_prune_and_automod();
  test_session_m0_nonce_echo_and_retry();
  test_session_m0_nonce_fake_edges();
  test_session_switch_concurrent_streams();
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
  return CT_RESULT("test_session");
}
