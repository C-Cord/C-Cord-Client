// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
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

[[maybe_unused]] static uint64_t g_now = 1000;

[[maybe_unused]] static uint64_t fake_now(void *ud)
{
  (void)ud;
  return g_now;
}

[[maybe_unused]] static size_t pump_all(ccord_session *s)
{
  size_t total = 0;
  for (int i = 0; i < 8; i++) {
    size_t n = ccord_session_pump(s);
    if (n == 0) break;
    total += n;
  }
  return total;
}


typedef struct fake_transport {
  ccord_transport base;
  ccord_net_event ev[64];
  size_t n, i;
  ccord_channel channel;
  ccord_id last_requested_history_channel;
  uint32_t history_request_count;
  ccord_id requested_history_channels[16];
  size_t requested_history_recorded;
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

[[maybe_unused]] static ccord_status fake_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}

[[maybe_unused]] static void fake_disconnect(ccord_transport *t) { (void)t; }

[[maybe_unused]] static ccord_status fake_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->last_requested_history_channel = channel_id;
  if (f->requested_history_recorded < 16u) {
    f->requested_history_channels[f->requested_history_recorded++] = channel_id;
  }
  f->history_request_count++;
  (void)limit;
  ccord_error_clear(err);
  return CCORD_OK;
}

[[maybe_unused]] static ccord_status fake_send(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->send_count++;
  f->last_send_channel = channel_id;
  memcpy(f->last_send_text, utf8, len);
  f->last_send_text[len] = '\0';
  ccord_error_clear(err);
  return CCORD_OK;
}

[[maybe_unused]] static ccord_status fake_send_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
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

[[maybe_unused]] static bool fake_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_transport *f = (fake_transport *)t;
  if (f->i >= f->n) return false;
  *out = f->ev[f->i++];
  return true;
}

[[maybe_unused]] static ccord_status fake_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id, uint32_t limit,
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

[[maybe_unused]] static ccord_status fake_select_guild(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->last_selected_guild = guild_id;
  ccord_error_clear(err);
  return CCORD_OK;
}

[[maybe_unused]] static ccord_status fake_select_channel(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  f->last_selected_guild = guild_id;
  f->last_selected_channel_guild = guild_id;
  f->last_selected_channel_id = channel_id;
  f->select_channel_count++;
  ccord_error_clear(err);
  return CCORD_OK;
}

[[maybe_unused]] static ccord_status fake_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err)
{
  fake_transport *f = (fake_transport *)t;
  (void)user_id;
  f->profile_request_count++;
  ccord_error_clear(err);
  return CCORD_OK;
}

[[maybe_unused]] static ccord_status fake_request_members(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
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

[[maybe_unused]] static void fake_destroy(ccord_transport *t)
{
  free(t);
}

[[maybe_unused]] static const ccord_transport_vtable fake_vt = {
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
[[maybe_unused]] static const ccord_transport_vtable fake_nonce_vt = {
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

[[maybe_unused]] static size_t drain_notifications(ccord_session *s, ccord_id *channels, size_t cap)
{
  size_t n = 0;
  ccord_notification note;
  while (ccord_session_pop_notification(s, &note)) {
    if (n < cap) channels[n] = note.channel_id;
    n++;
  }
  return n;
}

[[maybe_unused]] static bool bytes_contain(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t needle_len)
{
  if (needle_len == 0) return true;
  if (!hay || hay_len < needle_len) return false;
  for (size_t i = 0; i + needle_len <= hay_len; i++) {
    if (memcmp(hay + i, needle, needle_len) == 0) return true;
  }
  return false;
}

