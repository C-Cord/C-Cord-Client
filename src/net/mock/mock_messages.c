// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

ccord_message mk_store_new_rich(mock *m, size_t ci, ccord_id author, const char *text, size_t len,
                                    const ccord_message_rich *rich, ccord_error *err, ccord_status *st)
{
  ccord_message msg = {.id = m->next_msg_id, .channel_id = m->channels[ci].id, .author_id = author, .content = nullptr};
  msg.timestamp_ms = (m->cfg.base_time_ms ? m->cfg.base_time_ms : MOCK_DEFAULT_BASE_MS) +
                     (int64_t)(mk_now_of(m) - m->start_ms);
  msg.content = ccord_strndup(text, len);
  if (!msg.content) {
    *st = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    return msg;
  }
  if (rich) {
    msg.rich = ccord_message_rich_clone(rich, err);
    if (!msg.rich) {
      mk_free_content(&msg.content);
      *st = err && err->code != CCORD_OK ? err->code : CCORD_ERR_NOMEM;
      return msg;
    }
  }
  if (ccord_message_is_ephemeral(&msg)) {
    mk_retire_content(m, msg.content);
    mk_retire_rich(m, msg.rich);
    *st = CCORD_OK;
    m->next_msg_id++;
    return msg;
  }
  *st = mk_chan_append(m, ci, msg, err);
  if (*st != CCORD_OK) return msg; // mk_chan_append already freed the content
  m->next_msg_id++;
  return msg;
}

ccord_message mk_store_new(mock *m, size_t ci, ccord_id author, const char *text, size_t len, ccord_error *err, ccord_status *st)
{
  return mk_store_new_rich(m, ci, author, text, len, nullptr, err, st);
}

ccord_status mk_queue_message_create(mock *m, ccord_message msg, uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_MESSAGE_CREATE, due);
  it.msgs = ccord_alloc_array(1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.msgs[0] = msg;
  it.ev.messages = it.msgs;
  it.ev.message_count = 1;
  it.ev.channel_id = msg.channel_id;
  it.ev.guild_id = CCORD_ID_NONE;
  return mk_push_item(m, &it, err);
}

ccord_status mk_queue_message_update(mock *m, ccord_message msg, uint64_t due, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_MESSAGE_UPDATE, due);
  it.msgs = ccord_alloc_array(1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.msgs[0] = msg;
  it.ev.messages = it.msgs;
  it.ev.message_count = 1;
  it.ev.channel_id = msg.channel_id;
  return mk_push_item(m, &it, err);
}


ccord_status mock_request_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_HISTORY]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  if (limit == 0) limit = MOCK_HISTORY_DEFAULT;
  size_t total = m->chn[ci];
  size_t take = limit < total ? limit : total;
  size_t from = total - take;

  mock_item it = mk_make_item(CCORD_NET_HISTORY, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(take ? take : 1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (take) memcpy(it.msgs, m->chmsgs[ci] + from, take * sizeof *it.msgs);
  it.ev.messages = it.msgs;
  it.ev.message_count = take;
  return mk_push_item(m, &it, err);
}

// refuse_writes: the call succeeded but Discord says no. Reported on a later poll, with the user's text.
// The nonce (M0-3 sends) rides along so the session can fail exactly the pending echo this refusal belongs to.
ccord_status mk_queue_action_refused(mock *m, ccord_action_kind action, ccord_id channel_id, ccord_id message_id,
                                         const char *utf8, size_t len, const char *nonce, size_t nonce_len,
                                         ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, mk_now_of(m) + m->cfg.latency_ms);
  it.draft = ccord_strndup(utf8, len);
  if (!it.draft) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  it.ev.action = action;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.status = CCORD_ERR_PERMISSION;
  it.ev.text = "You don't have permission to do that.";
  it.ev.draft = it.draft;
  it.ev.draft_len = len;
  if (nonce && nonce_len > 0 && nonce_len < sizeof it.ev.action_nonce) {
    memcpy(it.ev.action_nonce, nonce, nonce_len);
    it.ev.action_nonce[nonce_len] = '\0';
  }
  return mk_push_item(m, &it, err);
}


static bool mock_slowmode_refused(mock *m, size_t ci, ccord_action_kind action, ccord_id message_id,
                                  const char *utf8, size_t len, const char *nonce, size_t nonce_len,
                                  ccord_status *out, ccord_error *err)
{
  const ccord_channel *ch = &m->channels[ci];
  uint32_t rl = ch->rate_limit_per_user;
  if (rl == 0 || ch->guild_id == CCORD_DM_GUILD_ID || ch->guild_id == CCORD_ID_NONE) return false;
  uint64_t perms = mock_self_perms(m, ch->guild_id);
  if ((perms & (CCORD_PERM_BYPASS_SLOWMODE | CCORD_PERM_MANAGE_CHANNELS | CCORD_PERM_MANAGE_MESSAGES)) != 0) {
    return false;
  }
  uint64_t now = mk_now_of(m);
  for (size_t i = 0; i < sizeof m->slowmode_sent / sizeof m->slowmode_sent[0]; i++) {
    if (m->slowmode_sent[i].has_sent && m->slowmode_sent[i].channel_id == ch->id) {
      if (now < m->slowmode_sent[i].sent_ms + (uint64_t)rl * 1000ull) {
        mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, now + m->cfg.latency_ms);
        it.draft = ccord_strndup(utf8 ? utf8 : "", len);
        if (!it.draft) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.ev.action = action;
        it.ev.channel_id = ch->id;
        it.ev.message_id = message_id;
        it.ev.status = CCORD_ERR_PERMISSION;
        it.ev.text = "This channel is in slowmode. Wait a moment before sending again.";
        it.ev.draft = it.draft;
        it.ev.draft_len = len;
        if (nonce && nonce_len > 0 && nonce_len < sizeof it.ev.action_nonce) {
          memcpy(it.ev.action_nonce, nonce, nonce_len);
          it.ev.action_nonce[nonce_len] = '\0';
        }
        *out = mk_push_item(m, &it, err);
        return true;
      }
      break;
    }
  }
  return false;
}

static void mock_slowmode_mark_sent(mock *m, ccord_id channel_id)
{
  size_t slot = 0;
  for (size_t i = 0; i < sizeof m->slowmode_sent / sizeof m->slowmode_sent[0]; i++) {
    if (m->slowmode_sent[i].has_sent && m->slowmode_sent[i].channel_id == channel_id) {
      m->slowmode_sent[i].sent_ms = mk_now_of(m);
      return;
    }
    if (!m->slowmode_sent[i].has_sent && slot == 0 && !m->slowmode_sent[0].has_sent) slot = i;
    else if (!m->slowmode_sent[i].has_sent && m->slowmode_sent[slot].has_sent) slot = i;
  }
  m->slowmode_sent[slot].channel_id = channel_id;
  m->slowmode_sent[slot].sent_ms = mk_now_of(m);
  m->slowmode_sent[slot].has_sent = true;
}

static ccord_status mock_send_message_impl(mock *m, ccord_id channel_id, const char *utf8, size_t len,
                                           const char *nonce, size_t nonce_len, ccord_error *err)
{
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!ccord_utf8_valid_text(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->cfg.refuse_writes) {
    return mk_queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, utf8, len, nonce, nonce_len, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_SEND, CCORD_ID_NONE, utf8, len, nonce, nonce_len, &sm_st, err)) return sm_st;
  ccord_message msg = mk_store_new(m, ci, m->self_id, utf8, len, err, &st);
  if (st != CCORD_OK) return st;
  if (nonce && nonce_len > 0) {
    // The stored row and the queued echo both carry the nonce, like Discord echoing it to the sender.
    memcpy(msg.nonce, nonce, nonce_len);
    msg.nonce[nonce_len] = '\0';
    long mi = mk_find_message(m, ci, msg.id);
    if (mi >= 0) memcpy(m->chmsgs[ci][mi].nonce, msg.nonce, sizeof msg.nonce);
  }
  mock_slowmode_mark_sent(m, channel_id);
  return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
}

ccord_status mock_send_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err)
{
  return mock_send_message_impl((mock *)t, channel_id, utf8, len, nullptr, 0, err);
}

// M0-3: send with the session's nonce; the echo (or refusal) carries it back.
ccord_status mock_send_message_with_nonce(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                                 const char *nonce, size_t nonce_len, ccord_error *err)
{
  if (!nonce || nonce_len == 0 || nonce_len >= 32u || memchr(nonce, 0, nonce_len) != nullptr) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: send nonce");
  }
  return mock_send_message_impl((mock *)t, channel_id, utf8, len, nonce, nonce_len, err);
}

ccord_status mock_edit_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                      const char *utf8, size_t len, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!ccord_utf8_valid_text(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (m->cfg.refuse_writes) return mk_queue_action_refused(m, CCORD_ACTION_EDIT, channel_id, message_id, utf8, len, nullptr, 0, err);
  char *content = ccord_strndup(utf8, len);
  if (!content) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  ccord_message *msg = &m->chmsgs[ci][mi];
  mk_retire_content(m, msg->content);
  msg->content = content;
  msg->edited = true;
  msg->edited_timestamp_ms = mk_message_time(m);
  return mk_queue_message_update(m, *msg, mk_now_of(m) + m->cfg.latency_ms, err);
}

ccord_status mock_delete_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  mk_retire_content(m, m->chmsgs[ci][mi].content);
  mk_retire_rich(m, m->chmsgs[ci][mi].rich);
  memmove(&m->chmsgs[ci][mi], &m->chmsgs[ci][mi + 1], (m->chn[ci] - (size_t)mi - 1) * sizeof *m->chmsgs[ci]);
  m->chn[ci]--;
  mock_item it = mk_make_item(CCORD_NET_MESSAGE_DELETE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  return mk_push_item(m, &it, err);
}

ccord_status mock_send_typing(ccord_transport *t, ccord_id channel_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_TYPING]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  mock_item it = mk_make_item(CCORD_NET_TYPING_START, due);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.user_id = m->self_id;
  it.ev.timestamp_ms = (int64_t)due + 8000;
  return mk_push_item(m, &it, err);
}

ccord_status mock_toggle_reaction(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                         const char *emoji, size_t len, bool add, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!emoji || len == 0 || !ccord_utf8_valid_text(emoji, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid reaction");
  }
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  ccord_message *msg = &m->chmsgs[ci][mi];
  if (add) {
    msg->self_reacted = true;
    msg->reaction_count++;
  } else {
    if (msg->reaction_count > 0) msg->reaction_count--;
    if (msg->reaction_count == 0) msg->self_reacted = false;
  }
  mock_item it = mk_make_item(CCORD_NET_REACTION_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.user_id = m->self_id;
  it.ev.reaction_count = msg->reaction_count;
  it.ev.self_reacted = msg->self_reacted;
  it.ev.reaction_delta = add ? 1 : -1;
  char *emoji_copy = ccord_strndup(emoji, len);
  if (emoji_copy) {
    mk_retire_content(m, emoji_copy);
    it.ev.reaction_emoji = emoji_copy;
  }
  return mk_push_item(m, &it, err);
}

ccord_status mock_request_reaction_users(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                                const char *emoji, size_t emoji_len, uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_REACTION_USERS]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!emoji || emoji_len == 0 || !ccord_utf8_valid_text(emoji, emoji_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid reaction emoji");
  }
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (limit == 0) limit = 50u;
  if (limit > 100u) limit = 100u;

  const char *we = emoji;
  size_t wl = emoji_len;
  if (wl > 4 && we[0] == '<' && we[wl - 1] == '>') {
    if (we[1] == ':') { we += 2; wl -= 3; }
    else if ((we[1] == 'a' || we[1] == 'A') && we[2] == ':') { we += 3; wl -= 4; }
  }

  const ccord_message *msg = &m->chmsgs[ci][mi];
  uint32_t want_count = msg->reaction_count;
  bool inc_self = msg->self_reacted;
  if (msg->reactions && msg->reaction_kind_count > 0) {
    want_count = 0;
    inc_self = false;
    for (size_t i = 0; i < msg->reaction_kind_count; i++) {
      const char *re = msg->reactions[i].emoji;
      if (re && strlen(re) == wl && memcmp(re, we, wl) == 0) {
        want_count = msg->reactions[i].count;
        inc_self = msg->reactions[i].self_reacted;
        break;
      }
    }
  }
  if (want_count > limit) want_count = limit;
  if (want_count > MOCK_NUM_USERS) want_count = MOCK_NUM_USERS;

  mock_item it = mk_make_item(CCORD_NET_REACTION_USERS, mk_now_of(m) + m->cfg.latency_ms);
  it.users_own = ccord_alloc_array(want_count ? want_count : 1u, sizeof *it.users_own);
  char *ecopy = ccord_strndup(we, wl);
  if (!it.users_own || !ecopy) {
    ccord_free(it.users_own);
    ccord_free(ecopy);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  mk_retire_content(m, ecopy);

  size_t kept = 0;
  if (inc_self && kept < want_count) {
    it.users_own[kept++] = m->users[0];
  }
  size_t start_u = (size_t)((message_id % (MOCK_NUM_USERS - 1u)) + 1u);
  for (size_t step = 0; step < MOCK_NUM_USERS - 1u && kept < want_count; step++) {
    size_t ui = 1u + ((start_u - 1u + step) % (MOCK_NUM_USERS - 1u));
    it.users_own[kept++] = m->users[ui];
  }
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.reaction_emoji = ecopy;
  it.ev.users = it.users_own;
  it.ev.user_count = kept;
  return mk_push_item(m, &it, err);
}

ccord_status mock_ack_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_ACK]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  if (mk_find_message(m, ci, message_id) < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  return CCORD_OK;
}


ccord_status mock_send_reply_ex(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                       bool mention_author, const char *utf8, size_t len, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!ccord_utf8_valid_text(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long tmi = mk_find_message(m, ci, reply_to_id);
  if (tmi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown reply target");
  if (m->cfg.refuse_writes) return mk_queue_action_refused(m, CCORD_ACTION_REPLY, channel_id, reply_to_id, utf8, len, nullptr, 0, err);
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_REPLY, reply_to_id, utf8, len, nullptr, 0, &sm_st, err)) return sm_st;
  // The reply carries the message it answers, as Discord attaches it.
  const ccord_message *target = &m->chmsgs[ci][tmi];
  const char *tname = "";
  for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
    if (m->users[ui].id == target->author_id && m->users[ui].name) tname = m->users[ui].name;
  }
  char snippet[CCORD_RICH_SNIPPET_MAX_BYTES + 1];
  size_t sn = 0;
  for (const char *tc = target->content ? target->content : ""; *tc;) {
    unsigned char c = (unsigned char)*tc;
    size_t seq = c < 0x80u ? 1u : (c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u)));
    if (sn + seq >= sizeof snippet || strlen(tc) < seq) break;
    memcpy(snippet + sn, tc, seq);
    sn += seq;
    tc += seq;
  }
  snippet[sn] = '\0';
  ccord_id target_author = target->author_id;
  bool ping_target = mention_author && target_author != CCORD_ID_NONE && target_author != m->self_id;
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL, .type = 19,
    .mention_users = ping_target ? &target_author : nullptr,
    .mention_user_count = ping_target ? 1u : 0u,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = reply_to_id, .channel_id = channel_id},
    .ref_preview = {.present = true, .author_id = target_author, .author_name = tname, .content = snippet,
                    .has_attachments = target->rich && target->rich->attachment_count > 0},
  };
  ccord_message msg = mk_store_new_rich(m, ci, m->self_id, utf8, len, &rich, err, &st);
  if (st != CCORD_OK) return st;
  msg.reply_to_id = reply_to_id;
  if (m->chn[ci] > 0) m->chmsgs[ci][m->chn[ci] - 1].reply_to_id = reply_to_id;
  mock_slowmode_mark_sent(m, channel_id);
  return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
}

ccord_status mock_send_reply(ccord_transport *t, ccord_id channel_id, ccord_id reply_to_id,
                                    const char *utf8, size_t len, ccord_error *err)
{
  return mock_send_reply_ex(t, channel_id, reply_to_id, true, utf8, len, err);
}

// ---- messaging parity: TTS, forward, crosspost, mark unread ------------------------------------

// POST /channels/{c}/messages with tts=true. Text rules as send_message; Discord additionally wants
// SEND_TTS_MESSAGES in a guild (DMs always allow it), so the mock refuses it where the role lacks the bit.
ccord_status mock_send_tts_message(ccord_transport *t, ccord_id channel_id, const char *utf8, size_t len,
                                          ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (len == 0 || len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (!utf8 || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  }
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->cfg.refuse_writes) {
    return mk_queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, utf8, len, nullptr, 0, err);
  }
  ccord_id g = m->channels[ci].guild_id;
  if (g != CCORD_DM_GUILD_ID && g != CCORD_ID_NONE &&
      (mock_channel_perms(m, g, channel_id) & CCORD_PERM_SEND_TTS_MESSAGES) == 0u) {
    return mk_queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, utf8, len, nullptr, 0, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_SEND, CCORD_ID_NONE, utf8, len, nullptr, 0, &sm_st, err)) return sm_st;
  ccord_message msg = mk_store_new(m, ci, m->self_id, utf8, len, err, &st);
  if (st != CCORD_OK) return st;
  msg.tts = true;
  long mi = mk_find_message(m, ci, msg.id);
  if (mi >= 0) m->chmsgs[ci][mi].tts = true;   // the stored row carries it into history too
  mock_slowmode_mark_sent(m, channel_id);
  return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
}

// POST /channels/{c}/messages with message_reference type 1: the new message carries a snapshot of the source
// (its text, timestamp and media) in rich->forward, and its own content is empty like Discord's.
ccord_status mock_forward_message(ccord_transport *t, ccord_id channel_id, ccord_id source_channel_id,
                                         ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  size_t sci = mk_channel_index(m, source_channel_id, false, &st, err);
  if (sci == SIZE_MAX) return st;
  long mi = mk_find_message(m, sci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (m->cfg.refuse_writes) {
    return mk_queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, "", 0, nullptr, 0, err);
  }
  ccord_id dst_guild = m->channels[ci].guild_id;
  if (dst_guild != CCORD_DM_GUILD_ID && dst_guild != CCORD_ID_NONE &&
      (mock_channel_perms(m, dst_guild, channel_id) & CCORD_PERM_SEND_MESSAGES) == 0u) {
    return mk_queue_action_refused(m, CCORD_ACTION_SEND, channel_id, CCORD_ID_NONE, "", 0, nullptr, 0, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_SEND, CCORD_ID_NONE, "", 0, nullptr, 0, &sm_st, err)) return sm_st;
  const ccord_message *src = &m->chmsgs[sci][mi];
  const ccord_message_rich *sr = src->rich;
  ccord_id src_guild = m->channels[sci].guild_id;
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL,
    .reference = {.kind = CCORD_REF_FORWARD, .message_id = message_id, .channel_id = source_channel_id,
                  .guild_id = (src_guild == CCORD_DM_GUILD_ID || src_guild == CCORD_ID_NONE) ? CCORD_ID_NONE : src_guild},
    .forward = {.present = true, .content = src->content ? src->content : "", .timestamp_ms = src->timestamp_ms,
                .attachments = sr ? sr->attachments : nullptr, .attachment_count = sr ? sr->attachment_count : 0u,
                .embeds = sr ? sr->embeds : nullptr, .embed_count = sr ? sr->embed_count : 0u,
                .stickers = sr ? sr->stickers : nullptr, .sticker_count = sr ? sr->sticker_count : 0u},
  };
  ccord_message msg = mk_store_new_rich(m, ci, m->self_id, "", 0, &rich, err, &st);
  if (st != CCORD_OK) return st;
  mock_slowmode_mark_sent(m, channel_id);
  return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
}

// POST /channels/{c}/messages/{m}/crosspost: announcement channels only. The crossposted flag rides the
// MESSAGE_UPDATE (nothing changes locally before the server confirms).
ccord_status mock_crosspost_message(ccord_transport *t, ccord_id channel_id, ccord_id message_id,
                                           ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not an announcement channel");
  }
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (m->cfg.refuse_writes) {
    return mk_queue_action_refused(m, CCORD_ACTION_SEND, channel_id, message_id, "", 0, nullptr, 0, err);
  }
  ccord_message *msg = &m->chmsgs[ci][mi];
  ccord_id g = m->channels[ci].guild_id;
  uint64_t need = (msg->author_id == m->self_id) ? CCORD_PERM_SEND_MESSAGES : CCORD_PERM_MANAGE_MESSAGES;
  if (g != CCORD_DM_GUILD_ID && g != CCORD_ID_NONE && (mock_channel_perms(m, g, channel_id) & need) == 0u) {
    return mk_queue_action_refused(m, CCORD_ACTION_SEND, channel_id, message_id, "", 0, nullptr, 0, err);
  }
  if ((ccord_message_flags(msg) & CCORD_MSGFLAG_CROSSPOSTED) != 0u) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message already crossposted");
  }
  ccord_message_rich nr = msg->rich ? *msg->rich : (ccord_message_rich){0};
  nr.present |= CCORD_RICH_HAS_TYPE;   // the merge applies flags with the type part
  nr.flags |= CCORD_MSGFLAG_CROSSPOSTED;
  ccord_message_rich *cloned = ccord_message_rich_clone(&nr, err);
  if (!cloned) return CCORD_ERR_NOMEM;
  mk_retire_rich(m, msg->rich);
  msg->rich = cloned;
  return mk_queue_message_update(m, *msg, mk_now_of(m) + m->cfg.latency_ms, err);
}

// POST /channels/{c}/messages/{m}/ack for the message BEFORE the target: the target and everything after it
// stay unread. When marking the first message of a channel unread, the acked id is `message_id - 1` so every
// stored message remains strictly newer than the read cursor.
ccord_status mock_mark_unread(ccord_transport *t, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_ACK]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  if (mi == 0 && message_id <= 1u) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no message before the target");
  uint32_t mentions = 0;
  for (size_t k = (size_t)mi; k < m->chn[ci]; k++) {
    if (m->chmsgs[ci][k].author_id != m->self_id && ccord_message_mentions_user(&m->chmsgs[ci][k], m->self_id)) {
      if (mentions < UINT32_MAX) mentions++;
    }
  }
  mock_item it = mk_make_item(CCORD_NET_MESSAGE_ACK, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = mi > 0 ? m->chmsgs[ci][mi - 1].id : (message_id - 1u);
  it.ev.mention_count = mentions;
  return mk_push_item(m, &it, err);
}

ccord_status mock_toggle_pin(ccord_transport *t, ccord_id channel_id, ccord_id message_id, bool pinned,
                                    ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long mi = mk_find_message(m, ci, message_id);
  if (mi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown message");
  m->chmsgs[ci][mi].pinned = pinned;
  mock_item it = mk_make_item(CCORD_NET_MESSAGE_PIN_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = m->channels[ci].guild_id;
  it.ev.channel_id = channel_id;
  it.ev.message_id = message_id;
  it.ev.pinned = pinned;
  return mk_push_item(m, &it, err);
}

ccord_status mock_request_history_before(ccord_transport *t, ccord_id channel_id, ccord_id before_id,
                                                uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_HISTORY]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  if (before_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: before id");
  if (limit == 0) limit = MOCK_HISTORY_DEFAULT;
  size_t end = m->chn[ci];
  while (end > 0 && m->chmsgs[ci][end - 1].id >= before_id) end--;
  size_t take = limit < end ? limit : end;
  size_t from = end - take;
  mock_item it = mk_make_item(CCORD_NET_HISTORY, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(take ? take : 1, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (take) memcpy(it.msgs, m->chmsgs[ci] + from, take * sizeof *it.msgs);
  it.ev.messages = it.msgs;
  it.ev.message_count = take;
  return mk_push_item(m, &it, err);
}

ccord_status mock_request_pins_before(ccord_transport *t, ccord_id channel_id, const char *before_cursor,
                                             uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_PINS]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, false, &st, err);
  if (ci == SIZE_MAX) return st;
  bool has_before = before_cursor && before_cursor[0] != '\0';
  ccord_id before_id = CCORD_ID_NONE;
  if (has_before && !ccord_id_parse(before_cursor, strlen(before_cursor), &before_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid pins cursor");
  }
  if (limit == 0) limit = 50;
  if (limit > 50) limit = 50;
  mock_item it = mk_make_item(CCORD_NET_PIN_LIST, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(limit, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t n = 0;
  bool has_more = false;
  for (size_t i = m->chn[ci]; i > 0; i--) {
    const ccord_message *cand = &m->chmsgs[ci][i - 1];
    if (!cand->pinned) continue;
    if (has_before && cand->id >= before_id) continue;
    if (n < limit) {
      it.msgs[n++] = *cand;
    } else {
      has_more = true;
      break;
    }
  }
  if (n > 0) {
    char cbuf[24];
    int clen = ccord_id_format(it.msgs[n - 1].id, cbuf);
    if (clen > 0) {
      char *ccopy = ccord_strndup(cbuf, (size_t)clen);
      if (ccopy) {
        mk_retire_content(m, ccopy);
        it.ev.pins_next_cursor = ccopy;
      }
    }
  }
  it.ev.messages = it.msgs;
  it.ev.message_count = n;
  it.ev.pins_has_more = has_more;
  it.ev.pins_append = has_before;
  return mk_push_item(m, &it, err);
}

ccord_status mock_request_pins(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  return mock_request_pins_before(t, channel_id, nullptr, limit, err);
}


bool mk_has_ci_substr(const char *haystack, const char *needle, size_t needle_len)
{
  if (!haystack || !needle || needle_len == 0) return false;
  size_t hlen = strlen(haystack);
  if (hlen < needle_len) return false;
  for (size_t i = 0; i + needle_len <= hlen; i++) {
    bool match = true;
    for (size_t j = 0; j < needle_len; j++) {
      if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

static const char *infer_mime_type(const char *filename, bool *out_is_image)
{
  if (out_is_image) *out_is_image = false;
  if (!filename) return "application/octet-stream";
  const char *dot = strrchr(filename, '.');
  if (!dot) return "application/octet-stream";
  if (strcmp(dot, ".png") == 0 || strcmp(dot, ".PNG") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/png";
  }
  if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".JPG") == 0 ||
      strcmp(dot, ".jpeg") == 0 || strcmp(dot, ".JPEG") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/jpeg";
  }
  if (strcmp(dot, ".webp") == 0 || strcmp(dot, ".WEBP") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/webp";
  }
  if (strcmp(dot, ".gif") == 0 || strcmp(dot, ".GIF") == 0) {
    if (out_is_image) *out_is_image = true;
    return "image/gif";
  }
  if (strcmp(dot, ".ogg") == 0 || strcmp(dot, ".OGG") == 0) return "audio/ogg";
  return "application/octet-stream";
}

// Retained upload bodies, keyed by the attachment URL the mock answers with. Bounded FIFO: the oldest entry is dropped
// when another upload would exceed either cap. A failed allocation just skips retention; the upload itself still works.
static void upload_cache_remove_at(mock *m, size_t i)
{
  if (i >= m->nuploads) return;
  m->upload_bytes -= m->uploads[i].len;
  ccord_free(m->uploads[i].url);
  ccord_free(m->uploads[i].data);
  memmove(&m->uploads[i], &m->uploads[i + 1], (m->nuploads - i - 1) * sizeof m->uploads[0]);
  m->nuploads--;
}

static void upload_cache_drop_oldest(mock *m)
{
  if (m->nuploads > 0) upload_cache_remove_at(m, 0);
}

void mk_upload_cache_free(mock *m)
{
  while (m->nuploads > 0) upload_cache_drop_oldest(m);
}

static void upload_cache_add(mock *m, const char *url, const void *data, size_t len)
{
  if (!m || !url || url[0] == '\0' || !data || len == 0 || len > MOCK_UPLOAD_CACHE_BYTES_MAX) return;
  // The mock URL is derived from the filename alone, so a repeat name replaces the earlier body instead of serving stale bytes.
  for (size_t i = 0; i < m->nuploads; i++) {
    if (strcmp(m->uploads[i].url, url) == 0) {
      upload_cache_remove_at(m, i);
      break;
    }
  }
  while (m->nuploads > 0 &&
         (m->nuploads >= MOCK_UPLOAD_CACHE_MAX || m->upload_bytes + len > MOCK_UPLOAD_CACHE_BYTES_MAX)) {
    upload_cache_drop_oldest(m);
  }
  char *url_copy = ccord_strndup(url, strlen(url));
  uint8_t *data_copy = ccord_alloc_array(len, 1);
  if (!url_copy || !data_copy) {
    ccord_free(url_copy);
    ccord_free(data_copy);
    return;
  }
  memcpy(data_copy, data, len);
  m->uploads[m->nuploads++] = (mock_upload){.url = url_copy, .data = data_copy, .len = len};
  m->upload_bytes += len;
}

const mock_upload *mk_upload_cache_find(const mock *m, const char *url)
{
  for (size_t i = 0; m && url && i < m->nuploads; i++) {
    if (strcmp(m->uploads[i].url, url) == 0) return &m->uploads[i];
  }
  return nullptr;
}

ccord_status mock_send_message_with_files(ccord_transport *t, ccord_id channel_id,
                                                 ccord_id reply_to_id, bool mention_author,
                                                 const char *utf8, size_t len,
                                                 const ccord_upload_file *files, size_t file_count,
                                                 ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!files || file_count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: no upload files");
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many files");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: message length");
  if (len > 0 && (!utf8 || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: message is not valid text");
  }
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty file data");
    }
  }
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  long tmi = -1;
  if (reply_to_id != CCORD_ID_NONE) {
    tmi = mk_find_message(m, ci, reply_to_id);
    if (tmi < 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown reply target");
  }
  if (m->cfg.refuse_writes) {
    return mk_queue_action_refused(m, CCORD_ACTION_UPLOAD, channel_id, reply_to_id, utf8 ? utf8 : "", len, nullptr, 0, err);
  }
  ccord_status sm_st = CCORD_OK;
  if (mock_slowmode_refused(m, ci, CCORD_ACTION_UPLOAD, reply_to_id, utf8 ? utf8 : "", len, nullptr, 0, &sm_st, err)) return sm_st;

  char wnames[CCORD_RICH_MAX_ATTACHMENTS][256];
  char urls[CCORD_RICH_MAX_ATTACHMENTS][512];
  ccord_attachment atts[CCORD_RICH_MAX_ATTACHMENTS];
  memset(atts, 0, sizeof atts);
  for (size_t i = 0; i < file_count; i++) {
    const char *fn = files[i].filename;
    bool already_spoiler = strncmp(fn, "SPOILER_", 8) == 0;
    if (files[i].spoiler && !already_spoiler) {
      (void)snprintf(wnames[i], sizeof wnames[i], "SPOILER_%.240s", fn);
    } else {
      (void)snprintf(wnames[i], sizeof wnames[i], "%.248s", fn);
    }
    bool is_img = false;
    const char *mime = infer_mime_type(wnames[i], &is_img);
    (void)snprintf(urls[i], sizeof urls[i], "https://cdn.discordapp.com/attachments/mock/%.255s", wnames[i]);
    atts[i] = (ccord_attachment){
      .id = m->next_msg_id + i,
      .filename = wnames[i],
      .description = "",
      .content_type = mime,
      .url = urls[i],
      .proxy_url = "",
      .size = files[i].size,
      .duration_ms = files[i].duration_ms,
      .waveform_b64 = files[i].waveform_b64 ? files[i].waveform_b64 : "",
      .spoiler = files[i].spoiler || already_spoiler,
    };
    // Keep the bytes under the URL the attachment advertises, so fetch_media can play back what was uploaded.
    upload_cache_add(m, urls[i], files[i].data, files[i].size);
  }

  uint32_t msg_flags = 0;
  for (size_t i = 0; i < file_count; i++) {
    if (files[i].duration_ms != 0) msg_flags |= CCORD_MSGFLAG_VOICE_MESSAGE;
  }
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL, .flags = msg_flags, .attachments = atts, .attachment_count = file_count,
  };
  ccord_id target_author = CCORD_ID_NONE;
  char snippet[CCORD_RICH_SNIPPET_MAX_BYTES + 1] = "";
  if (tmi >= 0) {
    const ccord_message *target = &m->chmsgs[ci][tmi];
    target_author = target->author_id;
    const char *tname = "";
    for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
      if (m->users[ui].id == target_author && m->users[ui].name) tname = m->users[ui].name;
    }
    size_t sn = 0;
    for (const char *tc = target->content ? target->content : ""; *tc;) {
      unsigned char c = (unsigned char)*tc;
      size_t seq = c < 0x80u ? 1u : (c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u)));
      if (sn + seq >= sizeof snippet || strlen(tc) < seq) break;
      memcpy(snippet + sn, tc, seq);
      sn += seq;
      tc += seq;
    }
    snippet[sn] = '\0';
    bool ping_target = mention_author && target_author != CCORD_ID_NONE && target_author != m->self_id;
    rich.type = 19;
    rich.mention_users = ping_target ? &target_author : nullptr;
    rich.mention_user_count = ping_target ? 1u : 0u;
    rich.reference = (ccord_msg_reference){.kind = CCORD_REF_REPLY, .message_id = reply_to_id, .channel_id = channel_id};
    rich.ref_preview = (ccord_ref_preview){.present = true, .author_id = target_author, .author_name = tname,
                                           .content = snippet,
                                           .has_attachments = target->rich && target->rich->attachment_count > 0};
  }

  ccord_message msg = mk_store_new_rich(m, ci, m->self_id, utf8 ? utf8 : "", utf8 ? len : 0u, &rich, err, &st);
  if (st != CCORD_OK) return st;
  if (reply_to_id != CCORD_ID_NONE) {
    msg.reply_to_id = reply_to_id;
    if (m->chn[ci] > 0) m->chmsgs[ci][m->chn[ci] - 1].reply_to_id = reply_to_id;
  }
  mock_slowmode_mark_sent(m, channel_id);
  uint64_t now_t = mk_now_of(m);
  uint64_t due_t = now_t + m->cfg.latency_ms;
  st = mk_queue_message_create(m, msg, due_t, err);
  if (st != CCORD_OK) return st;
  if (m->ninflight_uploads >= MOCK_INFLIGHT_UPLOADS_MAX) {
    memmove(&m->inflight_uploads[0], &m->inflight_uploads[1],
            (MOCK_INFLIGHT_UPLOADS_MAX - 1u) * sizeof m->inflight_uploads[0]);
    m->ninflight_uploads = MOCK_INFLIGHT_UPLOADS_MAX - 1u;
  }
  mock_inflight_upload *iu = &m->inflight_uploads[m->ninflight_uploads++];
  memset(iu, 0, sizeof *iu);
  iu->upload_id = ++m->next_upload_id;
  iu->channel_id = channel_id;
  iu->message_id = msg.id;
  iu->file_count = (uint32_t)file_count;
  iu->start_ms = now_t;
  iu->due_ms = due_t;
  for (size_t i = 0; i < file_count; i++) {
    iu->file_sizes[i] = (uint64_t)files[i].size;
    iu->total_bytes += (uint64_t)files[i].size;
    const char *raw_fn = files[i].filename ? files[i].filename : "file";
    const char *sl = strrchr(raw_fn, '/');
    const char *bsl = strrchr(raw_fn, '\\');
    const char *base = raw_fn;
    if (sl && sl + 1 > base) base = sl + 1;
    if (bsl && bsl + 1 > base) base = bsl + 1;
    bool add_sp = files[i].spoiler && strncmp(base, "SPOILER_", 8) != 0;
    (void)snprintf(iu->filenames[i], sizeof iu->filenames[i], "%s%s",
                   add_sp ? "SPOILER_" : "", base[0] ? base : "file");
  }
  return CCORD_OK;
}

size_t mock_active_uploads(ccord_transport *t, ccord_upload_progress *out, size_t max)
{
  mock *m = (mock *)t;
  if (!m || !m->connected) return 0;
  uint64_t now = mk_now_of(m);
  for (size_t i = 0; i < m->ninflight_uploads; i++) {
    const mock_inflight_upload *iu = &m->inflight_uploads[i];
    if (out && i < max) {
      ccord_upload_progress *p = &out[i];
      memset(p, 0, sizeof *p);
      p->upload_id = iu->upload_id;
      p->channel_id = iu->channel_id;
      p->file_count = iu->file_count > 0 ? iu->file_count : 1u;
      p->total_bytes = iu->total_bytes;
      double frac = 0.0;
      if (iu->due_ms > iu->start_ms && now > iu->start_ms) {
        frac = (double)(now - iu->start_ms) / (double)(iu->due_ms - iu->start_ms);
        if (frac > 1.0) frac = 1.0;
      }
      if (frac < 0.10) {
        p->phase = CCORD_UPLOAD_SLOT;
        p->file_index = 0;
        p->uploaded_bytes = 0;
      } else if (frac >= 0.90) {
        p->phase = CCORD_UPLOAD_FINALIZE;
        p->file_index = p->file_count - 1u;
        p->uploaded_bytes = iu->total_bytes;
      } else {
        p->phase = CCORD_UPLOAD_PUT;
        double put_f = (frac - 0.10) / 0.80;
        uint64_t done = (uint64_t)((double)iu->total_bytes * put_f + 0.5);
        if (done > iu->total_bytes) done = iu->total_bytes;
        p->uploaded_bytes = done;
        uint64_t acc = 0;
        uint32_t fidx = p->file_count - 1u;
        for (uint32_t fi = 0; fi < p->file_count; fi++) {
          acc += iu->file_sizes[fi];
          if (done < acc || fi + 1u == p->file_count) {
            fidx = fi;
            break;
          }
        }
        p->file_index = fidx;
      }
      (void)snprintf(p->filename, sizeof p->filename, "%s", iu->filenames[p->file_index]);
    }
  }
  return m->ninflight_uploads;
}

ccord_status mock_send_message_with_file(ccord_transport *t, ccord_id channel_id,
                                                const char *utf8, size_t len,
                                                const char *filename, const void *file_data,
                                                size_t file_size, ccord_error *err)
{
  ccord_upload_file f = {.filename = filename, .data = file_data, .size = file_size, .spoiler = false};
  return mock_send_message_with_files(t, channel_id, CCORD_ID_NONE, true, utf8, len, &f, 1, err);
}

// POST a poll: validates exactly like the live body builder, then posts the poll message as the current user. A refused write
// (refuse_writes) fails the call itself with CCORD_ERR_PERMISSION.
ccord_status mock_create_poll(ccord_transport *t, ccord_id channel_id, const ccord_poll_create *poll,
                                     ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_status st = ccord_poll_build_body(poll, &body, err);
  ccord_buf_free(&body);
  if (st != CCORD_OK) return st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  if (m->cfg.refuse_writes) {   // unlike a text send, refused in the call itself so the poll modal can show the reason
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "You don't have permission to create polls here.");
  }
  if (m->late_poll_fail[0]) {   // accepted by the call, refused later like a Discord 4xx; the poll is never created
    mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, mk_now_of(m) + m->cfg.latency_ms);
    it.ev.action = CCORD_ACTION_CREATE_POLL;
    it.ev.channel_id = channel_id;
    it.ev.status = CCORD_ERR_PERMISSION;
    it.ev.text = m->late_poll_fail;
    return mk_push_item(m, &it, err);
  }
  ccord_message_rich rich = {.present = CCORD_RICH_ALL};
  rich.poll.present = true;
  rich.poll.question = poll->question;
  rich.poll.answer_count = poll->answer_count;
  rich.poll.multiselect = poll->allow_multiselect;
  rich.poll.expiry_ms = mk_message_time(m) + (int64_t)poll->duration_hours * 3600 * 1000;
  for (size_t i = 0; i < poll->answer_count; i++) {
    rich.poll.answers[i] = (ccord_poll_answer){.id = (uint32_t)i + 1u, .text = poll->answers[i], .emoji = ""};
  }
  ccord_message msg = mk_store_new_rich(m, ci, m->self_id, "", 0, &rich, err, &st);
  if (st != CCORD_OK) return st;
  return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
}


typedef struct mock_search_ud {
  const mock *m;
  ccord_id guild_id;
} mock_search_ud;

static bool mock_search_user_matches(void *ud, ccord_id user_id, const char *needle)
{
  const mock_search_ud *su = ud;
  const mock *m = su->m;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == user_id) {
      if (ccord_search_contains_ci(m->users[i].name, needle) ||
          ccord_search_contains_ci(m->users[i].handle, needle)) {
        return true;
      }
      break;
    }
  }
  if (su->guild_id != CCORD_ID_NONE && su->guild_id != CCORD_DM_GUILD_ID) {
    for (size_t i = 0; i < m->nmembers; i++) {
      if (m->members[i].guild_id == su->guild_id && m->members[i].user_id == user_id) {
        if (m->members[i].nick[0] != '\0' && ccord_search_contains_ci(m->members[i].nick, needle)) {
          return true;
        }
        break;
      }
    }
  }
  return false;
}

ccord_status mock_search_messages_ex(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                            const ccord_search_query *query, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_SEARCH]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!query) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null search query");
  size_t clen = strlen(query->content);
  if (clen > 0 && !ccord_utf8_valid_text(query->content, clen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid search query");
  }
  ccord_id eff_ch = query->channel_id != CCORD_ID_NONE ? query->channel_id
                    : (query->in_channel[0] != '\0' ? CCORD_ID_NONE : channel_id);
  bool has_any = clen > 0 || query->has_operators || eff_ch != CCORD_ID_NONE;
  if (!has_any) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty search query");

  mock_item it = mk_make_item(CCORD_NET_SEARCH_RESULTS, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild_id;
  it.ev.channel_id = eff_ch;
  it.ev.search_offset = query->offset;
  it.msgs = ccord_alloc_array(CCORD_SEARCH_PAGE_SIZE, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  size_t total = 0;
  size_t count = 0;
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    const ccord_channel *ch = &m->channels[ci];
    if (ch->kind != CCORD_CHANNEL_TEXT) continue;
    if (eff_ch != CCORD_ID_NONE && ch->id != eff_ch) continue;
    if (guild_id != CCORD_ID_NONE && ch->guild_id != guild_id) continue;
    mock_search_ud su = {.m = m, .guild_id = ch->guild_id};
    for (size_t mi = m->chn[ci]; mi > 0; mi--) {
      const ccord_message *msg = &m->chmsgs[ci][mi - 1];
      const char *aname = "";
      const char *ahandle = "";
      const char *anick = "";
      for (size_t u = 0; u < MOCK_NUM_USERS; u++) {
        if (m->users[u].id == msg->author_id) {
          aname = m->users[u].name ? m->users[u].name : "";
          ahandle = m->users[u].handle ? m->users[u].handle : "";
          break;
        }
      }
      for (size_t mb = 0; mb < m->nmembers; mb++) {
        if (m->members[mb].guild_id == ch->guild_id && m->members[mb].user_id == msg->author_id) {
          anick = m->members[mb].nick;
          break;
        }
      }
      ccord_search_msg_ctx ctx = {
        .self_id = m->self_id,
        .author_name = aname,
        .author_handle = ahandle,
        .author_nick = anick,
        .channel_name = ch->name ? ch->name : "",
        .user_matches = mock_search_user_matches,
        .ud = &su,
      };
      if (ccord_search_message_matches(query, msg, &ctx)) {
        if (total >= query->offset && count < CCORD_SEARCH_PAGE_SIZE) {
          it.msgs[count++] = *msg;
        }
        total++;
      }
    }
  }
  it.ev.messages = it.msgs;
  it.ev.message_count = count;
  it.ev.search_total_results = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  return mk_push_item(m, &it, err);
}

ccord_status mock_search_messages(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                         const char *query, size_t query_len, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_SEARCH]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!query || query_len == 0 || !ccord_utf8_valid_text(query, query_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid search query");
  }
  mock_item it = mk_make_item(CCORD_NET_SEARCH_RESULTS, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild_id;
  it.ev.channel_id = channel_id;
  it.msgs = ccord_alloc_array(50, sizeof *it.msgs);
  if (!it.msgs) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t count = 0;
  size_t total = 0;
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    if (channel_id != CCORD_ID_NONE && m->channels[ci].id != channel_id) continue;
    if (guild_id != CCORD_ID_NONE && m->channels[ci].guild_id != guild_id) continue;
    for (size_t mi = m->chn[ci]; mi > 0; mi--) {
      const ccord_message *msg = &m->chmsgs[ci][mi - 1];
      if (mk_has_ci_substr(msg->content, query, query_len)) {
        if (count < 50) it.msgs[count++] = *msg;
        total++;
      }
    }
  }
  it.ev.messages = it.msgs;
  it.ev.message_count = count;
  it.ev.search_total_results = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  return mk_push_item(m, &it, err);
}

ccord_status mock_open_dm(ccord_transport *t, ccord_id recipient_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (recipient_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid recipient");

  const char *uname = "Direct Message";
  const char *uavatar = "";
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == recipient_id) {
      uname = m->users[i].name;
      uavatar = m->users[i].avatar_url ? m->users[i].avatar_url : "";
      break;
    }
  }
  ccord_id dm_cid = 900000u + recipient_id;
  size_t existing = SIZE_MAX;
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == dm_cid) {
      existing = i;
      break;
    }
  }
  if (existing == SIZE_MAX) {
    size_t n = m->nchannels + 1;
    ccord_channel *nc = ccord_realloc_array(m->channels, n, sizeof *nc);
    ccord_message **nmsgs = ccord_realloc_array(m->chmsgs, n, sizeof *nmsgs);
    size_t *nchn = ccord_realloc_array(m->chn, n, sizeof *nchn);
    size_t *nchcap = ccord_realloc_array(m->chcap, n, sizeof *nchcap);
    if (!nc || !nmsgs || !nchn || !nchcap) {
      if (nc) m->channels = nc;
      if (nmsgs) m->chmsgs = nmsgs;
      if (nchn) m->chn = nchn;
      if (nchcap) m->chcap = nchcap;
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    m->channels = nc;
    m->chmsgs = nmsgs;
    m->chn = nchn;
    m->chcap = nchcap;
    m->channels[m->nchannels] = (ccord_channel){
      .id = dm_cid,
      .guild_id = CCORD_DM_GUILD_ID,
      .parent_id = CCORD_ID_NONE,
      .kind = CCORD_CHANNEL_TEXT,
      .discord_type = CCORD_DISCORD_CHANNEL_DM,
      .name = uname,
      .topic = "Direct Message",
      .user_id = recipient_id,
      .recipient_count = 1u,
      .recipient_ids = {recipient_id},
      .icon_url = uavatar,
    };
    m->chmsgs[m->nchannels] = nullptr;
    m->chn[m->nchannels] = 0;
    m->chcap[m->nchannels] = 0;
    existing = m->nchannels;
    m->nchannels = n;
    m->snap.channels = m->channels;
    m->snap.channel_count = m->nchannels;
  }

  mock_item it = mk_make_item(CCORD_NET_CHANNEL_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
  it.chan = ccord_alloc_array(1, sizeof *it.chan);
  if (!it.chan) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.chan = m->channels[existing];
  it.ev.channel = it.chan;
  it.ev.channel_id = it.chan->id;
  it.ev.guild_id = CCORD_DM_GUILD_ID;
  return mk_push_item(m, &it, err);
}


ccord_status ccord_mock_post_rich_message(ccord_transport *t, ccord_id channel_id, ccord_id author_id,
                                          const char *content, const ccord_message_rich *rich,
                                          ccord_id *out_id, ccord_error *err)
{
  if (out_id) *out_id = CCORD_ID_NONE;
  ccord_error_clear(err);
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  ccord_status st;
  size_t ci = mk_channel_index(m, channel_id, true, &st, err);
  if (ci == SIZE_MAX) return st;
  const char *txt = content ? content : "";
  size_t len = strlen(txt);
  if (len > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(txt, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid rich message text");
  }
  if (rich) {
    st = ccord_message_rich_validate(rich, err);
    if (st != CCORD_OK) return st;
  }
  ccord_id author = author_id != CCORD_ID_NONE ? author_id : m->users[10].id;
  ccord_message msg = mk_store_new_rich(m, ci, author, txt, len, rich, err, &st);
  if (st != CCORD_OK) return st;
  if (out_id) *out_id = msg.id;
  return mk_queue_message_create(m, msg, mk_now_of(m) + m->cfg.latency_ms, err);
}


