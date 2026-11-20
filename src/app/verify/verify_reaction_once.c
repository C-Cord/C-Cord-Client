// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Own-reaction counting verification (--verify-slice reaction-once); see verify_ui_parity.h.
//
// Adding your own reaction reaches the store twice: the REST result (the transport's refreshed message, or a +1 delta
// when the refresh failed) and the gateway MESSAGE_REACTION_ADD echo, in either order. Each must change the count by
// exactly one. The slice applies both to the real store through the same calls the session makes for a
// REACTION_UPDATE event (set_reactions + set_reaction_ex for an authoritative message, adjust_reaction_ex for a delta).
#include "verify_ui_parity.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/error.h>

#define RO_EMOJI "\xF0\x9F\xA7\xAA"   // test tube

typedef struct ro {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
  ccord_id ch;
  ccord_id other;   // an author that is not self
} ro;

static bool ro_check(ro *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

// A fresh message without reactions; returns its id (0 on failure).
static ccord_id ro_message(ro *s)
{
  size_t n = ccord_store_message_count(s->c->store, s->ch);
  const ccord_message *last = n ? ccord_store_message_at(s->c->store, s->ch, n - 1) : nullptr;
  ccord_message m = {.id = last ? last->id + 1 : 1, .channel_id = s->ch, .author_id = s->other,
                     .timestamp_ms = last ? last->timestamp_ms + 60000 : 1, .content = "reaction target"};
  ccord_error err;
  return ccord_store_append_message(s->c->store, &m, &err) == CCORD_OK ? m.id : 0;
}

static const ccord_reaction *ro_find(const ro *s, ccord_id msg)
{
  const ccord_message *m = ccord_store_find_message(s->c->store, s->ch, msg);
  for (size_t i = 0; m && i < m->reaction_kind_count; i++) {
    if (m->reactions[i].emoji && strcmp(m->reactions[i].emoji, RO_EMOJI) == 0) return &m->reactions[i];
  }
  return nullptr;
}

static uint32_t ro_count(const ro *s, ccord_id msg)
{
  const ccord_reaction *r = ro_find(s, msg);
  return r ? r->count : 0u;
}

static bool ro_self(const ro *s, ccord_id msg)
{
  const ccord_reaction *r = ro_find(s, msg);
  return r && r->self_reacted;
}

// The refreshed message after the REST call: the authoritative count and the own flag together.
static void ro_rest(ro *s, ccord_id msg, uint32_t count, bool self)
{
  ccord_error err;
  (void)ccord_store_set_reactions(s->c->store, s->ch, msg, 0, false, &err);
  if (count > 0) (void)ccord_store_set_reaction_ex(s->c->store, s->ch, msg, RO_EMOJI, count, self, 0, false, &err);
}

// A +1/-1 delta: the gateway echo (own or someone else's), or the REST result when the refresh failed.
static void ro_delta(ro *s, ccord_id msg, int32_t delta, bool is_self, bool burst)
{
  ccord_error err;
  (void)ccord_store_adjust_reaction_ex(s->c->store, s->ch, msg, RO_EMOJI, delta, is_self, burst, &err);
}

static int ro_finish(ro *s)
{
  if (s->fail[0]) {
    printf("reaction-once: FAIL %s\n", s->fail);
    fflush(stdout);
    return 1;
  }
  printf("reaction-once: PASS checks=%d\n", s->checks);
  fflush(stdout);
  return 0;
}

int ccord_verify_reaction_once_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  ro s = {.c = ctx};
  ctx->frames(ctx->ud, 10);
  ccord_id g0 = ccord_store_selected_guild(ctx->store);
  ccord_error err;
  // The last plain text channel that has history (it arrives asynchronously after a select).
  for (size_t i = ccord_store_channel_count(ctx->store, g0); i > 0; i--) {
    const ccord_channel *ch = ccord_store_channel_at(ctx->store, g0, i - 1);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ch->discord_type != 0) continue;
    if (ccord_session_select_channel(ctx->session, ch->id, &err) != CCORD_OK) continue;
    ctx->frames(ctx->ud, 20);
    if (ccord_store_message_count(ctx->store, ch->id) > 0) break;
  }
  s.ch = ccord_store_selected_channel(ctx->store);
  s.other = ccord_store_self_id(ctx->store) + 1;
  if (!ro_check(&s, s.ch != CCORD_ID_NONE, "mock channel selected")) return ro_finish(&s);

  // Add: REST result, then the own echo.
  ccord_id a = ro_message(&s);
  if (!ro_check(&s, a != 0 && ro_count(&s, a) == 0, "fresh message has no reaction")) return ro_finish(&s);
  ro_rest(&s, a, 1, true);
  ro_delta(&s, a, 1, true, false);
  if (ro_check(&s, ro_count(&s, a) == 1 && ro_self(&s, a), "rest then echo count")) {
    printf("reaction-once: rest then echo count=%u\n", ro_count(&s, a));
  }

  // Add: echo first, then the REST result.
  ccord_id b = ro_message(&s);
  ro_delta(&s, b, 1, true, false);
  ro_rest(&s, b, 1, true);
  if (ro_check(&s, ro_count(&s, b) == 1 && ro_self(&s, b), "echo then rest count")) {
    printf("reaction-once: echo then rest count=%u\n", ro_count(&s, b));
  }

  // Add: refresh failed (the REST result is a +1 delta), echo after and echo before.
  ccord_id c = ro_message(&s);
  ro_delta(&s, c, 1, true, false);   // failed-refresh delta
  ro_delta(&s, c, 1, true, false);   // echo
  ccord_id c2 = ro_message(&s);
  ro_delta(&s, c2, 1, true, false);   // echo
  ro_delta(&s, c2, 1, true, false);   // failed-refresh delta
  if (ro_check(&s, ro_count(&s, c) == 1 && ro_count(&s, c2) == 1 && ro_self(&s, c) && ro_self(&s, c2),
               "refresh failed count")) {
    printf("reaction-once: refresh failed count=%u\n", ro_count(&s, c));
  }

  // Remove: back to 0 in both orders, and with the REST refresh failing.
  bool ok = true;
  ro_rest(&s, a, 0, false);
  ro_delta(&s, a, -1, true, false);
  ok &= ro_count(&s, a) == 0 && !ro_self(&s, a);
  ro_delta(&s, b, -1, true, false);
  ro_rest(&s, b, 0, false);
  ok &= ro_count(&s, b) == 0 && !ro_self(&s, b);
  ro_delta(&s, c, -1, true, false);
  ro_delta(&s, c, -1, true, false);
  ok &= ro_count(&s, c) == 0;
  // Other users: their add raises the count, my add and remove move it by one on top, my flag never leaks.
  ccord_id d = ro_message(&s);
  ro_delta(&s, d, 1, false, false);
  ro_delta(&s, d, 1, false, false);
  ok &= ro_count(&s, d) == 2 && !ro_self(&s, d);
  ro_rest(&s, d, 3, true);
  ro_delta(&s, d, 1, true, false);
  ok &= ro_count(&s, d) == 3 && ro_self(&s, d);
  ro_rest(&s, d, 2, false);
  ro_delta(&s, d, -1, true, false);
  ok &= ro_count(&s, d) == 2 && !ro_self(&s, d);
  ro_delta(&s, d, -1, false, false);
  ok &= ro_count(&s, d) == 1;
  // Own add from another device: only the echo arrives and the flag is not yet set.
  ccord_id e = ro_message(&s);
  ro_delta(&s, e, 1, true, false);
  ok &= ro_count(&s, e) == 1 && ro_self(&s, e);
  // Burst and normal reactions are tracked apart: one of each from me makes 2, each echo changes nothing.
  ccord_id f = ro_message(&s);
  ro_delta(&s, f, 1, true, false);
  ro_delta(&s, f, 1, true, true);
  ro_delta(&s, f, 1, true, false);
  ro_delta(&s, f, 1, true, true);
  const ccord_reaction *fr = ro_find(&s, f);
  ok &= fr && fr->count == 2 && fr->burst_count == 1 && fr->self_reacted && fr->me_burst;
  if (ro_check(&s, ok, "remove and other users")) printf("reaction-once: remove and other users ok\n");

  return ro_finish(&s);
}
