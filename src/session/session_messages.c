// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool pins_is_in_flight(const ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < s->npins_in_flight; i++) {
    if (s->pins_in_flight[i] == channel_id) return true;
  }
  return false;
}

static void pins_mark_in_flight(ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE || pins_is_in_flight(s, channel_id)) return;
  if (s->npins_in_flight < sizeof s->pins_in_flight / sizeof s->pins_in_flight[0]) {
    s->pins_in_flight[s->npins_in_flight++] = channel_id;
  }
}

void sess_pins_clear_in_flight(ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < s->npins_in_flight; ) {
    if (s->pins_in_flight[i] == channel_id) {
      memmove(&s->pins_in_flight[i], &s->pins_in_flight[i + 1],
              (s->npins_in_flight - i - 1) * sizeof s->pins_in_flight[0]);
      s->npins_in_flight--;
    } else {
      i++;
    }
  }
}


// ---- M0-3 local pending/failed echoes ----
// A send inserts a visibly-pending echo with a local id whose top bit no current snowflake uses, so it sorts after
// confirmed history and cannot collide with a server id. The server's nonce-matched MESSAGE_CREATE replaces it; a
// refusal marks it failed. Echoes never move unread/mention counts: they are the user's own unconfirmed text.
#define SESSION_LOCAL_ID_FLAG (1ULL << 63)

bool session_is_local_echo_id(ccord_id id)
{
  return (id & SESSION_LOCAL_ID_FLAG) != 0;
}

// Unique decimal nonce: monotonic timestamp + a per-session counter (well under Discord's 25-character cap).
static void make_send_nonce(ccord_session *s, char out[static 32])
{
  (void)snprintf(out, 32, "%llu%06llu", (unsigned long long)ccord_clock_ms(),
                 (unsigned long long)(++s->nonce_seq % 1000000ull));
}

// Copies the newest pending/failed echo of `channel_id` carrying `nonce` into `out` (by value: store pointers do not
// survive the upserts that follow). Newest-first: echoes sort after confirmed history.
bool sess_find_local_echo_by_nonce(const ccord_session *s, ccord_id channel_id, const char *nonce,
                                     session_local_echo *out)
{
  if (!nonce || nonce[0] == '\0') return false;
  size_t n = ccord_store_message_count(s->store, channel_id);
  for (size_t i = n; i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(s->store, channel_id, i - 1u);
    if (!m || !session_is_local_echo_id(m->id) || m->send_state == CCORD_MSG_SEND_CONFIRMED) continue;
    if (strcmp(m->nonce, nonce) != 0) continue;
    out->id = m->id;
    out->author_id = m->author_id;
    out->timestamp_ms = m->timestamp_ms;
    out->send_state = m->send_state;
    memcpy(out->nonce, m->nonce, sizeof out->nonce);
    (void)snprintf(out->text, sizeof out->text, "%s", m->content);
    return true;
  }
  return false;
}

// Deletes an echo without touching unread/mention counts: the echo never contributed to them, but the store's
// delete would otherwise forget an "unread" row whose local id sits past the read position.
void sess_remove_local_echo(ccord_session *s, ccord_id channel_id, ccord_id echo_id)
{
  const ccord_channel *c = ccord_store_channel(s->store, channel_id);
  if (!c) return;
  uint32_t unread = c->unread_count, mentions = c->mention_count;
  ccord_error err;
  if (ccord_store_delete_message(s->store, channel_id, echo_id, &err) == CCORD_OK) {
    (void)ccord_store_set_channel_counts(s->store, channel_id, unread, mentions, &err);
  }
}

// Re-states an echo (pending <-> failed) by delete + re-append at the same id, preserving the counts.
bool sess_set_local_echo_state(ccord_session *s, ccord_id channel_id, const session_local_echo *e, uint8_t state)
{
  const ccord_channel *c = ccord_store_channel(s->store, channel_id);
  if (!c) return false;
  uint32_t unread = c->unread_count, mentions = c->mention_count;
  ccord_message m = {
    .id = e->id,
    .channel_id = channel_id,
    .author_id = e->author_id,
    .timestamp_ms = e->timestamp_ms,
    .content = e->text,
    .send_state = state,
  };
  memcpy(m.nonce, e->nonce, sizeof m.nonce);
  ccord_error err;
  if (ccord_store_delete_message(s->store, channel_id, e->id, &err) != CCORD_OK) return false;
  if (ccord_store_append_message(s->store, &m, &err) != CCORD_OK) return false;
  (void)ccord_store_set_channel_counts(s->store, channel_id, unread, mentions, &err);
  return true;
}


// Standard or url-safe base64 (padding optional, ASCII whitespace ignored) into a fresh ccord_malloc buffer.
// Mirrors the settings-proto reader's decoder so the cached wire bytes are exactly what the server sent.

bool sess_blank(const char *s, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    if (s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\r') return false;
  }
  return true;
}

// Membership screening: while the signed-in member is pending, the guild refuses their messages. The store owns the
// flag (ccord_member.pending); it clears only when Discord confirms the submitted form, never optimistically.
ccord_status session_screening_gate(const ccord_session *s, const ccord_channel *ch, ccord_error *err)
{
  if (ch->guild_id != CCORD_DM_GUILD_ID && ccord_session_self_pending(s, ch->guild_id)) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION,
                           "Complete the server's membership screening before sending messages.");
  }
  return CCORD_OK;
}

ccord_status ccord_session_send_message(ccord_session *s, ccord_id channel_id, const char *utf8, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s || !utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "message is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  if (sess_blank(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "message is empty");
  if (memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is not valid text");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  // M0-3: the pending echo needs a nonce the transport can carry and confirm; hook-less transports (and a store
  // without a self id yet) keep the legacy no-echo path.
  ccord_id self_id = ccord_store_self_id(s->store);
  if (!CCORD_TRANSPORT_OPTIONAL(s->transport, send_message_with_nonce) || self_id == CCORD_ID_NONE) {
    return ccord_transport_send_message(s->transport, channel_id, utf8, len, err);
  }
  char text[CCORD_SEND_MAX_BYTES + 1];   // NUL-terminated copy: the caller's buffer is borrowed, and the store
  memcpy(text, utf8, len);               // upsert below may reallocate the vectors the channel pointer lives in
  text[len] = '\0';
  char nonce[32];
  make_send_nonce(s, nonce);
  session_local_echo echo = {
    .id = SESSION_LOCAL_ID_FLAG | (++s->local_msg_seq & (SESSION_LOCAL_ID_FLAG - 1u)),
    .author_id = self_id,
    .timestamp_ms = (int64_t)ccord_clock_ms(),
    .send_state = CCORD_MSG_SEND_PENDING,
  };
  memcpy(echo.nonce, nonce, sizeof echo.nonce);
  memcpy(echo.text, text, len + 1u);
  ccord_message m = {
    .id = echo.id,
    .channel_id = channel_id,
    .author_id = self_id,
    .timestamp_ms = echo.timestamp_ms,
    .content = text,
    .send_state = CCORD_MSG_SEND_PENDING,
  };
  memcpy(m.nonce, nonce, sizeof m.nonce);
  ccord_error aerr;
  bool echoed = ccord_store_append_message(s->store, &m, &aerr) == CCORD_OK;
  if (!echoed) CCORD_LOG_W("session: pending echo not stored: %s", aerr.message);
  ccord_status st = ccord_transport_send_message_with_nonce(s->transport, channel_id, text, len,
                                                            nonce, strlen(nonce), err);
  if (st != CCORD_OK && echoed) {
    // Synchronous refusal: roll the echo back. The caller still holds the text (the composer keeps it on error).
    sess_remove_local_echo(s, channel_id, echo.id);
  }
  return st;
}

ccord_status ccord_session_retry_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!session_is_local_echo_id(message_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "not a failed message");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m || m->send_state != CCORD_MSG_SEND_FAILED || m->nonce[0] == '\0') {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "not a failed message");
  }
  if (!CCORD_TRANSPORT_OPTIONAL(s->transport, send_message_with_nonce)) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "transport: retry not supported");
  }
  // Copy the draft and the ORIGINAL nonce to the stack before the store mutates (Discord dedupes by the nonce).
  session_local_echo echo = {
    .id = m->id,
    .author_id = m->author_id,
    .timestamp_ms = m->timestamp_ms,
    .send_state = CCORD_MSG_SEND_PENDING,
  };
  memcpy(echo.nonce, m->nonce, sizeof echo.nonce);
  (void)snprintf(echo.text, sizeof echo.text, "%s", m->content);
  if (!sess_set_local_echo_state(s, channel_id, &echo, CCORD_MSG_SEND_PENDING)) {
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "session: could not re-pend the message");
  }
  ccord_status st = ccord_transport_send_message_with_nonce(s->transport, channel_id, echo.text, strlen(echo.text),
                                                            echo.nonce, strlen(echo.nonce), err);
  if (st != CCORD_OK) {
    // Synchronous refusal: the bubble stays failed with its draft.
    (void)sess_set_local_echo_state(s, channel_id, &echo, CCORD_MSG_SEND_FAILED);
  }
  return st;
}

ccord_status sess_validate_text(const char *utf8, size_t len, ccord_error *err)
{
  if (!utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null text");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "text is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  if (sess_blank(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "text is empty");
  if (memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "text is not valid text");
  }
  return CCORD_OK;
}

ccord_status ccord_session_edit_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                        const char *utf8, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_status st = sess_validate_text(utf8, len, err);
  if (st != CCORD_OK) return st;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  const ccord_message *em = ccord_store_find_message(s->store, channel_id, message_id);
  if (!em) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(em->id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  return ccord_transport_edit_message(s->transport, channel_id, message_id, utf8, len, err);
}

ccord_status ccord_session_delete_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  const ccord_message *dm = ccord_store_find_message(s->store, channel_id, message_id);
  if (!dm) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(dm->id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  return ccord_transport_delete_message(s->transport, channel_id, message_id, err);
}

ccord_status ccord_session_send_typing(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  return ccord_transport_send_typing(s->transport, channel_id, err);
}

ccord_status ccord_session_toggle_reaction(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                           const char *emoji, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!emoji || len == 0 || memchr(emoji, 0, len) || !ccord_utf8_valid_text(emoji, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid reaction");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  bool already_reacted = m->self_reacted;
  const char *send_emoji = emoji;
  size_t send_len = len;
  if (m->reaction_kind_count > 0 && m->reactions) {
    already_reacted = false;
    for (size_t i = 0; i < m->reaction_kind_count; i++) {
      const char *re = m->reactions[i].emoji;
      if (!re) continue;
      size_t rlen = strlen(re);
      bool match = (rlen == len && memcmp(re, emoji, len) == 0);
      if (!match && !memchr(emoji, ':', len)) {
        const char *colon = strrchr(re, ':');
        if (colon && (size_t)(colon - re) == len && memcmp(re, emoji, len) == 0) {
          match = true;
        }
      }
      if (match) {
        already_reacted = m->reactions[i].self_reacted;
        send_emoji = re;
        send_len = rlen;
        break;
      }
    }
  }
  return ccord_transport_toggle_reaction(s->transport, channel_id, message_id, send_emoji, send_len,
                                         !already_reacted, err);
}

ccord_status ccord_session_ack_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (message_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: ack message id");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (m && ccord_message_is_ephemeral(m)) {
    ccord_error_clear(err);
    return CCORD_OK;
  }
  if (m && session_is_local_echo_id(m->id)) {
    ccord_error_clear(err);   // unconfirmed echo: there is no server message to acknowledge yet
    return CCORD_OK;
  }
  ccord_id upto = message_id;
  if (ch->unread_count == 0 && ch->mention_count == 0 && ch->last_message_id > upto) {
    upto = ch->last_message_id;
  }
  ccord_status st = ccord_transport_ack_message(s->transport, channel_id, message_id, err);
  if (st == CCORD_OK) {
    ccord_error ignored;
    (void)ccord_store_apply_read(s->store, channel_id, upto, UINT32_MAX, &ignored);
  }
  return st;
}

ccord_status ccord_session_dismiss_ephemeral(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                             ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  return ccord_store_dismiss_ephemeral(s->store, channel_id, message_id, err);
}


ccord_status ccord_session_send_reply_ex(ccord_session *s, ccord_id channel_id, ccord_id reply_to_id,
                                         bool mention_author, const char *utf8, size_t len, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_status st = sess_validate_text(utf8, len, err);
  if (st != CCORD_OK) return st;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  const ccord_message *target = ccord_store_find_message(s->store, channel_id, reply_to_id);
  if (!target) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown reply target");
  }
  if (ccord_message_is_ephemeral(target)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "cannot reply to ephemeral message");
  }
  return ccord_transport_send_reply_ex(s->transport, channel_id, reply_to_id, mention_author, utf8, len, err);
}

ccord_status ccord_session_send_reply(ccord_session *s, ccord_id channel_id, ccord_id reply_to_id,
                                      const char *utf8, size_t len, ccord_error *err)
{
  return ccord_session_send_reply_ex(s, channel_id, reply_to_id, true, utf8, len, err);
}

ccord_status ccord_session_toggle_pin(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  return ccord_transport_toggle_pin(s->transport, channel_id, message_id, !m->pinned, err);
}

ccord_status ccord_session_request_pins(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: channel id");
  if (!ccord_store_channel(s->store, channel_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  sess_pins_clear_in_flight(s, channel_id);
  return ccord_transport_request_pins(s->transport, channel_id, 50u, err);
}

ccord_status ccord_session_request_more_pins(ccord_session *s, ccord_id channel_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: channel id");
  if (!ccord_store_channel(s->store, channel_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!ccord_store_channel_pins_has_more(s->store, channel_id)) return CCORD_OK;
  const char *cursor = ccord_store_channel_pins_cursor(s->store, channel_id);
  if (!cursor || cursor[0] == '\0') return CCORD_OK;
  if (pins_is_in_flight(s, channel_id)) return CCORD_OK;
  ccord_status st = ccord_transport_request_pins_before(s->transport, channel_id, cursor, 50u, err);
  if (st == CCORD_OK) pins_mark_in_flight(s, channel_id);
  return st;
}

bool ccord_session_pins_loading(const ccord_session *s, ccord_id channel_id)
{
  return pins_is_in_flight(s, channel_id);
}

ccord_status ccord_session_request_history_before(ccord_session *s, ccord_id channel_id, ccord_id before_id,
                                                  ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || before_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: history ids");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  session_channel_hist *chist = sess_chan_hist_for(s, channel_id);
  if (chist) {
    if (!chist->has_more) return CCORD_OK;
    if (chist->in_flight_before_id != CCORD_ID_NONE) return CCORD_OK;
  }
  ccord_status st =
    ccord_transport_request_history_before(s->transport, channel_id, before_id, CCORD_SESSION_HISTORY_LIMIT, err);
  if (st == CCORD_OK && chist) {
    chist->in_flight_before_id = before_id;
  }
  return st;
}

bool ccord_session_history_has_more(const ccord_session *s, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return false;
  if (!ccord_store_channel(s->store, channel_id)) return false;
  const session_channel_hist *chist = sess_find_chan_hist(s, channel_id);
  if (chist) return chist->has_more;
  if (ccord_store_history_loaded(s->store, channel_id)) {
    return ccord_store_message_count(s->store, channel_id) >= CCORD_SESSION_HISTORY_LIMIT;
  }
  return true;
}


ccord_status ccord_session_send_message_with_files(ccord_session *s, ccord_id channel_id,
                                                   ccord_id reply_to_id, bool mention_author,
                                                   const char *utf8, size_t len,
                                                   const ccord_upload_file *files, size_t file_count,
                                                   ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: too many attachments");
  }
  if (!files || file_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty attachment list");
  }
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: empty file data");
    }
  }
  if (len > CCORD_SEND_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  }
  if (len > 0 && (!utf8 || memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is not valid text");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  if (reply_to_id != CCORD_ID_NONE) {
    const ccord_message *rt = ccord_store_find_message(s->store, channel_id, reply_to_id);
    if (!rt) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown reply target");
    if (ccord_message_is_ephemeral(rt)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "cannot reply to ephemeral message");
    }
  }
  return ccord_transport_send_message_with_files(s->transport, channel_id, reply_to_id, mention_author,
                                                 utf8 ? utf8 : "", len, files, file_count, err);
}

ccord_status ccord_session_send_poll(ccord_session *s, ccord_id channel_id, const ccord_poll_create *poll,
                                     ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!poll) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null poll");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before posting a poll.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  return ccord_transport_create_poll(s->transport, channel_id, poll, err);
}

ccord_status ccord_session_report_menu_request(ccord_session *s, ccord_report_type type, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!ccord_report_type_name(type)) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad report type");
  return ccord_transport_request_report_menu(s->transport, type, err);
}

ccord_status ccord_session_report_submit(ccord_session *s, const ccord_report_request *req, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!req || !ccord_report_type_name(req->type)) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad report");
  ccord_id self = ccord_store_self_id(s->store);
  if (req->type == CCORD_REPORT_MESSAGE) {
    const ccord_message *m = req->channel_id != CCORD_ID_NONE && req->message_id != CCORD_ID_NONE
                               ? ccord_store_find_message(s->store, req->channel_id, req->message_id)
                               : nullptr;
    if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "That message is no longer available to report.");
    if (m->send_state != CCORD_MSG_SEND_CONFIRMED || ccord_message_is_ephemeral(m)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "That message can't be reported.");
    }
    if (m->author_id == self) return ccord_error_set(err, CCORD_ERR_INVALID, "You can't report your own message.");
  } else {
    if (req->reported_user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad report");
    if (req->reported_user_id == self) return ccord_error_set(err, CCORD_ERR_INVALID, "You can't report yourself.");
  }
  return ccord_transport_submit_report(s->transport, req, err);
}

ccord_status ccord_session_send_message_with_file(ccord_session *s, ccord_id channel_id,
                                                  const char *utf8, size_t len,
                                                  const char *filename, const void *file_data,
                                                  size_t file_size, ccord_error *err)
{
  ccord_upload_file single = {
    .filename = filename,
    .data = file_data,
    .size = file_size,
    .spoiler = false,
  };
  return ccord_session_send_message_with_files(s, channel_id, CCORD_ID_NONE, true, utf8, len, &single, 1u, err);
}

ccord_status ccord_session_send_voice_message(ccord_session *s, ccord_id channel_id,
                                              const uint8_t *ogg, size_t len, uint32_t duration_ms,
                                              const char *waveform_b64, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !ogg) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (len == 0 || duration_ms == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "voice message: empty clip or duration");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  ccord_id self = ccord_store_self_id(s->store);
  if (ch->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
    uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, channel_id, self, session_now_unix_ms(s));
    uint64_t send_bit = ccord_discord_type_is_thread(ch->discord_type) ? CCORD_PERM_SEND_MESSAGES_IN_THREADS
                                                                       : CCORD_PERM_SEND_MESSAGES;
    if ((ch_perms & send_bit) == 0u || (ch_perms & CCORD_PERM_ATTACH_FILES) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION,
                             "You need Send Messages and Attach Files permissions in this channel.");
    }
  }
  ccord_upload_file voice = {
    .filename = "voice-message.ogg",
    .data = ogg,
    .size = len,
    .spoiler = false,
    .duration_ms = duration_ms,
    .waveform_b64 = waveform_b64,
  };
  return ccord_session_send_message_with_files(s, channel_id, CCORD_ID_NONE, true, "", 0, &voice, 1u, err);
}

size_t ccord_session_active_uploads(const ccord_session *s, ccord_id channel_id,
                                    ccord_upload_progress *out, size_t max)
{
  if (!s || !s->transport) return 0;
  if (channel_id == CCORD_ID_NONE) {
    return ccord_transport_active_uploads(s->transport, out, max);
  }
  ccord_upload_progress buf[32];
  size_t n = ccord_transport_active_uploads(s->transport, buf, sizeof buf / sizeof buf[0]);
  size_t scanned = n < (sizeof buf / sizeof buf[0]) ? n : (sizeof buf / sizeof buf[0]);
  size_t matched = 0;
  for (size_t i = 0; i < scanned; i++) {
    if (buf[i].channel_id != channel_id) continue;
    if (out && matched < max) out[matched] = buf[i];
    matched++;
  }
  return matched;
}


// ---- messaging parity: TTS, forward, crosspost, mark unread, saved messages ----

static const ccord_message *find_known_message(const ccord_session *s, ccord_id channel_id, ccord_id message_id)
{
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (m) return m;
  for (size_t i = 0; i < s->search_count; i++) {
    if (s->search_results[i].channel_id == channel_id && s->search_results[i].id == message_id) {
      return &s->search_results[i];
    }
  }
  size_t nsaved = ccord_store_saved_message_count(s->store);
  for (size_t i = 0; i < nsaved; i++) {
    const ccord_message *sm = ccord_store_saved_message_at(s->store, i);
    if (sm && sm->channel_id == channel_id && sm->id == message_id) return sm;
  }
  return nullptr;
}

ccord_status ccord_session_send_tts_message(ccord_session *s, ccord_id channel_id, const char *utf8, size_t len,
                                            ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s || !utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (len > CCORD_SEND_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is longer than %u bytes", CCORD_SEND_MAX_BYTES);
  }
  if (sess_blank(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "message is empty");
  if (memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message is not valid text");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  ccord_id self = ccord_store_self_id(s->store);
  if (ch->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
    uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, channel_id, self, session_now_unix_ms(s));
    uint64_t send_bit = ccord_discord_type_is_thread(ch->discord_type) ? CCORD_PERM_SEND_MESSAGES_IN_THREADS
                                                                       : CCORD_PERM_SEND_MESSAGES;
    if ((ch_perms & send_bit) == 0u || (ch_perms & CCORD_PERM_SEND_TTS_MESSAGES) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Send TTS Messages permission in this channel.");
    }
  }
  return ccord_transport_send_tts_message(s->transport, channel_id, utf8, len, err);
}

ccord_status ccord_session_forward_message(ccord_session *s, ccord_id target_channel_id, ccord_id source_channel_id,
                                           ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (target_channel_id == CCORD_ID_NONE || source_channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid forward ids");
  }
  const ccord_channel *dst = ccord_store_channel(s->store, target_channel_id);
  if (!dst) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown target channel");
  if (dst->kind != CCORD_CHANNEL_TEXT || ccord_store_channel_is_forum(s->store, target_channel_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "target channel is not a text channel");
  }
  if (dst->guild_id == CCORD_DM_GUILD_ID && dst->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, dst, err);
  if (gate != CCORD_OK) return gate;
  const ccord_channel *src = ccord_store_channel(s->store, source_channel_id);
  if (!src) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown source channel");
  const ccord_message *m = find_known_message(s, source_channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message cannot be forwarded");
  }
  ccord_id self = ccord_store_self_id(s->store);
  if (src->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, src->guild_id, self)) {
    uint64_t src_perms = ccord_store_permissions(s->store, src->guild_id, source_channel_id, self, session_now_unix_ms(s));
    if ((src_perms & (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_READ_MESSAGE_HISTORY)) !=
        (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_READ_MESSAGE_HISTORY)) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION,
                             "You need Read Message History permission in the source channel.");
    }
  }
  if (dst->guild_id != CCORD_DM_GUILD_ID && ccord_store_permissions_exact(s->store, dst->guild_id, self)) {
    uint64_t dst_perms = ccord_store_permissions(s->store, dst->guild_id, target_channel_id, self, session_now_unix_ms(s));
    uint64_t send_bit = ccord_discord_type_is_thread(dst->discord_type) ? CCORD_PERM_SEND_MESSAGES_IN_THREADS
                                                                        : CCORD_PERM_SEND_MESSAGES;
    if ((dst_perms & send_bit) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION, "You need Send Messages permission in the target channel.");
    }
  }
  return ccord_transport_forward_message(s->transport, target_channel_id, source_channel_id, message_id, err);
}

ccord_status ccord_session_crosspost_message(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                             ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid crosspost ids");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (ch->discord_type != CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "Only announcement channel messages can be published.");
  }
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message has not been sent yet");
  }
  ccord_id self = ccord_store_self_id(s->store);
  if (ccord_store_permissions_exact(s->store, ch->guild_id, self)) {
    uint64_t ch_perms = ccord_store_permissions(s->store, ch->guild_id, channel_id, self, session_now_unix_ms(s));
    if (m->author_id == self) {
      if ((ch_perms & CCORD_PERM_SEND_MESSAGES) == 0u) {
        return ccord_error_set(err, CCORD_ERR_PERMISSION,
                               "You need Send Messages permission to publish your message.");
      }
    } else if ((ch_perms & CCORD_PERM_MANAGE_MESSAGES) == 0u) {
      return ccord_error_set(err, CCORD_ERR_PERMISSION,
                             "You need Manage Messages permission to publish messages from others.");
    }
  }
  return ccord_transport_crosspost_message(s->transport, channel_id, message_id, err);
}

ccord_status ccord_session_mark_unread(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: mark unread ids");
  }
  if (!ccord_store_channel(s->store, channel_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  }
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message cannot be marked unread");
  }
  ccord_status st = ccord_transport_mark_unread(s->transport, channel_id, message_id, err);
  if (st != CCORD_OK) return st;
  return ccord_store_mark_unread(s->store, channel_id, message_id, err);
}

ccord_status ccord_session_save_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: save message ids");
  }
  if (!ccord_store_channel(s->store, channel_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  }
  const ccord_message *m = find_known_message(s, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown message");
  if (session_is_local_echo_id(m->id) || ccord_message_is_ephemeral(m)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "message cannot be saved");
  }
  return ccord_store_save_message(s->store, m, err);
}

ccord_status ccord_session_unsave_message(ccord_session *s, ccord_id channel_id, ccord_id message_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: unsave message ids");
  }
  return ccord_store_unsave_message(s->store, channel_id, message_id, err);
}

bool ccord_session_message_saved(const ccord_session *s, ccord_id channel_id, ccord_id message_id)
{
  return s ? ccord_store_message_saved(s->store, channel_id, message_id) : false;
}

size_t ccord_session_saved_message_count(const ccord_session *s)
{
  return s ? ccord_store_saved_message_count(s->store) : 0u;
}

const ccord_message *ccord_session_saved_message_at(const ccord_session *s, size_t i)
{
  return s ? ccord_store_saved_message_at(s->store, i) : nullptr;
}


