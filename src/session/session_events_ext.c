// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void sess_apply_event_ext(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error err;
  switch (ev->type) {
    case CCORD_NET_ERROR:
      CCORD_LOG_W("session: transport error %s: %s", ccord_status_name(ev->status), ev->text ? ev->text : "");
      if (ev->user_id != CCORD_ID_NONE) {
        sess_clear_profile_requested(s, ev->user_id);
      }
      if (ev->channel_id == CCORD_ID_NONE && ev->user_id == CCORD_ID_NONE) {
        s->discovery_loading = false;
      }
      if (ev->channel_id != CCORD_ID_NONE) {
        sess_clear_requested(s, ev->channel_id);
        sess_pins_clear_in_flight(s, ev->channel_id);
        if (s->react_users_loading && s->react_users_channel_id == ev->channel_id) {
          s->react_users_loading = false;
        }
        session_channel_hist *chist = sess_find_chan_hist(s, ev->channel_id);
        if (chist) {
          chist->in_flight_before_id = CCORD_ID_NONE;
        }
        if (ev->channel_id == ccord_store_selected_channel(s->store) &&
            !ccord_store_history_loaded(s->store, ev->channel_id) &&
            s->retried_channel != ev->channel_id) {
          s->retried_channel = ev->channel_id;
          sess_request_history_once(s, ev->channel_id);
        }
      }
      break;
    case CCORD_NET_SPEAKING:
      ccord_store_set_speaking(s->store, ev->user_id, ev->speaking);
      break;
    case CCORD_NET_VOICE_LINK: {
      // The call stays joined while the transport reconnects: only the connection state changes. A stale event
      // after the user hung up finds no active channel and is ignored.
      if (ccord_store_active_voice_channel(s->store) == CCORD_ID_NONE) break;
      ccord_rtc_stats rtc;
      ccord_store_rtc_stats(s->store, &rtc);
      rtc.state = ev->voice_link == CCORD_VLINK_CONNECTED ? CCORD_VOICE_ESTABLISHED
                : ev->voice_link == CCORD_VLINK_RECONNECTING ? CCORD_VOICE_RECONNECTING : CCORD_VOICE_FAILED;
      (void)ccord_store_set_rtc_stats(s->store, &rtc, &err);
      ccord_error_clear(&err);
      break;
    }
    case CCORD_NET_ACTION_FAILED:
      if (ev->action == CCORD_ACTION_ADMIN &&
          (ev->admin_op == CCORD_ADMIN_PREVIEW_INVITE || ev->admin_op == CCORD_ADMIN_ACCEPT_INVITE)) {
        s->invite_preview_loading = false;
        // A failed preview must not keep the in-flight guard closed: the next ask retries immediately.
        if (s->invite_cache) {
          for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) {
            if (s->invite_cache[i].sent_ms != 0 && ev->admin_op == CCORD_ADMIN_PREVIEW_INVITE) {
              s->invite_cache[i].failed = true;
            }
            s->invite_cache[i].sent_ms = 0;
          }
        }
      }
      if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_GET_GUILD_PROFILE) {
        for (size_t i = 0; i < s->nguild_profiles; i++) {
          if (ev->guild_id == CCORD_ID_NONE || s->guild_profiles[i].guild_id == ev->guild_id) {
            s->guild_profiles[i].loading = false;
            s->guild_profiles[i].sent_ms = 0;
          }
        }
      }
      if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_AUTH_SESSIONS_LIST) {
        s->auth_sessions_state = CCORD_FETCH_FAILED;   // the old list stays; the guard reopens for a retry
        s->auth_sessions_sent_ms = 0;
      } else if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_CONNECTIONS_LIST) {
        s->connections_state = CCORD_FETCH_FAILED;
        s->connections_sent_ms = 0;
      } else if (ev->action == CCORD_ACTION_ADMIN && ev->admin_op == CCORD_ADMIN_ACTIVITY_SHELF) {
        for (size_t i = 0; i < s->nactivity_shelves; i++) {
          if (s->activity_shelves[i].guild_id == ev->guild_id) {
            s->activity_shelves[i].loading = false;   // the cached shelf stays; the next ask retries
            s->activity_shelves[i].sent_ms = 0;
          }
        }
      }
      // M0-3: a refused send with a nonce fails exactly its pending echo; the failed bubble keeps the draft, so
      // sess_record_failure must not also queue it (the composer would restore a second copy). Without a matching echo
      // (legacy send, or history replaced meanwhile) the failure keeps the legacy draft-restore ring.
      if (ev->action == CCORD_ACTION_SEND && ev->action_nonce[0] != '\0') {
        session_local_echo le;
        if (sess_find_local_echo_by_nonce(s, ev->channel_id, ev->action_nonce, &le) &&
            sess_set_local_echo_state(s, ev->channel_id, &le, CCORD_MSG_SEND_FAILED)) {
          break;
        }
      }
      if (ev->action == CCORD_ACTION_VOICE) {
        s->pending_voice_guild = CCORD_ID_NONE;
        s->pending_voice_channel = CCORD_ID_NONE;
      }
      sess_record_failure(s, ev);
      break;
    case CCORD_NET_POLL_VOTE: {
      bool is_self = ev->user_id != CCORD_ID_NONE && ev->user_id == ccord_store_self_id(s->store);
      if (ccord_store_apply_poll_vote(s->store, ev->channel_id, ev->message_id, ev->poll_answer_id,
                                      is_self, ev->poll_vote_added, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: poll vote rejected: %s", err.message);
      }
      break;
    }
    case CCORD_NET_INTERACTION_MODAL:
      if (ev->modal) {
        if (!s->modals) {
          s->modals = ccord_calloc(SESSION_MAX_MODALS, sizeof *s->modals);
          if (!s->modals) {
            CCORD_LOG_W("session: out of memory queuing interaction modal");
            break;
          }
        }
        if (s->modal_count == SESSION_MAX_MODALS) {
          s->modal_head = (s->modal_head + 1u) % SESSION_MAX_MODALS;
          s->modal_count--;
        }
        size_t idx = (s->modal_head + s->modal_count) % SESSION_MAX_MODALS;
        s->modals[idx] = *ev->modal;
        s->modal_count++;
      }
      break;
    case CCORD_NET_SLASH_COMMANDS: {
      ccord_id gid = ev->guild_id;
      if (gid == CCORD_ID_NONE && ev->channel_id != CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(s->store, ev->channel_id);
        gid = ch ? ch->guild_id : CCORD_DM_GUILD_ID;
      }
      if (gid == CCORD_ID_NONE) gid = CCORD_DM_GUILD_ID;
      if (ccord_store_set_slash_commands(s->store, gid, ev->slash_commands, ev->slash_command_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: slash commands rejected: %s", err.message);
      } else {
        session_slash_cache *ent = sess_slash_cache_for(s, gid);
        if (ent) {
          ent->in_flight = false;
          ent->loaded = true;
          ent->fetched_ms = ent->sent_ms ? ent->sent_ms : 1u;
        }
      }
      break;
    }
    case CCORD_NET_SLASH_AUTOCOMPLETE: {
      size_t n = ev->slash_choice_count;
      if (n > CCORD_SLASH_MAX_CHOICES) n = CCORD_SLASH_MAX_CHOICES;
      if (n > 0 && ev->slash_choices) {
        memcpy(s->slash_choices, ev->slash_choices, n * sizeof s->slash_choices[0]);
        s->slash_choice_count = n;
      } else {
        s->slash_choice_count = 0;
      }
      s->slash_choice_seq++;
      break;
    }
    case CCORD_NET_REACTION_USERS:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      sess_store_reaction_users(s, ev);
      break;
    case CCORD_NET_THREAD_MEMBER_SELF:
      if (ev->channel_id != CCORD_ID_NONE &&
          (ev->user_id == CCORD_ID_NONE || ev->user_id == ccord_store_self_id(s->store))) {
        if (ccord_store_set_thread_joined(s->store, ev->channel_id, ev->thread_joined, &err) != CCORD_OK &&
            err.code != CCORD_ERR_NOT_FOUND) {
          CCORD_LOG_W("session: thread member update rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_GUILD_LAYOUT:
      if (ccord_store_set_guild_layout(s->store, ev->folder_items, ev->folder_item_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: guild layout rejected: %s", err.message);
      }
      session_apply_status_proto(s, ev);   // the same update may carry the status too
      break;
    case CCORD_NET_USER_SETTINGS_PROTO:
      if (ev->raw_proto && ev->raw_proto_len > 0) session_apply_frecency_update(s, ev);   // type 2
      else session_apply_status_proto(s, ev);
      break;
    case CCORD_NET_SESSIONS_REPLACE: {
      // Discord never sends a user its own PRESENCE_UPDATE: the status it is shown with comes from the session list.
      // Display only: op 3 is not re-sent (that would turn another client's status into this session's).
      ccord_id self = ccord_store_self_id(s->store);
      const ccord_user *me = self != CCORD_ID_NONE ? ccord_store_user(s->store, self) : nullptr;
      if (me && me->presence != ev->presence) {
        const char *st_keep = (ev->has_presence_detail && !ev->has_activity)
                                ? ""
                                : (me->status_text ? me->status_text : "");
        char *keep = ccord_strdup(st_keep);
        if (ccord_store_update_presence(s->store, self, ev->presence, keep, &err) != CCORD_OK) {
          CCORD_LOG_W("session: own status from sessions not applied: %s", err.message);
        }
        ccord_free(keep);
      }
      // The sessions list also says what this account is playing: an activity another session or the RPC client
      // stopped is gone here too.
      if (ev->has_presence_detail) {
        if (!ev->has_activity) s->self_activity_from_rich = false;
        session_set_self_activity(s, ev->has_activity, ev->activity_type, ev->activity_name, ev->activity_details,
                                  ev->activity_state, ev->activity_start_ms, ev->activity_end_ms);
      }
      break;
    }
    case CCORD_NET_CHANNEL_POSITIONS:
      if (ccord_store_reorder_channels(s->store, ev->guild_id, ev->reorder_ids, ev->reorder_positions,
                                       ev->reorder_parents, ev->reorder_has_parent, ev->reorder_count, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: channel reorder rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MEMBER_VERIFICATION:
      if (ev->member_verification && ev->guild_id != CCORD_ID_NONE) {
        session_verification_cache *ent = sess_verification_cache_for(s, ev->guild_id);
        if (!ent || !sess_cache_verification_form(ent, ev->member_verification)) {
          CCORD_LOG_W("session: verification form not cached: out of memory");
        }
      }
      break;
    case CCORD_NET_INVITE_PREVIEW:
      s->invite_preview_loading = false;
      if (ev->invite_preview) {
        s->invite_preview = *ev->invite_preview;
        s->has_invite_preview = true;
        session_invite_cache *entry = sess_invite_cache_for(s, ev->invite_preview->code);
        if (entry) {
          uint64_t fms = entry->sent_ms ? entry->sent_ms : ccord_clock_ms();
          entry->fetched_ms = fms ? fms : 1u;
          entry->sent_ms = 0;
          entry->failed = false;
          entry->preview = *ev->invite_preview;
        }
      } else if (s->invite_cache) {
        for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) s->invite_cache[i].sent_ms = 0;
      }
      break;
    case CCORD_NET_INVITE_ACCEPTED: {
      s->invite_preview_loading = false;
      s->has_invite_preview = false;
      ccord_id gid = ev->guild_id != CCORD_ID_NONE
                       ? ev->guild_id
                       : (ev->invite_preview ? ev->invite_preview->guild_id : CCORD_ID_NONE);
      ccord_id cid = ev->channel_id != CCORD_ID_NONE
                       ? ev->channel_id
                       : (ev->invite_preview ? ev->invite_preview->channel_id : CCORD_ID_NONE);
      if (gid != CCORD_ID_NONE) {
        if (ccord_store_guild(s->store, gid)) {
          const ccord_channel *jch = cid != CCORD_ID_NONE ? ccord_store_channel(s->store, cid) : nullptr;
          if (jch && jch->guild_id == gid && jch->kind == CCORD_CHANNEL_TEXT) {
            (void)ccord_session_select_channel(s, cid, &err);
          } else {
            (void)ccord_session_select_guild(s, gid, &err);
          }
        } else {
          s->pending_join_guild_id = gid;
          s->pending_join_channel_id = cid;
        }
      }
      break;
    }
    case CCORD_NET_DISCOVERY_RESULTS: {
      s->discovery_loading = false;
      size_t n = ev->discovery_count;
      if (n > CCORD_DISCOVERY_PAGE_MAX) n = CCORD_DISCOVERY_PAGE_MAX;
      if (n > 0 && ev->discovery_guilds) {
        if (!s->discovery_items) {
          s->discovery_items = ccord_calloc(CCORD_DISCOVERY_PAGE_MAX, sizeof *s->discovery_items);
        }
        if (s->discovery_items) {
          memcpy(s->discovery_items, ev->discovery_guilds, n * sizeof s->discovery_items[0]);
        } else {
          n = 0;
        }
      }
      s->discovery_count = n;
      s->discovery_total = ev->discovery_total >= n ? ev->discovery_total : (uint32_t)n;
      s->discovery_offset = ev->discovery_offset;
      s->discovery_category_id = ev->discovery_category_id;
      const char *dq = ev->discovery_query ? ev->discovery_query : "";
      size_t dqlen = strlen(dq);
      if (dqlen >= sizeof s->discovery_query) dqlen = sizeof s->discovery_query - 1u;
      memcpy(s->discovery_query, dq, dqlen);
      s->discovery_query[dqlen] = '\0';

      if (!s->discovery_cache) {
        s->discovery_cache = ccord_calloc(SESSION_DISCOVERY_CACHE_SLOTS, sizeof *s->discovery_cache);
      }
      if (s->discovery_cache) {
        size_t slot = SIZE_MAX;
        for (size_t i = 0; i < SESSION_DISCOVERY_CACHE_SLOTS; i++) {
          if (s->discovery_cache[i].valid &&
              s->discovery_cache[i].category_id == ev->discovery_category_id &&
              s->discovery_cache[i].offset == ev->discovery_offset &&
              strcmp(s->discovery_cache[i].query, s->discovery_query) == 0) {
            slot = i;
            break;
          }
        }
        if (slot == SIZE_MAX) {
          slot = s->discovery_cache_rr % SESSION_DISCOVERY_CACHE_SLOTS;
          s->discovery_cache_rr++;
        }
        session_discovery_cache_entry *ce = &s->discovery_cache[slot];
        ce->valid = true;
        uint64_t fms = s->discovery_sent_ms ? s->discovery_sent_ms : ccord_clock_ms();
        ce->fetched_ms = fms ? fms : 1u;
        memcpy(ce->query, s->discovery_query, dqlen + 1u);
        ce->category_id = ev->discovery_category_id;
        ce->offset = ev->discovery_offset;
        ce->total = s->discovery_total;
        ce->count = n;
        if (n > 0 && s->discovery_items) memcpy(ce->items, s->discovery_items, n * sizeof ce->items[0]);
      }
      break;
    }
    case CCORD_NET_DM_RECIPIENT_CHANGE:
      for (size_t u = 0; u < ev->user_count; u++) {
        (void)ccord_store_upsert_user(s->store, &ev->users[u], &err);
      }
      if (ccord_store_change_dm_recipient(s->store, ev->channel_id, ev->user_id, ev->recipient_added, &err) !=
            CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: dm recipient change rejected: %s", err.message);
      }
      break;
    case CCORD_NET_MESSAGE_REQUEST_ACCEPTED:
      if (ccord_store_accept_message_request(s->store, ev->channel_id, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: message request accept rejected: %s", err.message);
      }
      break;
    case CCORD_NET_USER_NOTE_UPDATE:
      if (ccord_store_set_user_note(s->store, ev->user_id, ev->text ? ev->text : "", &err) != CCORD_OK) {
        CCORD_LOG_W("session: user note update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CUSTOM_STATUS_UPDATE:
      if (ccord_store_set_self_custom_status(s->store, ev->custom_status_emoji, ev->custom_status_text,
                                             ev->custom_status_expires_ms, ev->custom_status_clear, &err) != CCORD_OK) {
        CCORD_LOG_W("session: custom status update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_INCIDENTS:
      if (ccord_store_set_guild_incidents(s->store, ev->guild_id, ev->invites_disabled_until_ms,
                                          ev->dms_disabled_until_ms, &err) != CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: guild incidents update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_VANITY:
      if (ccord_store_set_guild_vanity(s->store, ev->guild_id, ev->invite_code ? ev->invite_code : "", &err) !=
            CCORD_OK &&
          err.code != CCORD_ERR_NOT_FOUND) {
        CCORD_LOG_W("session: guild vanity update rejected: %s", err.message);
      }
      break;
    case CCORD_NET_EMOJI_UPSERT:
      for (size_t i = 0; i < ev->emoji_count; i++) {
        ccord_id egid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->emojis[i].guild_id;
        if (ccord_store_upsert_guild_emoji(s->store, egid, &ev->emojis[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: emoji upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_EMOJI_DELETE:
      (void)ccord_store_remove_guild_emoji(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_STICKER_UPSERT:
      for (size_t i = 0; i < ev->sticker_count; i++) {
        ccord_id sgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->stickers[i].guild_id;
        if (ccord_store_upsert_guild_sticker(s->store, sgid, &ev->stickers[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: sticker upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_STICKER_DELETE:
      (void)ccord_store_remove_guild_sticker(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_SOUNDBOARD_UPSERT:
      for (size_t i = 0; i < ev->soundboard_count; i++) {
        ccord_id sbgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->soundboard[i].guild_id;
        if (ccord_store_upsert_guild_soundboard(s->store, sbgid, &ev->soundboard[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: soundboard upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_SOUNDBOARD_DELETE:
      (void)ccord_store_remove_guild_soundboard(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_PRUNE_RESULT:
      if (ccord_store_set_prune_info(s->store, ev->guild_id, &ev->prune_info, &err) != CCORD_OK) {
        CCORD_LOG_W("session: prune info rejected: %s", err.message);
      }
      break;
    case CCORD_NET_AUTOMOD_LIST:
      if (ccord_store_set_automod_rules(s->store, ev->guild_id, ev->automod_rules, ev->automod_rule_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: automod list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_AUTOMOD_UPSERT:
      for (size_t i = 0; ev->automod_rules && i < ev->automod_rule_count; i++) {
        ccord_id amgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->automod_rules[i].guild_id;
        if (ccord_store_upsert_automod_rule(s->store, amgid, &ev->automod_rules[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: automod upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_AUTOMOD_DELETE:
      (void)ccord_store_remove_automod_rule(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_WEBHOOK_LIST:
      if (ccord_store_set_webhooks(s->store, ev->guild_id, ev->webhooks, ev->webhook_count, &err) != CCORD_OK) {
        CCORD_LOG_W("session: webhook list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_WEBHOOK_UPSERT:
      for (size_t i = 0; ev->webhooks && i < ev->webhook_count; i++) {
        ccord_id whgid = ev->guild_id != CCORD_ID_NONE ? ev->guild_id : ev->webhooks[i].guild_id;
        if (ccord_store_upsert_webhook(s->store, whgid, &ev->webhooks[i], &err) != CCORD_OK) {
          CCORD_LOG_W("session: webhook upsert rejected: %s", err.message);
        }
      }
      break;
    case CCORD_NET_WEBHOOK_DELETE:
      (void)ccord_store_remove_webhook(s->store, ev->guild_id, ev->expr_id);
      break;
    case CCORD_NET_WEBHOOKS_UPDATE:
      ccord_store_note_webhooks_update(s->store, ev->guild_id, ev->channel_id);
      break;
    case CCORD_NET_CHANNEL_FOLLOWER_ADDED: {
      ccord_followed_channel fc = {
        .source_channel_id = ev->channel_id,
        .target_channel_id = ev->target_channel_id,
        .webhook_id = ev->expr_id,
      };
      if (ccord_store_add_followed_channel(s->store, ev->guild_id, &fc, &err) != CCORD_OK) {
        CCORD_LOG_W("session: followed channel rejected: %s", err.message);
      }
      if (ccord_store_webhooks_loaded(s->store, ev->guild_id)) {
        ccord_admin_request wreq;
        if (ccord_admin_list_webhooks(&wreq, ev->guild_id, nullptr) == CCORD_OK) {
          (void)ccord_transport_admin_action(s->transport, &wreq, nullptr);
        }
      }
      break;
    }
    case CCORD_NET_APP_CMD_PERMS_LIST:
      if (ccord_store_set_app_cmd_perms(s->store, ev->guild_id, ev->app_cmd_perms, ev->app_cmd_perms_count, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: app cmd perms rejected: %s", err.message);
      }
      break;
    case CCORD_NET_WELCOME_SCREEN:
      if (ev->welcome_screen &&
          ccord_store_set_welcome_screen(s->store, ev->guild_id, ev->welcome_screen, &err) != CCORD_OK) {
        CCORD_LOG_W("session: welcome screen rejected: %s", err.message);
      }
      break;
    case CCORD_NET_ONBOARDING:
      if (ev->onboarding && ccord_store_set_onboarding(s->store, ev->guild_id, ev->onboarding, &err) != CCORD_OK) {
        CCORD_LOG_W("session: onboarding rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_OVERWRITE_UPSERT:
      if (ccord_store_upsert_channel_overwrite(s->store, ev->channel_id, ev->guild_id, &ev->overwrite_item, &err) !=
          CCORD_OK) {
        CCORD_LOG_W("session: channel overwrite upsert rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CHANNEL_OVERWRITE_DELETE:
      (void)ccord_store_remove_channel_overwrite(s->store, ev->channel_id, ev->overwrite_item.id);
      break;
    case CCORD_NET_CHANNEL_INVITE_LIST:
      if (ccord_store_set_channel_invites(s->store, ev->guild_id, ev->channel_id, ev->invites, ev->invite_count,
                                          &err) != CCORD_OK) {
        CCORD_LOG_W("session: channel invite list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CALL_CREATE:
    case CCORD_NET_CALL_UPDATE:
      // Ringing is passive: the call row (and the caller's voice state, a separate event) arrives
      // unprompted. Nothing here was asked for by a local click.
      if (ev->call && ccord_store_set_call(s->store, ev->call, &err) != CCORD_OK) {
        CCORD_LOG_W("session: call rejected: %s", err.message);
      }
      break;
    case CCORD_NET_CALL_DELETE:
      (void)ccord_store_remove_call(s->store, ev->channel_id, &err);
      break;
    case CCORD_NET_STAGE_INSTANCE_UPDATE:
      if (ev->stage_deleted) {
        (void)ccord_store_remove_stage_instance(s->store, ev->channel_id, &err);
      } else if (ev->stage_instance &&
                 ccord_store_set_stage_instance(s->store, ev->stage_instance, &err) != CCORD_OK) {
        CCORD_LOG_W("session: stage instance rejected: %s", err.message);
      }
      break;
    case CCORD_NET_SCHEDULED_EVENT_LIST:
      // The REST answer replaces the guild's cached list (an empty answer drops it).
      if (ccord_store_set_scheduled_events(s->store, ev->guild_id, ev->scheduled_events, ev->scheduled_event_count,
                                           &err) != CCORD_OK) {
        CCORD_LOG_W("session: scheduled event list rejected: %s", err.message);
      }
      break;
    case CCORD_NET_SCHEDULED_EVENT_UPDATE:
      if (ev->scheduled_event_deleted) {
        (void)ccord_store_remove_scheduled_event(s->store, ev->guild_id, ev->scheduled_event_id);
      } else if (ev->scheduled_events && ev->scheduled_event_count > 0 &&
                 ccord_store_upsert_scheduled_event(s->store, &ev->scheduled_events[0], &err) != CCORD_OK) {
        CCORD_LOG_W("session: scheduled event rejected: %s", err.message);
      }
      break;
    case CCORD_NET_SCHEDULED_EVENT_USER_CHANGE: {
      // About us: the RSVP state (the store moves user_count with it). About someone else: only the count,
      // and only when it is known (0 means "not reported", never a guess).
      ccord_id self = ccord_store_self_id(s->store);
      if (self != CCORD_ID_NONE && ev->user_id == self) {
        ccord_status rs = ccord_store_set_scheduled_event_rsvp(s->store, ev->guild_id, ev->scheduled_event_id,
                                                               ev->scheduled_event_user_added, &err);
        if (rs != CCORD_OK && rs != CCORD_ERR_NOT_FOUND) {
          CCORD_LOG_W("session: scheduled event rsvp rejected: %s", err.message);
        }
      } else {
        const ccord_scheduled_event *e = ccord_store_find_scheduled_event(s->store, ev->scheduled_event_id);
        if (e && e->guild_id == ev->guild_id && e->user_count > 0) {
          ccord_scheduled_event upd = *e;
          upd.user_count = ev->scheduled_event_user_added ? upd.user_count + 1u : upd.user_count - 1u;
          if (ccord_store_upsert_scheduled_event(s->store, &upd, &err) != CCORD_OK) {
            CCORD_LOG_W("session: scheduled event count rejected: %s", err.message);
          }
        }
      }
      break;
    }
    case CCORD_NET_VOICE_STATUS_UPDATE:
      if (ccord_store_set_voice_status(s->store, ev->channel_id, ev->voice_status, &err) != CCORD_OK) {
        CCORD_LOG_W("session: voice status rejected: %s", err.message);
      }
      break;
    case CCORD_NET_GUILD_PROFILE: {
      session_guild_profile_cache *ent = sess_guild_profile_for(s, ev->guild_id);
      if (ent) {
        ent->profile = ev->guild_profile;
        if (ent->profile.guild_id == CCORD_ID_NONE) ent->profile.guild_id = ev->guild_id;
        ent->has_profile = true;
        ent->loading = false;
        ent->sent_ms = 0;
        uint64_t now = ccord_clock_ms();
        ent->fetched_ms = now ? now : 1u;
      }
      break;
    }
    case CCORD_NET_ACTIVITY_SHELF: {
      // The REST answer replaces the guild's cached shelf (an empty answer drops it).
      session_activity_shelf *ent = sess_activity_shelf_for(s, ev->guild_id);
      if (ent) {
        size_t n = ev->activity_apps ? ev->activity_app_count : 0u;
        if (n > SESSION_ACTIVITY_SHELF_MAX) n = SESSION_ACTIVITY_SHELF_MAX;
        if (n > 0) memcpy(ent->apps, ev->activity_apps, n * sizeof ent->apps[0]);
        ent->count = n;
        ent->loading = false;
        ent->sent_ms = 0;
        uint64_t now = ccord_clock_ms();
        ent->fetched_ms = now ? now : 1u;
      }
      break;
    }
    case CCORD_NET_ACTIVITY_INSTANCE_UPDATE:
      // participant_count 0 ends the instance; the store upsert removes the row.
      if (ccord_store_upsert_activity_instance(s->store, &ev->activity_instance, &err) != CCORD_OK) {
        CCORD_LOG_W("session: activity instance rejected: %s", err.message);
      }
      break;
    case CCORD_NET_USER_SETTINGS:
      // REST answer and gateway USER_SETTINGS_UPDATE are the same full object: the store is the single source.
      if (ev->user_settings &&
          ccord_store_set_user_settings(s->store, ev->user_settings, &err) != CCORD_OK) {
        CCORD_LOG_W("session: user settings rejected: %s", err.message);
      }
      break;
    case CCORD_NET_AUTH_SESSIONS: {
      size_t n = ev->auth_sessions ? ev->auth_session_count : 0u;
      if (n > CCORD_SESSION_MAX_AUTH_SESSIONS) n = CCORD_SESSION_MAX_AUTH_SESSIONS;
      if (n > 0) memcpy(s->auth_sessions, ev->auth_sessions, n * sizeof s->auth_sessions[0]);
      s->auth_session_count = n;
      s->auth_sessions_state = CCORD_FETCH_READY;
      s->auth_sessions_fetched_ms = s->auth_sessions_sent_ms
                                        ? s->auth_sessions_sent_ms
                                        : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
      s->auth_sessions_sent_ms = 0;
      break;
    }
    case CCORD_NET_AUTH_SESSIONS_LOGOUT: {
      size_t n = ev->session_hash_count;
      if (n > CCORD_ADMIN_SESSION_LOGOUT_MAX) n = CCORD_ADMIN_SESSION_LOGOUT_MAX;
      sess_drop_auth_sessions(s, ev->session_hashes, n);
      break;
    }
    case CCORD_NET_CONNECTIONS: {
      size_t n = ev->connections ? ev->connection_count : 0u;
      if (n > CCORD_SESSION_MAX_CONNECTIONS) n = CCORD_SESSION_MAX_CONNECTIONS;
      if (n > 0) memcpy(s->connections, ev->connections, n * sizeof s->connections[0]);
      s->connection_count = n;
      s->connections_state = CCORD_FETCH_READY;
      s->connections_fetched_ms =
        s->connections_sent_ms ? s->connections_sent_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
      s->connections_sent_ms = 0;
      break;
    }
    case CCORD_NET_CONNECTION_UPSERT:
      sess_upsert_connection(s, &ev->connection);
      break;
    case CCORD_NET_CONNECTION_DELETE:
      sess_drop_connection(s, ev->connection_name, ev->connection_id);
      break;
    case CCORD_NET_REPORT_MENU:
    case CCORD_NET_REPORT_SUBMITTED:
      sess_queue_report_event(s, ev);
      break;
    case CCORD_NET_ACCOUNT_PRIVATE:
      if (ev->account_private) sess_account_apply_private(s, ev->account_private);
      if (ev->users && ev->user_count > 0) {
        (void)ccord_store_upsert_user(s->store, &ev->users[0], &err);
      }
      break;
    case CCORD_NET_NONE:
      break;
    default: break;
  }
}


