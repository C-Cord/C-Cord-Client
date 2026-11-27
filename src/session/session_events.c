// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <ccord/core/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void flush_seeded_members(ccord_store *store, ccord_id gid, const ccord_member *batch, size_t nbatch,
                                 bool fallback_per_item)
{
  if (nbatch == 0) return;
  ccord_error ignored;
  if (ccord_store_upsert_guild_members(store, gid, batch, nbatch, nullptr, 0, &ignored) != CCORD_OK &&
      fallback_per_item && nbatch > 1u) {
    for (size_t i = 0; i < nbatch; i++) {
      (void)ccord_store_upsert_guild_members(store, gid, &batch[i], 1u, nullptr, 0, &ignored);
    }
  }
}

static void seed_channel_message_members(ccord_session *s, ccord_id channel_id, ccord_id guild_hint,
                                         const ccord_message *msgs, size_t msg_count,
                                         const ccord_member *ev_members, size_t ev_member_count)
{
  if (!s || !s->store) return;
  const ccord_channel *ch = (channel_id != CCORD_ID_NONE) ? ccord_store_channel(s->store, channel_id) : nullptr;
  ccord_id gid = ch ? ch->guild_id : guild_hint;
  if (gid == CCORD_ID_NONE || gid == CCORD_DM_GUILD_ID) return;
  if (!ccord_store_guild(s->store, gid)) return;

  ccord_member batch[64];
  size_t nbatch = 0;
  if (ev_members && ev_member_count > 0) {
    for (size_t i = 0; i < ev_member_count; i++) {
      if (ev_members[i].guild_id == gid && ccord_store_user(s->store, ev_members[i].user_id)) {
        batch[nbatch++] = ev_members[i];
        if (nbatch == sizeof batch / sizeof batch[0]) {
          flush_seeded_members(s->store, gid, batch, nbatch, true);
          nbatch = 0;
        }
      }
    }
    flush_seeded_members(s->store, gid, batch, nbatch, true);
    nbatch = 0;
  }
  if (!msgs || msg_count == 0) return;
  for (size_t i = 0; i < msg_count; i++) {
    ccord_id uid = msgs[i].author_id;
    if (uid == CCORD_ID_NONE || !ccord_store_user(s->store, uid)) continue;
    // Seeing an author says nothing new about a member we know: the sighting has no nickname or roles, so it must not
    // overwrite the ones the list already has.
    if (ccord_store_find_member(s->store, gid, uid)) continue;
    bool dup = false;
    for (size_t b = 0; b < nbatch; b++) {
      if (batch[b].user_id == uid) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    batch[nbatch++] = (ccord_member){
      .guild_id = gid,
      .user_id = uid,
      .role_name = "Members",
      .role_color = 0,
      .role_rank = 255,
    };
    if (nbatch == sizeof batch / sizeof batch[0]) {
      flush_seeded_members(s->store, gid, batch, nbatch, false);
      nbatch = 0;
    }
  }
  flush_seeded_members(s->store, gid, batch, nbatch, false);
}

// Applies the emoji/sticker lists carried by SNAPSHOT / GUILD_UPSERT / GUILD_EXPRESSIONS events. The lists are grouped
// by guild (each entry carries its guild_id); `fallback_guild` receives an empty replacement when the payload said the
// list is authoritative (replace flag) but contained no entries.
static void apply_expression_lists(ccord_session *s, const ccord_net_event *ev, ccord_id fallback_guild)
{
  ccord_error ignored;
  for (size_t i = 0; i < ev->emoji_count;) {
    size_t j = i;
    while (j < ev->emoji_count && ev->emojis[j].guild_id == ev->emojis[i].guild_id) j++;
    if (ev->emojis[i].guild_id != CCORD_ID_NONE) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_emojis_admin(s->store, ev->emojis[i].guild_id, &ev->emojis[i], j - i, &ignored);
      } else {
        (void)ccord_store_set_guild_emojis(s->store, ev->emojis[i].guild_id, &ev->emojis[i], j - i, &ignored);
      }
    }
    i = j;
  }
  for (size_t i = 0; i < ev->sticker_count;) {
    size_t j = i;
    while (j < ev->sticker_count && ev->stickers[j].guild_id == ev->stickers[i].guild_id) j++;
    if (ev->stickers[i].guild_id != CCORD_ID_NONE) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_stickers_admin(s->store, ev->stickers[i].guild_id, &ev->stickers[i], j - i, &ignored);
      } else {
        (void)ccord_store_set_guild_stickers(s->store, ev->stickers[i].guild_id, &ev->stickers[i], j - i, &ignored);
      }
    }
    i = j;
  }
  for (size_t i = 0; i < ev->soundboard_count;) {
    size_t j = i;
    while (j < ev->soundboard_count && ev->soundboard[j].guild_id == ev->soundboard[i].guild_id) j++;
    if (ev->soundboard[i].guild_id != CCORD_ID_NONE) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_soundboard_admin(s->store, ev->soundboard[i].guild_id, &ev->soundboard[i], j - i,
                                                     &ignored);
      } else {
        (void)ccord_store_set_guild_soundboard(s->store, ev->soundboard[i].guild_id, &ev->soundboard[i], j - i,
                                               &ignored);
      }
    }
    i = j;
  }
  if (fallback_guild != CCORD_ID_NONE) {
    if (ev->replace_emojis && ev->emoji_count == 0) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_emojis_admin(s->store, fallback_guild, nullptr, 0, &ignored);
      } else {
        (void)ccord_store_set_guild_emojis(s->store, fallback_guild, nullptr, 0, &ignored);
      }
    }
    if (ev->replace_stickers && ev->sticker_count == 0) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_stickers_admin(s->store, fallback_guild, nullptr, 0, &ignored);
      } else {
        (void)ccord_store_set_guild_stickers(s->store, fallback_guild, nullptr, 0, &ignored);
      }
    }
    if (ev->replace_soundboard && ev->soundboard_count == 0) {
      if (ev->expressions_admin) {
        (void)ccord_store_set_guild_soundboard_admin(s->store, fallback_guild, nullptr, 0, &ignored);
      } else {
        (void)ccord_store_set_guild_soundboard(s->store, fallback_guild, nullptr, 0, &ignored);
      }
    }
  }
}

// Applies the roles, member role ids and channel overwrites an event carries. Roles are grouped by guild (each
// carries its guild_id); an event without them changes nothing (unknown is not "none").
static void apply_permission_data(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error ignored;
  for (size_t i = 0; i < ev->role_count;) {
    size_t j = i;
    while (j < ev->role_count && ev->roles[j].guild_id == ev->roles[i].guild_id) j++;
    if (ev->roles[i].guild_id != CCORD_ID_NONE) {
      (void)ccord_store_set_guild_roles(s->store, ev->roles[i].guild_id, &ev->roles[i], j - i, &ignored);
    }
    i = j;
  }
  for (size_t i = 0; i < ev->member_role_count; i++) {
    const ccord_member_roles *m = &ev->member_roles[i];
    (void)ccord_store_set_member_roles(s->store, m->guild_id, m->user_id, m->role_ids, m->count, &ignored);
  }
  for (size_t i = 0; i < ev->overwrite_count; i++) {
    const ccord_channel_overwrites *o = &ev->overwrites[i];
    const ccord_channel *c = ccord_store_channel(s->store, o->channel_id);
    if (c) (void)ccord_store_set_channel_overwrites(s->store, o->channel_id, c->guild_id, o->items, o->count, &ignored);
  }
}

// Stores the notification rows an event carries. Rows come grouped by server and each group replaces what was known of
// that server (a READY carries them whole; the store was cleared by the snapshot).
static void apply_notify_rows(ccord_session *s, const ccord_notify_scope *rows, size_t n)
{
  ccord_error ignored;
  for (size_t i = 0; i < n;) {
    size_t j = i;
    while (j < n && rows[j].guild_id == rows[i].guild_id) j++;
    if (ccord_store_set_notify(s->store, rows[i].guild_id, &rows[i], j - i, &ignored) != CCORD_OK) {
      CCORD_LOG_W("session: notification settings rejected: %s", ignored.message);
    }
    i = j;
  }
}

// What applies to a channel right now. When the user never chose an override level, a guild channel falls back to its
// server's default_message_notifications (0 = All Messages, 1 = Only @mentions) when guild settings are known, and
// otherwise notifies for mentions and direct messages only.
static ccord_notify_effective notify_effective(const ccord_session *s, ccord_id channel_id)
{
  ccord_notify_effective eff = ccord_store_channel_notify(s->store, channel_id, ccord_clock_unix_ms());
  if (!eff.level_set) {
    const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
    const ccord_guild *g = (ch && ch->guild_id != CCORD_ID_NONE && ch->guild_id != CCORD_DM_GUILD_ID)
                               ? ccord_store_guild(s->store, ch->guild_id)
                               : nullptr;
    if (g && g->has_guild_settings) {
      eff.level = g->default_message_notifications == 0 ? CCORD_NOTIFY_ALL : CCORD_NOTIFY_MENTIONS;
    } else {
      eff.level = CCORD_NOTIFY_MENTIONS;
    }
  }
  return eff;
}

// Fixed-size inline text from a transport is only trusted when a terminator sits inside its buffer: a
// malformed row must compare unequal rather than read past the array.

typedef struct sess_prev_chan {
  ccord_id channel_id;
  ccord_id last_message_id;
  ccord_id last_read_id;
  uint32_t mention_count;
} sess_prev_chan;

static int cmp_prev_chan_id(const void *a, const void *b)
{
  const sess_prev_chan *x = a;
  const sess_prev_chan *y = b;
  if (x->channel_id < y->channel_id) return -1;
  if (x->channel_id > y->channel_id) return 1;
  return 0;
}

static const sess_prev_chan *find_prev_chan(const sess_prev_chan *arr, size_t n, ccord_id cid)
{
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (arr[mid].channel_id < cid) lo = mid + 1u;
    else if (arr[mid].channel_id > cid) hi = mid;
    else return &arr[mid];
  }
  return nullptr;
}

static const session_wake_catchup *find_pending_catchup(const ccord_session *s, ccord_id cid)
{
  if (!s) return nullptr;
  for (size_t i = 0; i < s->nwake_catchup; i++) {
    if (s->wake_catchup[i].channel_id == cid) return &s->wake_catchup[i];
  }
  return nullptr;
}

// Captures every channel's last_message_id / last_read_id / mention_count before a background READY
// replaces the store, so we can detect messages/mentions that arrived while suspended or offline.
// If a channel still has an unfinished wake catch-up from an earlier READY, preserve its older
// after_msg_id watermark so back-to-back READY events cannot drop the pending notification.
static sess_prev_chan *sess_snapshot_capture_prev(const ccord_session *s, size_t *out_n, ccord_id *out_max_msg)
{
  *out_n = 0;
  *out_max_msg = CCORD_ID_NONE;
  const ccord_store *store = s ? s->store : nullptr;
  if (!store) return nullptr;
  size_t ng = ccord_store_guild_count(store);
  size_t total = ccord_store_channel_count(store, CCORD_DM_GUILD_ID);
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(store, gi);
    if (g) total += ccord_store_channel_count(store, g->id);
  }
  if (total == 0) return nullptr;
  sess_prev_chan *arr = ccord_malloc(total * sizeof *arr);
  if (!arr) return nullptr;
  size_t w = 0;
  ccord_id max_msg = CCORD_ID_NONE;
  size_t ndm = ccord_store_channel_count(store, CCORD_DM_GUILD_ID);
  for (size_t ci = 0; ci < ndm && w < total; ci++) {
    const ccord_channel *c = ccord_store_channel_at(store, CCORD_DM_GUILD_ID, ci);
    if (!c) continue;
    ccord_id lmid = c->last_message_id < (UINT64_C(1) << 62) ? c->last_message_id : CCORD_ID_NONE;
    if (lmid > max_msg) max_msg = lmid;
    const session_wake_catchup *pend = find_pending_catchup(s, c->id);
    ccord_id prev_lmid = (pend && pend->after_msg_id < lmid) ? pend->after_msg_id : lmid;
    uint32_t prev_mc = (pend && pend->has_new_mention) ? 0u : c->mention_count;
    arr[w++] = (sess_prev_chan){
      .channel_id = c->id,
      .last_message_id = prev_lmid,
      .last_read_id = c->last_read_id,
      .mention_count = prev_mc,
    };
  }
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(store, gi);
    if (!g) continue;
    size_t nc = ccord_store_channel_count(store, g->id);
    for (size_t ci = 0; ci < nc && w < total; ci++) {
      const ccord_channel *c = ccord_store_channel_at(store, g->id, ci);
      if (!c) continue;
      ccord_id lmid = c->last_message_id < (UINT64_C(1) << 62) ? c->last_message_id : CCORD_ID_NONE;
      if (lmid > max_msg) max_msg = lmid;
      const session_wake_catchup *pend = find_pending_catchup(s, c->id);
      ccord_id prev_lmid = (pend && pend->after_msg_id < lmid) ? pend->after_msg_id : lmid;
      uint32_t prev_mc = (pend && pend->has_new_mention) ? 0u : c->mention_count;
      arr[w++] = (sess_prev_chan){
        .channel_id = c->id,
        .last_message_id = prev_lmid,
        .last_read_id = c->last_read_id,
        .mention_count = prev_mc,
      };
    }
  }
  if (w > 1u) qsort(arr, w, sizeof *arr, cmp_prev_chan_id);
  *out_n = w;
  *out_max_msg = max_msg;
  return arr;
}

static bool sess_take_wake_catchup(ccord_session *s, ccord_id channel_id, session_wake_catchup *out)
{
  for (size_t i = 0; i < s->nwake_catchup; i++) {
    if (s->wake_catchup[i].channel_id == channel_id) {
      if (out) *out = s->wake_catchup[i];
      if (i + 1u < s->nwake_catchup) {
        memmove(&s->wake_catchup[i], &s->wake_catchup[i + 1u],
                (s->nwake_catchup - i - 1u) * sizeof s->wake_catchup[0]);
      }
      s->nwake_catchup--;
      return true;
    }
  }
  return false;
}

static void sess_wake_catchup_emit_fallback(ccord_session *s, const session_wake_catchup *cu)
{
  if (!s || !cu || !s->in_background) return;
  const ccord_channel *c = ccord_store_channel(s->store, cu->channel_id);
  if (!c || c->is_spam || c->is_message_request) return;
  ccord_id floor_id = cu->after_msg_id;
  if (c->last_read_id != CCORD_ID_NONE && c->last_read_id > floor_id) floor_id = c->last_read_id;
  ccord_id ln = sess_last_notified_id(s, cu->channel_id);
  if (cu->target_msg_id == CCORD_ID_NONE || cu->target_msg_id <= floor_id || cu->target_msg_id == ln) return;
  ccord_id author_id = cu->is_dm ? c->user_id : CCORD_ID_NONE;
  if (author_id != CCORD_ID_NONE) {
    ccord_relationship_kind rel = ccord_store_relationship(s->store, author_id);
    if (rel == CCORD_REL_BLOCKED || rel == CCORD_REL_IGNORED) return;
  }
  sess_queue_notification(s, (ccord_notification){
    .guild_id = c->guild_id,
    .channel_id = c->id,
    .message_id = cu->target_msg_id,
    .author_id = author_id,
    .is_dm = cu->is_dm,
    .is_mention = cu->has_new_mention,
  });
}

static void sess_wake_catchup_from_store(ccord_session *s, const session_wake_catchup *cu, bool allow_overflow_fallback)
{
  if (!s || !cu || !s->in_background) return;
  const ccord_channel *c = ccord_store_channel(s->store, cu->channel_id);
  if (!c || c->is_spam || c->is_message_request) return;
  ccord_id self_id = ccord_store_self_id(s->store);
  ccord_notify_effective eff = notify_effective(s, cu->channel_id);
  bool is_dm = (c->guild_id == CCORD_DM_GUILD_ID);

  ccord_id floor_id = cu->after_msg_id;
  if (c->last_read_id != CCORD_ID_NONE && c->last_read_id > floor_id) floor_id = c->last_read_id;
  ccord_id ln = sess_last_notified_id(s, cu->channel_id);

  size_t nmsg = ccord_store_message_count(s->store, cu->channel_id);
  enum { CATCHUP_MAX_PER_CHANNEL = 3u };
  ccord_notification picked[CATCHUP_MAX_PER_CHANNEL];
  size_t npicked = 0;

  // Only inspect messages in (floor_id, cu->target_msg_id]: anything newer arrived after READY and is
  // delivered via live MESSAGE_CREATE, so a newer live message bumping sess_last_notified_id must not
  // mask the wake-triggering message that arrived before READY.
  for (size_t i = 0; i < nmsg; i++) {
    const ccord_message *m = ccord_store_message_at(s->store, cu->channel_id, i);
    if (!m || m->id <= floor_id || m->id > cu->target_msg_id || m->id >= (UINT64_C(1) << 62)) continue;
    if (m->id == ln) continue;
    if (ccord_message_is_ephemeral(m)) continue;
    if (m->author_id == CCORD_ID_NONE || m->author_id == self_id) continue;
    ccord_relationship_kind rel = ccord_store_relationship(s->store, m->author_id);
    if (rel == CCORD_REL_BLOCKED || rel == CCORD_REL_IGNORED) continue;
    if ((ccord_message_flags(m) & CCORD_MSGFLAG_SUPPRESS_NOTIFICATIONS) != 0) continue;

    bool direct = ccord_message_mentions_user(m, self_id);
    bool everyone = m->rich && m->rich->mention_everyone;
    bool role = sess_message_pings_own_role(s, c->guild_id, m->rich);
    bool is_mention = direct || (everyone && !eff.suppress_everyone) || (role && !eff.suppress_roles);
    if (!ccord_notify_should_notify(&eff, is_dm || direct, everyone, role)) continue;

    ccord_notification note = {
      .guild_id = c->guild_id,
      .channel_id = c->id,
      .message_id = m->id,
      .author_id = m->author_id,
      .is_dm = is_dm,
      .is_mention = is_mention,
    };
    if (npicked < CATCHUP_MAX_PER_CHANNEL) {
      picked[npicked++] = note;
    } else {
      memmove(&picked[0], &picked[1], (CATCHUP_MAX_PER_CHANNEL - 1u) * sizeof picked[0]);
      picked[CATCHUP_MAX_PER_CHANNEL - 1u] = note;
    }
  }

  for (size_t i = 0; i < npicked; i++) {
    sess_queue_notification(s, picked[i]);
  }
  if (npicked == 0 && allow_overflow_fallback && cu->has_new_mention && nmsg >= CCORD_SESSION_HISTORY_LIMIT) {
    const ccord_message *oldest = ccord_store_message_at(s->store, cu->channel_id, 0);
    if (oldest && oldest->id > floor_id) {
      sess_wake_catchup_emit_fallback(s, cu);
    }
  }
}

static void sess_wake_catchup_consider(ccord_session *s, const ccord_channel *c, const sess_prev_chan *prev_chans,
                                       size_t nprev, ccord_id prev_max_msg_id)
{
  if (!c || !session_channel_has_chat(c) || c->is_spam || c->is_message_request) return;
  if (c->last_message_id == CCORD_ID_NONE || c->last_message_id >= (UINT64_C(1) << 62)) return;
  if (c->last_read_id != CCORD_ID_NONE && c->last_message_id <= c->last_read_id) return;
  if (c->last_message_id <= sess_last_notified_id(s, c->id)) return;

  bool is_dm = (c->guild_id == CCORD_DM_GUILD_ID);
  if (is_dm && c->user_id != CCORD_ID_NONE) {
    ccord_relationship_kind rel = ccord_store_relationship(s->store, c->user_id);
    if (rel == CCORD_REL_BLOCKED || rel == CCORD_REL_IGNORED) return;
  }

  const sess_prev_chan *prev = find_prev_chan(prev_chans, nprev, c->id);
  bool new_msg = prev ? (c->last_message_id > prev->last_message_id)
                      : (is_dm && prev_max_msg_id != CCORD_ID_NONE && c->last_message_id > prev_max_msg_id);
  bool new_mention = prev
                         ? (c->mention_count > prev->mention_count ||
                            (c->mention_count > 0 && c->last_message_id > prev->last_message_id &&
                             c->last_read_id > prev->last_read_id))
                         : (!is_dm && c->mention_count > 0 && prev_max_msg_id != CCORD_ID_NONE &&
                            c->last_message_id > prev_max_msg_id);
  if (!new_msg && !new_mention) return;

  ccord_notify_effective eff = notify_effective(s, c->id);
  if (eff.muted || eff.level == CCORD_NOTIFY_NOTHING) return;
  bool eligible = is_dm ? new_msg : (new_mention || (eff.level == CCORD_NOTIFY_ALL && new_msg));
  if (!eligible) return;

  ccord_id after = prev ? prev->last_message_id : prev_max_msg_id;
  if (c->last_read_id != CCORD_ID_NONE && c->last_read_id > after) after = c->last_read_id;
  ccord_id ln = sess_last_notified_id(s, c->id);
  if (ln > after) after = ln;
  if (c->last_message_id <= after) return;

  session_wake_catchup cand = {
    .channel_id = c->id,
    .guild_id = c->guild_id,
    .after_msg_id = after,
    .target_msg_id = c->last_message_id,
    .is_dm = is_dm,
    .has_new_mention = new_mention,
  };
  if (s->nwake_catchup < SESSION_WAKE_CATCHUP_MAX) {
    s->wake_catchup[s->nwake_catchup++] = cand;
  } else {
    size_t oldest = 0;
    for (size_t i = 1; i < SESSION_WAKE_CATCHUP_MAX; i++) {
      if (s->wake_catchup[i].target_msg_id < s->wake_catchup[oldest].target_msg_id) oldest = i;
    }
    if (cand.target_msg_id > s->wake_catchup[oldest].target_msg_id) {
      s->wake_catchup[oldest] = cand;
    }
  }
}

static void sess_snapshot_wake_catchup(ccord_session *s, const sess_prev_chan *prev_chans, size_t nprev,
                                       ccord_id prev_max_msg_id)
{
  if (!s || !s->in_background || !prev_chans || nprev == 0) return;
  int64_t now_unix_ms = session_now_unix_ms(s);
  ccord_id self_id = ccord_store_self_id(s->store);

  size_t ndm = ccord_store_channel_count(s->store, CCORD_DM_GUILD_ID);
  for (size_t ci = 0; ci < ndm; ci++) {
    const ccord_channel *c = ccord_store_channel_at(s->store, CCORD_DM_GUILD_ID, ci);
    sess_wake_catchup_consider(s, c, prev_chans, nprev, prev_max_msg_id);
  }

  size_t ng = ccord_store_guild_count(s->store);
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(s->store, gi);
    if (!g || ccord_store_guild_muted(s->store, g->id, now_unix_ms)) continue;
    ccord_store_guild_perm_base perm_base = {0};
    bool have_base = ccord_store_guild_base_permissions(s->store, g->id, self_id, now_unix_ms, &perm_base);
    size_t nc = ccord_store_channel_count(s->store, g->id);
    for (size_t ci = 0; ci < nc; ci++) {
      const ccord_channel *c = ccord_store_channel_at(s->store, g->id, ci);
      if (!c) continue;
      if (have_base && perm_base.exact &&
          (ccord_store_channel_permissions_with_base(s->store, &perm_base, c) & CCORD_PERM_VIEW_CHANNEL) == 0) {
        continue;
      }
      sess_wake_catchup_consider(s, c, prev_chans, nprev, prev_max_msg_id);
    }
  }

  if (s->nwake_catchup == 0) return;
  CCORD_LOG_I("session: background READY catch-up fetching %zu newly-unread channel(s)", s->nwake_catchup);

  size_t kept = 0;
  for (size_t i = 0; i < s->nwake_catchup; i++) {
    session_wake_catchup cu = s->wake_catchup[i];
    if (sess_request_history_once(s, cu.channel_id)) {
      s->wake_catchup[kept++] = cu;
    } else if (ccord_store_history_loaded(s->store, cu.channel_id)) {
      sess_wake_catchup_from_store(s, &cu, true);
    } else {
      sess_wake_catchup_emit_fallback(s, &cu);
    }
  }
  s->nwake_catchup = kept;
}

void sess_apply_event(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error err;
  switch (ev->type) {
    case CCORD_NET_STATE:
      s->state = ev->state;
      sess_post_state(s);
      if (session_is_authenticated(s)) {
        if (s->in_background) {
          s->bg_deferred_sync = true;
        } else {
          ccord_id sel_ch = ccord_store_selected_channel(s->store);
          sess_request_history_once(s, sel_ch);
          sess_maybe_request_dm_profile(s, sel_ch);
        }
      }
      if (s->state == CCORD_CONN_CONNECTED && s->rich_op3_dirty && !s->stale &&
          ccord_store_self_id(s->store) != CCORD_ID_NONE) {
        ccord_error gw_err;
        if (session_send_presence_with_custom(s, session_current_presence(s), &gw_err) == CCORD_OK) {
          s->rich_op3_dirty = false;
        }
      }
      break;
    case CCORD_NET_SNAPSHOT:
      // READY: the stale flag goes first so everything below (history, presence, profile requests) is live again.
      {
        ccord_id prev_self = ccord_store_self_id(s->store);
        ccord_id new_self = ev->snapshot ? ev->snapshot->self_id : CCORD_ID_NONE;
        bool was_stale = s->stale;
        sess_prev_chan *prev_chans = nullptr;
        size_t nprev_chans = 0;
        ccord_id prev_max_msg_id = CCORD_ID_NONE;
        if (s->in_background && !was_stale && prev_self != CCORD_ID_NONE && new_self == prev_self) {
          prev_chans = sess_snapshot_capture_prev(s, &nprev_chans, &prev_max_msg_id);
        }
        s->nwake_catchup = 0;
        s->stale = false;
        if (ccord_store_set_snapshot_ex(s->store, ev->snapshot, was_stale, &err) != CCORD_OK) {
          ccord_free(prev_chans);
          CCORD_LOG_E("session: snapshot rejected: %s", err.message);
          s->state = CCORD_CONN_FAILED;
          sess_post_state(s);
        } else {
          if (prev_self != CCORD_ID_NONE && new_self != CCORD_ID_NONE && prev_self != new_self) {
            ccord_store_clear_all_expressions(s->store);
            s->nnotified_track = 0;
          }
          ccord_store_forum_reset(s->store);
        if (ev->forum_meta_count > 0 && ev->forum_metas) {
          (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
        }
        if (ev->forum_post_count > 0 && ev->forum_posts) {
          (void)ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err);
        }
        ccord_store_clear_guild_expressions(s->store);
        apply_expression_lists(s, ev, CCORD_ID_NONE);
        apply_permission_data(s, ev);
        if (ev->notify_count > 0 && ev->notify) apply_notify_rows(s, ev->notify, ev->notify_count);
        if (ev->folder_item_count > 0 && ev->folder_items) {
          (void)ccord_store_set_guild_layout(s->store, ev->folder_items, ev->folder_item_count, &err);
        }
        for (size_t i = 0; ev->scheduled_events && i < ev->scheduled_event_count; i++) {
          (void)ccord_store_upsert_scheduled_event(s->store, &ev->scheduled_events[i], &err);
        }
        s->nrequested = 0;
        s->nchan_hist = 0;
        s->nprofile_req = 0;
        s->nslash_cache = 0;
        s->nactivity_shelves = 0;
        s->nguild_profiles = 0;
        s->nvoice_track = 0;
        s->voice_track_synced = true;
        if (ev->snapshot && ev->snapshot->voice_states) {
          for (size_t i = 0; i < ev->snapshot->voice_state_count; i++) {
            const ccord_voice_state *svs = &ev->snapshot->voice_states[i];
            if (svs->channel_id != CCORD_ID_NONE) {
              sess_voice_track_upsert(s, svs->guild_id, svs->channel_id, svs->user_id);
            }
          }
        }
        sess_reset_invite_cache(s);
        sess_reset_verification_cache(s);
        s->slash_choice_count = 0;
        s->npins_in_flight = 0;
        s->retried_channel = CCORD_ID_NONE;
        s->has_pending_presence = false;
        ccord_mutex_lock(s->audio_mtx);
        atomic_store_explicit(&s->has_voice_cfg, false, memory_order_release);
        ccord_mutex_unlock(s->audio_mtx);
        s->ptt_held = false;
        sess_reset_account_lists(s);
        if (s->state != CCORD_CONN_CONNECTED) {
          s->state = CCORD_CONN_CONNECTED;
          sess_post_state(s);
        }
        // A fresh connection starts with an empty gateway: restore the Rich Presence activities.
        session_repush_rich_presence(s);
        if (was_stale) session_replay_stale(s);
        ccord_id sel_ch = ccord_store_selected_channel(s->store);
        if (sel_ch == CCORD_ID_NONE) {
          size_t ng = ccord_store_guild_count(s->store);
          for (size_t gi = 0; gi < ng && sel_ch == CCORD_ID_NONE; gi++) {
            const ccord_guild *g = ccord_store_guild_at(s->store, gi);
            if (!g) continue;
            size_t nc = ccord_store_channel_count(s->store, g->id);
            for (size_t ci = 0; ci < nc; ci++) {
              const ccord_channel *c = ccord_store_channel_at(s->store, g->id, ci);
              if (c && c->kind == CCORD_CHANNEL_TEXT) {
                if (ccord_store_select_channel(s->store, c->id, &err) == CCORD_OK) {
                  sel_ch = c->id;
                }
                break;
              }
            }
          }
          if (sel_ch == CCORD_ID_NONE) {
            size_t ndm = ccord_store_channel_count(s->store, CCORD_DM_GUILD_ID);
            for (size_t ci = 0; ci < ndm; ci++) {
              const ccord_channel *c = ccord_store_channel_at(s->store, CCORD_DM_GUILD_ID, ci);
              if (c && c->kind == CCORD_CHANNEL_TEXT && !c->is_message_request) {
                if (ccord_store_select_channel(s->store, c->id, &err) == CCORD_OK) {
                  sel_ch = c->id;
                }
                break;
              }
            }
          }
        }
        ccord_id sel_guild = ccord_store_selected_guild(s->store);
        if (sel_guild != CCORD_ID_NONE) {
          ccord_error hook_err;
          (void)ccord_transport_select_channel(s->transport, sel_guild, sel_ch, &hook_err);
        }
        if (s->in_background) {
          s->bg_deferred_sync = true;
          if (prev_chans) sess_snapshot_wake_catchup(s, prev_chans, nprev_chans, prev_max_msg_id);
        } else {
          (void)sess_request_history_once(s, sel_ch);
          sess_maybe_request_dm_profile(s, sel_ch);
          if (s->reading_known && s->reading_channel != CCORD_ID_NONE) {
            ccord_id self_id = ccord_store_self_id(s->store);
            if (self_id != CCORD_ID_NONE && !ccord_store_user_profile(s->store, self_id)) {
              ccord_error ignored;
              (void)ccord_session_request_profile(s, self_id, &ignored);
            }
          }
        }
        ccord_free(prev_chans);
        ccord_os_trim_memory();
        }
      }
      break;
    case CCORD_NET_HISTORY: {
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      seed_channel_message_members(s, ev->channel_id, ev->guild_id, ev->messages, ev->message_count,
                                   ev->guild_members, ev->guild_member_count);
      if (!ev->history_from_cache) {
        sess_clear_requested(s, ev->channel_id);
      }
      session_channel_hist *chist = sess_chan_hist_for(s, ev->channel_id);
      if (chist) {
        if (!ev->history_from_cache) chist->in_flight_before_id = CCORD_ID_NONE;
        chist->has_more = (ev->message_count >= CCORD_SESSION_HISTORY_LIMIT);
      }
      session_wake_catchup cu = {0};
      bool had_catchup = !ev->history_from_cache && sess_take_wake_catchup(s, ev->channel_id, &cu);
      if (ccord_store_set_history(s->store, ev->channel_id, ev->messages, ev->message_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: history rejected: %s", err.message);
        if (had_catchup) sess_wake_catchup_emit_fallback(s, &cu);
      } else {
        if (!ev->history_from_cache && s->retried_channel == ev->channel_id) {
          s->retried_channel = CCORD_ID_NONE;
        }
        if (had_catchup) sess_wake_catchup_from_store(s, &cu, true);
      }
      break;
    }
    case CCORD_NET_HISTORY_FAILED: {
      sess_clear_requested(s, ev->channel_id);
      session_channel_hist *chist = sess_find_chan_hist(s, ev->channel_id);
      if (chist) {
        chist->in_flight_before_id = CCORD_ID_NONE;
      }
      session_wake_catchup cu = {0};
      if (sess_take_wake_catchup(s, ev->channel_id, &cu)) {
        sess_wake_catchup_emit_fallback(s, &cu);
      }
      break;
    }
    case CCORD_NET_MESSAGE_CREATE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      seed_channel_message_members(s, ev->channel_id, ev->guild_id, ev->messages, ev->message_count,
                                   ev->guild_members, ev->guild_member_count);
      for (size_t i = 0; i < ev->message_count; i++) {
        const ccord_message *in = &ev->messages[i];
        const ccord_channel *existing_ch = ccord_store_channel(s->store, in->channel_id);
        if (!existing_ch) {
          if (ccord_store_forum_apply_message(s->store, in, &err) == CCORD_OK) {
            continue;
          }
          if (err.code != CCORD_ERR_NOT_FOUND &&
              ccord_store_forum_find_post(s->store, in->channel_id) != nullptr) {
            continue;
          }
        } else if (ccord_discord_type_is_thread(existing_ch->discord_type) ||
                   (existing_ch->parent_id != CCORD_ID_NONE &&
                    ccord_store_channel_is_forum(s->store, existing_ch->parent_id))) {
          (void)ccord_store_forum_apply_message(s->store, in, &err);
        }
        if (!existing_ch &&
            (ev->guild_id == CCORD_ID_NONE || ev->guild_id == CCORD_DM_GUILD_ID) &&
            in->author_id != CCORD_ID_NONE && ccord_store_self_id(s->store) != CCORD_ID_NONE) {
          ccord_id dm_uid = (in->author_id != ccord_store_self_id(s->store)) ? in->author_id : CCORD_ID_NONE;
          const ccord_user *au = dm_uid != CCORD_ID_NONE ? ccord_store_user(s->store, dm_uid) : nullptr;
          const char *dm_name = (au && au->name && au->name[0] != '\0') ? au->name : "Direct Message";
          const char *dm_icon = (au && au->avatar_url) ? au->avatar_url : "";
          ccord_channel synth_dm = {
            .id = in->channel_id,
            .guild_id = CCORD_DM_GUILD_ID,
            .kind = CCORD_CHANNEL_TEXT,
            .name = dm_name,
            .topic = "",
            .last_message_id = ccord_message_is_ephemeral(in) ? CCORD_ID_NONE : in->id,
            .user_id = dm_uid,
            .icon_url = dm_icon,
            .discord_type = CCORD_DISCORD_CHANNEL_UNKNOWN,
          };
          ccord_error dm_err;
          (void)ccord_store_upsert_channel(s->store, &synth_dm, &dm_err);
          // The placeholder is named after the author, which is wrong for group DMs.
          // Ask for the real channel (name, icon, recipients); CHANNEL_UPSERT replaces it.
          (void)ccord_transport_request_channel(s->transport, in->channel_id, &dm_err);
        }
        // M0-3: a nonce-matched server echo confirms the local pending/failed row: drop it, then append the real
        // message below (dedup by id). A foreign nonce matches nothing and appends normally.
        if (in->nonce[0] != '\0') {
          session_local_echo le;
          if (sess_find_local_echo_by_nonce(s, in->channel_id, in->nonce, &le)) {
            sess_remove_local_echo(s, in->channel_id, le.id);
          }
        }
        if (ccord_store_append_message(s->store, in, &err) != CCORD_OK) {
          CCORD_LOG_W("session: message rejected: %s", err.message);
          continue;
        }
        const ccord_channel *c = ccord_store_channel(s->store, in->channel_id);
        if (c && c->guild_id != CCORD_ID_NONE && c->guild_id != CCORD_DM_GUILD_ID &&
            in->author_id != CCORD_ID_NONE && ccord_store_user(s->store, in->author_id) &&
            !ccord_store_find_member(s->store, c->guild_id, in->author_id)) {
          ccord_error m_err;
          ccord_member m = {
            .guild_id = c->guild_id,
            .user_id = in->author_id,
            .role_name = "Members",
            .role_color = 0,
            .role_rank = 255,
          };
          (void)ccord_store_upsert_guild_members(s->store, c->guild_id, &m, 1, nullptr, 0, &m_err);
        }
        // Ephemeral interaction responses exist only in local session memory: never notify, ACK, or increment unreads.
        if (ccord_message_is_ephemeral(in)) continue;
        bool is_dm = (c && c->guild_id == CCORD_DM_GUILD_ID);
        ccord_id self_id = ccord_store_self_id(s->store);
        ccord_notify_effective notify_eff = notify_effective(s, in->channel_id);
        // A mention is what Discord says it is: the user's id in the message's mention list (which includes a reply that
        // pings), or the id in the text; @everyone / @here and a held @role count unless the server's settings suppress them.
        bool direct = ccord_message_mentions_user(in, self_id);
        bool everyone = in->rich && in->rich->mention_everyone;
        bool role = sess_message_pings_own_role(s, c ? c->guild_id : CCORD_ID_NONE, in->rich);
        bool is_mention = direct || (everyone && !notify_eff.suppress_everyone) || (role && !notify_eff.suppress_roles);
        // A blocked or ignored author never notifies and never counts as unread or a mention.
        bool from_blocked_or_ignored = in->author_id != CCORD_ID_NONE &&
            (ccord_store_relationship(s->store, in->author_id) == CCORD_REL_BLOCKED ||
             ccord_store_relationship(s->store, in->author_id) == CCORD_REL_IGNORED);
        bool suppress_notifs = (ccord_message_flags(in) & CCORD_MSGFLAG_SUPPRESS_NOTIFICATIONS) != 0;
        ccord_id reading = s->reading_known ? s->reading_channel : ccord_store_selected_channel(s->store);
        if (in->author_id != CCORD_ID_NONE && in->author_id != self_id && !(c && c->is_spam) && !from_blocked_or_ignored &&
            !suppress_notifs &&
            !(in->channel_id == reading && !is_mention) &&
            ccord_notify_should_notify(&notify_eff, is_dm || direct, everyone, role)) {
          sess_queue_notification(s, (ccord_notification){
            .guild_id = c ? c->guild_id : CCORD_ID_NONE,
            .channel_id = in->channel_id,
            .message_id = in->id,
            .author_id = in->author_id,
            .is_dm = is_dm,
            .is_mention = is_mention,
          });
        }
        // The user's own message (typed here, or on another device) is read by definition: it moves the read position to it
        // and clears the channel. Anyone else's counts as unread unless the user is reading that channel right now.
        bool unjoined_thread = c && ccord_discord_type_is_thread(c->discord_type) && c->thread_has_member && !c->thread_joined;
        if (in->author_id != CCORD_ID_NONE && in->author_id == self_id) {
          if (c) (void)ccord_store_apply_read(s->store, in->channel_id, in->id, UINT32_MAX, &err);
        } else if (in->channel_id != reading && !from_blocked_or_ignored && !unjoined_thread) {
          if (c && !c->is_spam && !c->is_message_request) {
            bool dm_muted = is_dm && (notify_eff.muted || notify_eff.level == CCORD_NOTIFY_NOTHING);
            bool count_mention = (is_dm && !dm_muted) || is_mention;
            uint32_t unread = c->unread_count + 1u;
            uint32_t mentions = c->mention_count + (count_mention ? 1u : 0u);
            ccord_error ignored;
            (void)ccord_store_set_channel_counts(s->store, in->channel_id, unread, mentions, &ignored);
          }
        }
      }
      break;
    case CCORD_NET_MESSAGE_UPDATE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      for (size_t i = 0; i < ev->message_count; i++) {
        // A live update can race the initial history: silently ignore uncached messages.
        if (!ccord_store_find_message(s->store, ev->messages[i].channel_id, ev->messages[i].id)) continue;
        if (ccord_store_update_message(s->store, &ev->messages[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: message update rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_MESSAGE_DELETE:
      if (ccord_store_delete_message(s->store, ev->channel_id, ev->message_id, &err) != CCORD_OK) {
        CCORD_LOG_W("session: message delete rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MESSAGE_DELETE_BULK:
      if (ccord_store_delete_messages_bulk(s->store, ev->channel_id, ev->message_ids, ev->message_id_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: bulk message delete rejected: %s", err.message);
      }
      break;
    case CCORD_NET_PRESENCE_UPDATE:
      if (ev->user_id != CCORD_ID_NONE && ev->user_id == ccord_store_self_id(s->store)) {
        s->has_pending_presence = false;
      }
      if (ev->has_presence_detail) {
        if (ccord_store_update_presence_detail_ex(
              s->store, ev->user_id, ev->presence, ev->status_text, ev->custom_status_emoji, ev->custom_status_text,
              ev->custom_status_expires_ms, ev->has_activity, ev->activity_type, ev->activity_name,
              ev->activity_details, ev->activity_state, ev->activity_start_ms, ev->activity_end_ms,
              true, ev->client_status_bits, ev->activities_ext, &err) !=
            CCORD_OK) {
          CCORD_LOG_W("session: presence detail rejected: %s", err.message);
        }
      } else if (ccord_store_update_presence(s->store, ev->user_id, ev->presence, ev->status_text, &err) != CCORD_OK) {
        CCORD_LOG_W("session: presence rejected: %s", err.message);
      }
      break;
    case CCORD_NET_TYPING_START:
      if (ccord_store_set_typing(s->store, ev->channel_id, ev->user_id, ev->timestamp_ms, &err) != CCORD_OK) {
        CCORD_LOG_W("session: typing rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_UPSERT:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->forum_meta_count > 0 && ev->forum_metas) {
        (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts) {
        (void)ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err);
      }
      if (ev->channel && ccord_discord_type_is_thread(ev->channel->discord_type) &&
          ev->channel->parent_id != CCORD_ID_NONE &&
          ccord_store_channel_is_forum(s->store, ev->channel->parent_id) &&
          ccord_store_channel(s->store, ev->channel->id) == nullptr) {
        break;
      }
      if (!ev->channel || ccord_store_upsert_channel(s->store, ev->channel, &err) != CCORD_OK) {
        CCORD_LOG_W("session: channel upsert rejected: %s", ev->channel ? err.message : "null channel");
      } else {
        if (ev->forum_meta_count > 0 && ev->forum_metas) {
          (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
        }
        apply_permission_data(s, ev);
        if (ev->admin_op == CCORD_ADMIN_CREATE_GROUP_DM) {
          (void)ccord_session_select_channel(s, ev->channel->id, &err);
        }
        if (ccord_discord_type_is_thread(ev->channel->discord_type) && ev->channel->parent_id != CCORD_ID_NONE &&
            !ccord_store_channel_is_forum(s->store, ev->channel->parent_id)) {
          const ccord_message *starter = ccord_store_find_message(s->store, ev->channel->parent_id, ev->channel->id);
          if (starter && (!starter->rich || starter->rich->thread.id == CCORD_ID_NONE)) {
            ccord_message_rich rpatch = {
              .present = CCORD_RICH_HAS_THREAD,
              .text_absent = true,
              .thread = {
                .id = ev->channel->id,
                .name = ev->channel->name ? ev->channel->name : "Thread",
                .message_count = ev->channel->thread_message_count,
              },
            };
            ccord_message mpatch = {
              .id = starter->id,
              .channel_id = starter->channel_id,
              .author_id = starter->author_id,
              .timestamp_ms = starter->timestamp_ms,
              .content = starter->content,
              .rich = &rpatch,
            };
            ccord_error merr;
            (void)ccord_store_update_message(s->store, &mpatch, &merr);
          }
        }
      }
      break;
    case CCORD_NET_CHANNEL_DELETE:
      (void)ccord_store_forum_remove_post(s->store, ev->channel_id, &err);
      if (ccord_store_channel(s->store, ev->channel_id)) {
        if (ccord_store_delete_channel(s->store, ev->channel_id, &err) != CCORD_OK) {
          CCORD_LOG_W("session: channel delete rejected: %s", err.message);
        } else {
          sess_voice_track_remove_channel(s, ev->channel_id);
        }
      }
      break;
    case CCORD_NET_REACTION_UPDATE: {
      // Ignore reactions for messages that have not reached the cache yet.
      const ccord_message *rmsg = ccord_store_find_message(s->store, ev->channel_id, ev->message_id);
      if (!rmsg) break;
      ccord_store_set_reaction_actor(s->store, ev->user_id);
      if (ev->message_count > 0 && ev->messages) {
        const ccord_message *authoritative = &ev->messages[0];
        if (authoritative->reaction_kind_count == 0) {
          if (ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id,
                                        authoritative->reaction_count, authoritative->self_reacted,
                                        &err) != CCORD_OK) {
            CCORD_LOG_W("session: reaction clear rejected: %s", err.message);
          }
        } else {
          // Diff against existing reactions in place to avoid clearing & reallocating unchanged emojis.
          size_t valid_auth = 0;
          for (size_t ri = 0; ri < authoritative->reaction_kind_count; ri++) {
            const ccord_reaction *ar = &authoritative->reactions[ri];
            if (ar->emoji && ar->count > 0) valid_auth++;
          }
          if (valid_auth == 0) {
            if (ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id, 0, false, &err) != CCORD_OK) {
              CCORD_LOG_W("session: reaction clear rejected: %s", err.message);
            }
          } else {
            char stale_emojis[CCORD_MAX_REACTIONS_PER_MESSAGE][CCORD_EMOJI_MAX_BYTES + 1];
            size_t nstale = 0;
            for (size_t ci = 0; ci < rmsg->reaction_kind_count; ci++) {
              const char *cur_emoji = rmsg->reactions[ci].emoji;
              if (!cur_emoji) continue;
              bool kept = false;
              for (size_t ri = 0; ri < authoritative->reaction_kind_count; ri++) {
                const ccord_reaction *ar = &authoritative->reactions[ri];
                if (ar->emoji && ar->count > 0 && strcmp(cur_emoji, ar->emoji) == 0) {
                  kept = true;
                  break;
                }
              }
              if (!kept && nstale < CCORD_MAX_REACTIONS_PER_MESSAGE) {
                size_t elen = strlen(cur_emoji);
                if (elen > CCORD_EMOJI_MAX_BYTES) elen = CCORD_EMOJI_MAX_BYTES;
                memcpy(stale_emojis[nstale], cur_emoji, elen);
                stale_emojis[nstale][elen] = '\0';
                nstale++;
              }
            }
            if (nstale > 0) {
              if (nstale == rmsg->reaction_kind_count) {
                (void)ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id, 0, false, &err);
              } else {
                for (size_t si = 0; si < nstale; si++) {
                  (void)ccord_store_set_reaction_ex(s->store, ev->channel_id, ev->message_id, stale_emojis[si],
                                                    0, false, 0, false, &err);
                }
              }
              rmsg = ccord_store_find_message(s->store, ev->channel_id, ev->message_id);
            }
            for (size_t ri = 0; rmsg && ri < authoritative->reaction_kind_count; ri++) {
              const ccord_reaction *ar = &authoritative->reactions[ri];
              if (!ar->emoji || ar->count == 0) continue;
              uint32_t want_burst = ar->burst_count > ar->count ? ar->count : ar->burst_count;
              bool want_me_burst = want_burst == 0 ? false : ar->me_burst;
              bool same = false;
              for (size_t ci = 0; ci < rmsg->reaction_kind_count; ci++) {
                const ccord_reaction *cr = &rmsg->reactions[ci];
                if (cr->emoji && strcmp(cr->emoji, ar->emoji) == 0) {
                  same = (cr->count == ar->count && cr->self_reacted == ar->self_reacted &&
                          cr->burst_count == want_burst && cr->me_burst == want_me_burst);
                  break;
                }
              }
              if (same) continue;
              if (ccord_store_set_reaction_ex(s->store, ev->channel_id, ev->message_id, ar->emoji,
                                              ar->count, ar->self_reacted, ar->burst_count, ar->me_burst,
                                              &err) != CCORD_OK) {
                CCORD_LOG_W("session: reaction sync rejected: %s", err.message);
              }
              rmsg = ccord_store_find_message(s->store, ev->channel_id, ev->message_id);
            }
          }
        }
      } else if (ev->reaction_delta != 0 && ev->reaction_emoji) {
        if (ccord_store_adjust_reaction_ex(s->store, ev->channel_id, ev->message_id, ev->reaction_emoji,
                                           ev->reaction_delta, ev->user_id == ccord_store_self_id(s->store),
                                           ev->reaction_burst, &err) != CCORD_OK) {
          CCORD_LOG_W("session: reaction delta rejected: %s", err.message);
        }
      } else if (ev->reaction_emoji) {
        bool self_flag = ev->self_reacted;
        if (ev->user_id != CCORD_ID_NONE && ev->user_id != ccord_store_self_id(s->store)) {
          self_flag = false;
          for (size_t ri = 0; ri < rmsg->reaction_kind_count; ri++) {
            if (rmsg->reactions[ri].emoji && strcmp(rmsg->reactions[ri].emoji, ev->reaction_emoji) == 0) {
              self_flag = rmsg->reactions[ri].self_reacted;
              break;
            }
          }
        }
        if (ccord_store_set_reaction(s->store, ev->channel_id, ev->message_id, ev->reaction_emoji,
                                     ev->reaction_count, self_flag, &err) != CCORD_OK) {
          CCORD_LOG_W("session: reaction rejected: %s", err.message);
        }
      } else if (ccord_store_set_reactions(s->store, ev->channel_id, ev->message_id, ev->reaction_count,
                                           ev->self_reacted, &err) != CCORD_OK) {
        CCORD_LOG_W("session: reaction rejected: %s", err.message);
      }
      ccord_store_set_reaction_actor(s->store, CCORD_ID_NONE);
      break;
    }
    case CCORD_NET_VOICE_STATE_UPDATE:
      if (!ev->voice_state) {
        CCORD_LOG_W("session: voice state rejected: null voice state");
        break;
      }
      session_apply_voice_state(s, ev->voice_state, (ev->users && ev->user_count > 0) ? &ev->users[0] : nullptr);
      break;
    case CCORD_NET_VOICE_STATES:
      session_apply_voice_states(s, ev->replace_voice_states ? ev->guild_id : CCORD_ID_NONE, ev->voice_states,
                                 ev->voice_state_count, ev->users, ev->user_count);
      break;
    case CCORD_NET_VOICE_SERVER_UPDATE:
      if (!ev->voice_server || ccord_store_set_voice_server(s->store, ev->voice_server, &err) != CCORD_OK) {
        CCORD_LOG_W("session: voice server rejected: %s", ev->voice_server ? err.message : "null voice server");
      }
      break;
    case CCORD_NET_STREAM_UPDATE:
      if (!ev->stream || ccord_store_set_stream(s->store, ev->stream, &err) != CCORD_OK) {
        CCORD_LOG_W("session: stream rejected: %s", ev->stream ? err.message : "null stream");
      }
      break;
    case CCORD_NET_MESSAGE_PIN_UPDATE:
      if (ccord_store_set_message_pinned(s->store, ev->channel_id, ev->message_id, ev->pinned, &err) != CCORD_OK) {
        CCORD_LOG_W("session: pin rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_UPSERT:
      if (!ev->guild) {
        CCORD_LOG_W("session: guild upsert without guild");
      } else if (ccord_store_upsert_guild(s->store, ev->guild, ev->guild_channels, ev->guild_channel_count,
                                          ev->guild_members, ev->guild_member_count, ev->users, ev->user_count,
                                          &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild upsert rejected: %s", err.message);
      } else {
        if (ev->forum_meta_count > 0 && ev->forum_metas) {
          (void)ccord_store_forum_set_metas(s->store, ev->forum_metas, ev->forum_meta_count, &err);
        }
        if (ev->forum_post_count > 0 && ev->forum_posts) {
          (void)ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err);
        }
        apply_expression_lists(s, ev, ev->guild->id);
        apply_permission_data(s, ev);
        if (ev->replace_voice_states || (ev->voice_states && ev->voice_state_count > 0)) {
          session_apply_voice_states(s, ev->replace_voice_states ? ev->guild->id : CCORD_ID_NONE,
                                     ev->voice_states, ev->voice_state_count, nullptr, 0);
        }
        if (ccord_store_selected_guild(s->store) == ev->guild->id) {
          ccord_id self_id = ccord_store_self_id(s->store);
          ccord_id cur_ch = ccord_store_selected_channel(s->store);
          if (cur_ch != CCORD_ID_NONE && ccord_store_permissions_exact(s->store, ev->guild->id, self_id) &&
              (ccord_store_permissions(s->store, ev->guild->id, cur_ch, self_id, session_now_unix_ms(s)) & CCORD_PERM_VIEW_CHANNEL) == 0) {
            size_t nc = ccord_store_channel_count(s->store, ev->guild->id);
            for (size_t ci = 0; ci < nc; ci++) {
              const ccord_channel *cand = ccord_store_channel_at(s->store, ev->guild->id, ci);
              if (cand && cand->kind == CCORD_CHANNEL_TEXT &&
                  (ccord_store_permissions(s->store, ev->guild->id, cand->id, self_id, session_now_unix_ms(s)) & CCORD_PERM_VIEW_CHANNEL) != 0) {
                (void)ccord_session_select_channel(s, cand->id, &err);
                break;
              }
            }
          }
        }
        if (ev->scheduled_event_count > 0 && ev->scheduled_events) {
          (void)ccord_store_set_scheduled_events(s->store, ev->guild->id, ev->scheduled_events,
                                                 ev->scheduled_event_count, &err);
        }
        if (ev->admin_op == CCORD_ADMIN_CREATE_GUILD ||
            (s->pending_join_guild_id != CCORD_ID_NONE && s->pending_join_guild_id == ev->guild->id)) {
          ccord_id join_ch = (s->pending_join_guild_id == ev->guild->id) ? s->pending_join_channel_id : CCORD_ID_NONE;
          s->pending_join_guild_id = CCORD_ID_NONE;
          s->pending_join_channel_id = CCORD_ID_NONE;
          const ccord_channel *jch = join_ch != CCORD_ID_NONE ? ccord_store_channel(s->store, join_ch) : nullptr;
          if (jch && jch->guild_id == ev->guild->id && jch->kind == CCORD_CHANNEL_TEXT) {
            (void)ccord_session_select_channel(s, join_ch, &err);
          } else {
            (void)ccord_session_select_guild(s, ev->guild->id, &err);
          }
        }
        if (s->in_background) {
          s->bg_deferred_sync = true;
        } else {
          sess_request_history_once(s, ccord_store_selected_channel(s->store));
        }
      }
      break;
    case CCORD_NET_GUILD_DELETE:
      if (ccord_store_delete_guild(s->store, ev->guild_id, &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild delete rejected: %s", err.message);
      } else {
        sess_voice_track_remove_guild(s, ev->guild_id);
      }
      break;
    case CCORD_NET_MEMBER_REMOVE:
      // NOT_FOUND is normal: the member list is lazy, so we may never have loaded this member.
      if (ccord_store_remove_member(s->store, ev->guild_id, ev->user_id, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: member remove rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_MEMBERS:
      if (ccord_store_upsert_guild_members(s->store, ev->guild_id, ev->guild_members, ev->guild_member_count,
                                           ev->users, ev->user_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild members rejected: %s", err.message);
      } else {
        apply_permission_data(s, ev);
      }
      // A member list update says how big the list is and how big its groups are; that is what the interface prints on the
      // group headers and what tells it whether more of the list is still to come.
      if (ev->member_total > 0 || ev->member_group_count > 0) {
        if (ccord_store_set_member_list_size(s->store, ev->guild_id, ev->member_total, ev->member_online, ev->member_groups,
                                             ev->member_group_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: member list size rejected: %s", err.message);
        }
      }
      // The piece that was asked for has arrived when a SYNC reaches the end of what was asked: not just any update of the
      // list (a presence change inside a range already held is one too). If it added nobody, the list has no more.
      if (s->paging_wait && s->paging_guild == ev->guild_id && ev->member_synced_to >= s->paging_chunks * CCORD_MEMBER_CHUNK) {
        s->paging_wait = false;
        if (ccord_store_member_count(s->store, ev->guild_id) <= s->paging_held) s->paging_done = true;
      }
      break;
    case CCORD_NET_ROLE_UPSERT:
      // One role (create / edit) or several (a reorder, or a new role that pushed the others up).
      for (size_t i = 0; i < ev->role_count; i++) {
        if (ccord_store_upsert_role(s->store, &ev->roles[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: role upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_MEMBER_ROLE_CHANGE:
      // NOT_FOUND is normal: this member's roles may not be known (lazy member list); the gateway update fills them in.
      if (ccord_store_apply_member_role(s->store, ev->guild_id, ev->user_id, ev->role_id, ev->role_added, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: member role change rejected: %s", err.message);
      }
      break;
    case CCORD_NET_BAN_LIST:
      if (ccord_store_set_bans(s->store, ev->guild_id, ev->bans, ev->ban_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: ban list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_BAN_REMOVE:
      (void)ccord_store_remove_ban(s->store, ev->guild_id, ev->user_id);
      break;
    case CCORD_NET_INVITE_LIST:
      if (ccord_store_set_invites(s->store, ev->guild_id, ev->invites, ev->invite_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: invite list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_INVITE_UPSERT:
      for (size_t i = 0; i < ev->invite_count; i++) {
        if (ccord_store_upsert_invite(s->store, ev->guild_id, &ev->invites[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: invite rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_INVITE_DELETE:
      (void)ccord_store_remove_invite(s->store, ev->guild_id, ev->invite_code);
      break;
    case CCORD_NET_NOTIFY_SETTINGS: {
      ccord_error nerr;
      if (ccord_store_set_notify(s->store, ev->guild_id, ev->notify, ev->notify_count, &nerr) != CCORD_OK) {
        CCORD_LOG_W("session: notification settings rejected: %s", nerr.message);
      }
      break;
    }
    case CCORD_NET_AUDIT_LOG:
      if (ccord_store_set_audit(s->store, ev->guild_id, ev->audit, ev->audit_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: audit log rejected: %s", err.message);
      }
      break;
    case CCORD_NET_ROLE_DELETE:
      // NOT_FOUND is normal: a role we never saw (the table arrives with the guild) cannot be removed.
      if (ccord_store_remove_role(s->store, ev->guild_id, ev->role_id, &err) != CCORD_OK && err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: role delete rejected: %s", err.message);
      }
      break;
    case CCORD_NET_PIN_LIST:
      sess_pins_clear_in_flight(s, ev->channel_id);
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      seed_channel_message_members(s, ev->channel_id, ev->guild_id, ev->messages, ev->message_count,
                                   ev->guild_members, ev->guild_member_count);
      if (ccord_store_set_pins_page(s->store, ev->channel_id, ev->messages, ev->message_count,
                                    ev->pins_append, ev->pins_has_more, ev->pins_next_cursor, &err) != CCORD_OK) {
        CCORD_LOG_W("session: pins rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_PINS_UPDATE:
      if (!s->in_background && session_is_authenticated(s) && ev->channel_id != CCORD_ID_NONE &&
          ev->channel_id == ccord_store_selected_channel(s->store) &&
          ccord_store_history_loaded(s->store, ev->channel_id)) {
        (void)ccord_transport_request_pins(s->transport, ev->channel_id, 50u, &err);
      }
      break;
    case CCORD_NET_MESSAGE_ACK: {
      // Another session read up to a message: what is newer than it stays unread.
      const ccord_channel *ac = ccord_store_channel(s->store, ev->channel_id);
      if (ac) {
        ccord_id upto = ev->message_id != CCORD_ID_NONE ? ev->message_id : ac->last_message_id;
        if (ac->unread_count == 0 && ac->mention_count == 0 && ev->mention_count == 0 &&
            ac->last_message_id > upto) {
          upto = ac->last_message_id;
        }
        if (ccord_store_apply_read(s->store, ev->channel_id, upto, ev->mention_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: message ack rejected: %s", err.message);
        }
      }
      break;
    }
    case CCORD_NET_RELATIONSHIP_UPDATE:
      if (ccord_store_set_relationship(s->store, ev->user_id, ev->relationship_kind, &err) != CCORD_OK) {
        CCORD_LOG_W("session: relationship update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MEDIA_LOADED:
      sess_store_media_loaded(s, ev);
      break;
    case CCORD_NET_MEDIA_RANGE:
      ccord_stream_hub_on_range(s->streams, ev, ccord_clock_ms());
      break;
    case CCORD_NET_VIDEO_RESOLVED: {
      enum { SESSION_VIDEO_RESOLVED_MAX = 8u };
      if (!s->video_resolved) {
        s->video_resolved = ccord_calloc(SESSION_VIDEO_RESOLVED_MAX, sizeof *s->video_resolved);
      }
      if (s->video_resolved) {
        if (s->video_resolved_count == SESSION_VIDEO_RESOLVED_MAX) {
          s->video_resolved_head = (s->video_resolved_head + 1u) % SESSION_VIDEO_RESOLVED_MAX;
          s->video_resolved_count--;
        }
        ccord_video_resolved *dst =
            &s->video_resolved[(s->video_resolved_head + s->video_resolved_count) % SESSION_VIDEO_RESOLVED_MAX];
        memset(dst, 0, sizeof *dst);
        dst->req_id = ev->resolve_id;
        dst->status = ev->status;
        dst->missing_helper = ev->resolved_missing_helper;
        dst->duration_ms = ev->resolved_duration_ms;
        if (ev->media_url) snprintf(dst->page_url, sizeof dst->page_url, "%s", ev->media_url);
        if (ev->resolved_video_url) snprintf(dst->video_url, sizeof dst->video_url, "%s", ev->resolved_video_url);
        if (ev->resolved_audio_url) snprintf(dst->audio_url, sizeof dst->audio_url, "%s", ev->resolved_audio_url);
        if (ev->text) snprintf(dst->error, sizeof dst->error, "%s", ev->text);
        s->video_resolved_count++;
      }
      break;
    }
    case CCORD_NET_RAW_GATEWAY:
      if (s->raw_gw_cb && ev->expr_arg && ev->expr_body) {
        s->raw_gw_cb(s->raw_gw_ud, ev->expr_arg, ev->expr_body);
      }
      break;
    case CCORD_NET_SEARCH_RESULTS:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      sess_store_search_results(s, ev);
      break;
    case CCORD_NET_USER_PROFILE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->user_profile) {
        if (ccord_store_upsert_user_profile(s->store, ev->user_profile, &err) != CCORD_OK) {
          CCORD_LOG_W("session: user profile rejected: %s", err.message);
        } else {
          sess_mark_profile_loaded(s, ev->user_profile->user_id);
        }
      }
      break;
    case CCORD_NET_GUILD_EXPRESSIONS: {
      ccord_net_event e = *ev;
      if (!e.replace_emojis) e.emoji_count = 0;
      if (!e.replace_stickers) e.sticker_count = 0;
      if (!e.replace_soundboard) e.soundboard_count = 0;
      apply_expression_lists(s, &e, ev->guild_id);
      break;
    }
    case CCORD_NET_EXPRESSION_DATA: {
      bool expr_ok = ev->status == CCORD_OK &&
                     (ev->expr_body != nullptr ||
                      (ev->expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev->raw_proto && ev->raw_proto_len > 0));
      const char *blob_body = ev->expr_body ? ev->expr_body : (expr_ok ? "{}" : nullptr);
      size_t blob_len = ev->expr_body ? ev->expr_body_len : (expr_ok ? 2u : 0u);
      if (ccord_store_set_expr_blob(s->store, ev->expr_kind, ev->expr_arg, blob_body, blob_len, expr_ok, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: expression data rejected: %s", err.message);
      } else if (ev->expr_kind == CCORD_EXPR_SETTINGS_PROTO && expr_ok) {
        sess_cache_settings_proto(s, ev);
        ccord_user_expressions ux;
        ccord_status ux_st =
          (ev->raw_proto && ev->raw_proto_len > 0)
            ? ccord_user_expressions_decode(ev->raw_proto, ev->raw_proto_len, &ux, &err)
            : ccord_user_expressions_from_settings_json(ev->expr_body, ev->expr_body_len, &ux, &err);
        if (ux_st == CCORD_OK) {
          sess_apply_favorite_gifs_wire(s, &ux, true);
          for (size_t i = 0; i < ux.favorite_emoji_count; i++) {
            if (ux.favorite_emojis[i] && !ccord_store_is_favorite_emoji(s->store, ux.favorite_emojis[i])) {
              (void)ccord_store_toggle_favorite_emoji(s->store, ux.favorite_emojis[i]);
            }
          }
          for (size_t i = 0; i < ux.emoji_frecency_count; i++) {
            ccord_store_seed_emoji_frecency(s->store, ux.emoji_frecency[i].key, ux.emoji_frecency[i].score);
          }
          for (size_t i = 0; i < ux.favorite_sticker_count; i++) {
            if (!ccord_store_is_favorite_sticker(s->store, ux.favorite_stickers[i])) {
              (void)ccord_store_toggle_favorite_sticker(s->store, ux.favorite_stickers[i]);
            }
          }
          for (size_t i = 0; i < ux.sticker_frecency_count; i++) {
            ccord_store_seed_sticker_frecency(s->store, ux.sticker_frecency[i].id, ux.sticker_frecency[i].score);
          }
          ccord_user_expressions_free(&ux);
        }
      }
      break;
    }
    case CCORD_NET_STREAM_SIGNAL:
      if (ev->stream_signal) {
        const ccord_stream_signal *sig = ev->stream_signal;
        ccord_id self_id = ccord_store_self_id(s->store);
        // Go Live broadcast cache: the server's signals are the only source (never a local click).
        // SERVER_UPDATE carries media credentials for the transport, not a list row.
        if (sig->channel_id != CCORD_ID_NONE && sig->user_id != CCORD_ID_NONE && !sig->server_update) {
          if (sig->kind == CCORD_STREAM_SIG_CREATE && !sig->unavailable) {
            ccord_stream_state bst = {
              .guild_id = sig->guild_id,
              .channel_id = sig->channel_id,
              .user_id = sig->user_id,
              .active = true,
              .paused = sig->paused,
              .config = (sig->user_id == self_id) ? s->pending_stream_cfg : (ccord_stream_config){0},
            };
            if (ccord_store_stream_upsert(s->store, &bst, sig->viewer_count, &err) != CCORD_OK) {
              CCORD_LOG_W("session: stream signal rejected: %s", err.message);
            }
          } else if (sig->kind == CCORD_STREAM_SIG_UPDATE) {
            // Viewer-count / pause change for a broadcast we must already know (NOT_FOUND before CREATE).
            ccord_status vst = ccord_store_stream_set_viewers(s->store, sig->channel_id, sig->user_id,
                                                              sig->viewer_count, &err);
            if (vst != CCORD_OK && vst != CCORD_ERR_NOT_FOUND) {
              CCORD_LOG_W("session: stream viewers rejected: %s", err.message);
            }
            const ccord_stream_state *existing = ccord_store_stream_find(s->store, sig->channel_id, sig->user_id);
            if (existing && existing->paused != sig->paused) {
              ccord_stream_state upd = *existing;
              upd.paused = sig->paused;
              (void)ccord_store_stream_upsert(s->store, &upd, sig->viewer_count, &err);
            }
          } else if (sig->kind == CCORD_STREAM_SIG_DELETE || sig->unavailable) {
            bool keep_peer_row = false;
            if (!sig->unavailable && sig->channel_id == s->unsub_channel_id && sig->user_id == s->unsub_user_id) {
              s->unsub_channel_id = CCORD_ID_NONE;
              s->unsub_user_id = CCORD_ID_NONE;
              const char *r = sig->reason ? sig->reason : "";
              bool explicit_end = strncmp(r, "broadcaster", 11) == 0 || strcmp(r, "stream_ended") == 0 ||
                                  strcmp(r, "stream_full") == 0 || strcmp(r, "unauthorized") == 0;
              if (strcmp(r, "viewer stopped watching") == 0) {
                keep_peer_row = true;
              } else if (!explicit_end) {
                ccord_id vguild = (sig->is_call || sig->guild_id == CCORD_ID_NONE) ? CCORD_DM_GUILD_ID : sig->guild_id;
                const ccord_voice_state *bvs = ccord_store_voice_user(s->store, vguild, sig->user_id);
                if (bvs && bvs->channel_id == sig->channel_id && bvs->self_stream) keep_peer_row = true;
              }
            }
            if (!keep_peer_row) {
              (void)ccord_store_stream_remove(s->store, sig->channel_id, sig->user_id, &err);
            }
          }
        }
        if (sig->user_id != CCORD_ID_NONE && sig->user_id == self_id && !sig->server_update) {
          const ccord_stream_state *prev = ccord_store_stream(s->store);
          bool paused = sig->kind == CCORD_STREAM_SIG_UPDATE ? sig->paused
                                                             : (sig->paused || (prev && prev->active && prev->paused));
          if (sig->deleted) paused = false;
          ccord_stream_state st = {
            .guild_id = sig->guild_id,
            .channel_id = sig->channel_id,
            .user_id = sig->user_id,
            .active = !sig->deleted,
            .paused = paused,
            .config = s->pending_stream_cfg,
          };
          (void)ccord_store_set_stream(s->store, &st, &err);
        }
        if (s->watched.active && sig->user_id == s->watched.user_id &&
            (sig->channel_id == CCORD_ID_NONE || sig->channel_id == s->watched.channel_id)) {
          if (sig->deleted || sig->unavailable) {
            s->watched.active = false;
            atomic_store_explicit(&s->watched_active, false, memory_order_release);
            s->watched.paused = false;
            ccord_free(s->stream_frame_buf);
            s->stream_frame_buf = nullptr;
          } else if (!sig->server_update) {
            s->watched.paused = sig->paused;
          }
        }
      }
      break;
    case CCORD_NET_FORUM_POSTS:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts) {
        if (ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: forum posts rejected: %s", err.message);
        }
      }
      if (ev->channel_id != CCORD_ID_NONE && ccord_store_channel_is_forum(s->store, ev->channel_id)) {
        const ccord_forum_state *prev = ccord_store_forum_state(s->store, ev->channel_id);
        ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
        fst.loaded = true;
        fst.loading = false;
        fst.has_more = ev->forum_has_more;
        fst.next_offset = ev->forum_offset + (uint32_t)ev->forum_post_count;
        if (ev->forum_total > 0 || ev->forum_offset == 0) fst.total = ev->forum_total;
        fst.error[0] = '\0';
        (void)ccord_store_forum_set_state(s->store, ev->channel_id, &fst, &err);
      }
      break;
    case CCORD_NET_FORUM_POST_CREATED:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts) {
        if (ccord_store_forum_upsert_posts(s->store, ev->forum_posts, ev->forum_post_count, &err) != CCORD_OK) {
          CCORD_LOG_W("session: created forum post rejected: %s", err.message);
        }
      }
      if (ev->channel_id != CCORD_ID_NONE && ccord_store_channel_is_forum(s->store, ev->channel_id)) {
        const ccord_forum_state *prev = ccord_store_forum_state(s->store, ev->channel_id);
        ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
        fst.creating = false;
        fst.error[0] = '\0';
        if (fst.total > 0) fst.total++;
        (void)ccord_store_forum_set_state(s->store, ev->channel_id, &fst, &err);
      }
      if (ev->forum_post_count > 0 && ev->forum_posts && ev->forum_posts[0].thread_id != CCORD_ID_NONE) {
        ccord_id tid = ev->forum_posts[0].thread_id;
        if (ccord_store_forum_open_post(s->store, tid, &err) == CCORD_OK) {
          if (ev->message_count > 0 && ev->messages) {
            seed_channel_message_members(s, tid, ev->guild_id, ev->messages, ev->message_count,
                                         ev->guild_members, ev->guild_member_count);
            (void)ccord_store_set_history(s->store, tid, ev->messages, ev->message_count, &err);
            session_channel_hist *chist = sess_chan_hist_for(s, tid);
            if (chist) {
              chist->in_flight_before_id = CCORD_ID_NONE;
              chist->has_more = false;
            }
          } else {
            sess_request_history_once(s, tid);
          }
        }
      }
      break;
    case CCORD_NET_FORUM_FAILED:
      if (ev->channel_id != CCORD_ID_NONE && ccord_store_channel_is_forum(s->store, ev->channel_id)) {
        const ccord_forum_state *prev = ccord_store_forum_state(s->store, ev->channel_id);
        ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
        if (ev->forum_creating) fst.creating = false;
        else fst.loading = false;
        const char *msg = (ev->text && ev->text[0]) ? ev->text : ccord_status_name(ev->status);
        size_t mlen = strlen(msg);
        if (mlen >= sizeof fst.error) mlen = sizeof fst.error - 1;
        memcpy(fst.error, msg, mlen);
        fst.error[mlen] = '\0';
        (void)ccord_store_forum_set_state(s->store, ev->channel_id, &fst, &err);
      }
      break;
    default:
      sess_apply_event_ext(s, ev);
      break;
  }
}

