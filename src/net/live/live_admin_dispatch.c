// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void live_admin_done(live *l, const live_rest_op *op, const char *body, size_t len)
{
  ccord_error err;
  ccord_live_event ev;
  ccord_live_event_init(&ev);
  bool have = false;
  bool quiet = false;   // no answer to apply is the normal outcome (204): don't log it
  switch (op->admin_op) {
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_CLONE_CHANNEL:
    case CCORD_ADMIN_EDIT_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG:
    case CCORD_ADMIN_ARCHIVE_THREAD:
      have = ccord_live_json_channel(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD:
      ev.ev.type = CCORD_NET_THREAD_MEMBER_SELF;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      ev.ev.user_id = l->self_id;
      ev.ev.thread_joined = op->admin_op == CCORD_ADMIN_JOIN_THREAD;
      have = true;
      break;
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST:
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_CHANNEL_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      have = true;
      break;
    case CCORD_ADMIN_EDIT_GUILD:
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
      have = ccord_live_json_guild(body, len, &ev, &err) == CCORD_OK;
      if (have) ev.ev.admin_op = op->admin_op;
      break;
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS:
      if (body && len > 0 && ccord_live_json_incident_actions(body, len, op->guild_id, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        ev.ev.type = CCORD_NET_GUILD_INCIDENTS;
        ev.ev.guild_id = op->guild_id;
        ev.ev.invites_disabled_until_ms = op->invites_disabled_until_ms;
        ev.ev.dms_disabled_until_ms = op->dms_disabled_until_ms;
        have = true;
      }
      break;
    case CCORD_ADMIN_SET_VANITY_URL:
      if (body && len > 0 && ccord_live_json_vanity_url(body, len, op->guild_id, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        size_t clen = strlen(op->invite_code);
        ccord_arena_init(&ev.arena, clen + 32u);
        ev.ev.invite_code = ccord_arena_strndup(&ev.arena, op->invite_code, clen);
        if (ev.ev.invite_code) {
          ev.ev.type = CCORD_NET_GUILD_VANITY;
          ev.ev.guild_id = op->guild_id;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_LEAVE_GUILD:
    case CCORD_ADMIN_DELETE_GUILD:
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_GUILD_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.admin_op = op->admin_op;
      have = true;
      break;
    case CCORD_ADMIN_SET_NICK:
    case CCORD_ADMIN_TIMEOUT:
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF:
    case CCORD_ADMIN_MOVE_VOICE_MEMBER:
      have = body && len > 0 && ccord_live_json_member(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN:
      ccord_live_event_init(&ev);
      ev.ev.type = CCORD_NET_MEMBER_REMOVE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.user_id = op->user_id;
      have = true;
      break;
    case CCORD_ADMIN_CREATE_ROLE:
    case CCORD_ADMIN_EDIT_ROLE:
    case CCORD_ADMIN_REORDER_ROLES:
      have = ccord_live_json_role(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_ROLE:
      ev.ev.type = CCORD_NET_ROLE_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.role_id = op->role_id;
      have = true;
      break;
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE:
      ev.ev.type = CCORD_NET_MEMBER_ROLE_CHANGE;   // Discord answers 204; the change is what we asked for
      ev.ev.guild_id = op->guild_id;
      ev.ev.user_id = op->user_id;
      ev.ev.role_id = op->role_id;
      ev.ev.role_added = op->admin_op == CCORD_ADMIN_ADD_MEMBER_ROLE;
      have = true;
      break;
    case CCORD_ADMIN_UNBAN:
      ev.ev.type = CCORD_NET_BAN_REMOVE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.user_id = op->user_id;
      have = true;
      break;
    case CCORD_ADMIN_CREATE_INVITE:
    case CCORD_ADMIN_LIST_INVITES:
      have = ccord_live_json_invites(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_REVOKE_INVITE:
      ev.ev.type = CCORD_NET_INVITE_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.invite_code = ccord_arena_strndup(&ev.arena, op->invite_code, strlen(op->invite_code));
      have = ev.ev.invite_code != nullptr;
      break;
    case CCORD_ADMIN_LIST_BANS:
      have = ccord_live_json_bans(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_LIST_AUDIT_LOG:
      have = ccord_live_json_audit_log(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_SET_NOTIFY:
      have = ccord_live_json_notify_settings(body, len, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
      if (body && len > 0 && ccord_live_json_user_settings(body, len, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_init(&ev);
        if (op->folder_item_count > 0 && op->folder_items) {
          ccord_arena_init(&ev.arena, op->folder_item_count * sizeof(ccord_guild_folder_item) + 64u);
          ccord_guild_folder_item *arr = ccord_arena_alloc(&ev.arena, op->folder_item_count * sizeof *arr,
                                                           _Alignof(ccord_guild_folder_item));
          if (arr) {
            memcpy(arr, op->folder_items, op->folder_item_count * sizeof *arr);
            ev.ev.type = CCORD_NET_GUILD_LAYOUT;
            ev.ev.folder_items = arr;
            ev.ev.folder_item_count = op->folder_item_count;
            have = true;
          }
        } else {
          ev.ev.type = CCORD_NET_GUILD_LAYOUT;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_REORDER_CHANNELS:
      if (op->reorder && op->reorder->count > 0) {
        size_t n = op->reorder->count;
        ccord_arena_init(&ev.arena, n * (sizeof(ccord_id) * 2u + sizeof(int32_t) + 1u) + 64u);
        ccord_id *ids = ccord_arena_alloc(&ev.arena, n * sizeof *ids, _Alignof(ccord_id));
        int32_t *pos = ccord_arena_alloc(&ev.arena, n * sizeof *pos, _Alignof(int32_t));
        ccord_id *pars = ccord_arena_alloc(&ev.arena, n * sizeof *pars, _Alignof(ccord_id));
        uint8_t *hpars = ccord_arena_alloc(&ev.arena, n, 1u);
        if (ids && pos && pars && hpars) {
          memcpy(ids, op->reorder->ids, n * sizeof *ids);
          memcpy(pos, op->reorder->positions, n * sizeof *pos);
          memcpy(pars, op->reorder->parents, n * sizeof *pars);
          memcpy(hpars, op->reorder->has_parent, n);
          ev.ev.type = CCORD_NET_CHANNEL_POSITIONS;
          ev.ev.guild_id = op->guild_id;
          ev.ev.reorder_ids = ids;
          ev.ev.reorder_positions = pos;
          ev.ev.reorder_parents = pars;
          ev.ev.reorder_has_parent = hpars;
          ev.ev.reorder_count = n;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_CREATE_GUILD:
      have = ccord_live_json_guild(body, len, &ev, &err) == CCORD_OK;
      if (have) ev.ev.admin_op = CCORD_ADMIN_CREATE_GUILD;
      break;
    case CCORD_ADMIN_PREVIEW_INVITE:
      have = ccord_live_json_invite_preview(body, len, false, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_ACCEPT_INVITE:
      have = ccord_live_json_invite_preview(body, len, true, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_CREATE_GROUP_DM:
    case CCORD_ADMIN_EDIT_GROUP_DM:
      have = ccord_live_json_channel(body, len, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_OK;
      if (have) ev.ev.admin_op = op->admin_op;
      break;
    case CCORD_ADMIN_ADD_DM_RECIPIENT:
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT:
      ev.ev.type = CCORD_NET_DM_RECIPIENT_CHANGE;
      ev.ev.guild_id = CCORD_DM_GUILD_ID;
      ev.ev.channel_id = op->channel_id;
      ev.ev.user_id = op->user_id;
      ev.ev.recipient_added = op->admin_op == CCORD_ADMIN_ADD_DM_RECIPIENT;
      have = true;
      break;
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST:
      if (body && len > 0 && ccord_live_json_channel(body, len, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_OK) {
        if (ev.channel_count > 0 && ev.channels) {
          ev.channels[0].is_message_request = false;
          ev.channels[0].is_spam = false;
        }
        ev.ev.admin_op = CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST;
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        ev.ev.type = CCORD_NET_MESSAGE_REQUEST_ACCEPTED;
        ev.ev.guild_id = CCORD_DM_GUILD_ID;
        ev.ev.channel_id = op->channel_id;
        have = true;
      }
      break;
    case CCORD_ADMIN_SET_USER_NOTE:
      if (body && len > 0 && ccord_live_json_user_note(body, len, op->user_id, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        size_t nlen = strlen(op->user_note);
        ccord_arena_init(&ev.arena, nlen + 32u);
        ev.ev.text = ccord_arena_strndup(&ev.arena, op->user_note, nlen);
        if (ev.ev.text) {
          ev.ev.type = CCORD_NET_USER_NOTE_UPDATE;
          ev.ev.user_id = op->user_id;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_SET_CUSTOM_STATUS:
      if (body && len > 0 && ccord_live_json_custom_status(body, len, &ev, &err) == CCORD_OK) {
        have = true;
      } else {
        ccord_live_event_free(&ev);
        ccord_live_event_init(&ev);
        ev.ev.type = CCORD_NET_CUSTOM_STATUS_UPDATE;
        ev.ev.custom_status_clear = op->custom_status_clear;
        if (!op->custom_status_clear) {
          if (op->custom_status_emoji_id != CCORD_ID_NONE && op->custom_status_emoji[0] != '\0') {   // stored as name:id
            snprintf(ev.ev.custom_status_emoji, sizeof ev.ev.custom_status_emoji, "%.40s:%llu", op->custom_status_emoji,
                     (unsigned long long)op->custom_status_emoji_id);
          } else {
            memcpy(ev.ev.custom_status_emoji, op->custom_status_emoji, sizeof ev.ev.custom_status_emoji);
          }
          memcpy(ev.ev.custom_status_text, op->custom_status_text, sizeof ev.ev.custom_status_text);
          ev.ev.custom_status_expires_ms = op->custom_status_expires_ms;
        }
        have = true;
      }
      break;
    case CCORD_ADMIN_LIST_EMOJIS:
    case CCORD_ADMIN_CREATE_EMOJI:
    case CCORD_ADMIN_EDIT_EMOJI:
      have = ccord_live_json_guild_emojis(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_EMOJI:
      ev.ev.type = CCORD_NET_EMOJI_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_LIST_STICKERS:
    case CCORD_ADMIN_CREATE_STICKER:
    case CCORD_ADMIN_EDIT_STICKER:
      have = ccord_live_json_guild_stickers(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_STICKER:
      ev.ev.type = CCORD_NET_STICKER_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_LIST_SOUNDBOARD:
    case CCORD_ADMIN_CREATE_SOUNDBOARD:
    case CCORD_ADMIN_EDIT_SOUNDBOARD:
      have = ccord_live_json_guild_soundboard(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_SOUNDBOARD:
      ev.ev.type = CCORD_NET_SOUNDBOARD_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_PREVIEW_PRUNE:
      have = body && ccord_live_json_prune(body, len, op->guild_id, op->prune_days, false, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_EXECUTE_PRUNE:
      have = body && ccord_live_json_prune(body, len, op->guild_id, op->prune_days, true, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_LIST_AUTOMOD_RULES:
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE:
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE:
      have = body && ccord_live_json_automod_rules(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE:
      ev.ev.type = CCORD_NET_AUTOMOD_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_LIST_WEBHOOKS:
    case CCORD_ADMIN_CREATE_WEBHOOK:
    case CCORD_ADMIN_EDIT_WEBHOOK:
      have = body && ccord_live_json_webhooks(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_DELETE_WEBHOOK:
      ev.ev.type = CCORD_NET_WEBHOOK_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.expr_id = op->expr_id;
      have = true;
      break;
    case CCORD_ADMIN_FOLLOW_CHANNEL:
      have = body && ccord_live_json_followed_channel(body, len, op->guild_id, op->channel_id, op->target_channel_id,
                                                      &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_LIST_APP_CMD_PERMS:
      have = body && ccord_live_json_app_cmd_perms(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN:
      have = body && ccord_live_json_welcome_screen(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING:
      have = body && ccord_live_json_onboarding(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_MEMBER_VERIFICATION:
      have = body && ccord_live_json_member_verification(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_GET_GUILD_PROFILE:
      have = body && ccord_live_json_guild_profile(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      if (!have) {
        ccord_live_event_free(&ev);
        live_fill_guild_profile_unavailable(&ev, op->guild_id);
        have = true;
      }
      break;
    case CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION:
      // Discord answers 204 (or a join-request object): only a member object is applied here; the gateway's
      // GUILD_MEMBER_UPDATE carries pending=false either way, so nothing is guessed locally.
      have = body && len > 0 && ccord_live_json_member(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      quiet = !have;
      break;
    case CCORD_ADMIN_ACTIVITY_SHELF:
      have = body && ccord_live_json_activity_shelf(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_ACTIVITY_LAUNCH:
    case CCORD_ADMIN_ACTIVITY_LEAVE:
      // Discord answers 204: the running (or ended) instance arrives as EMBEDDED_ACTIVITY_UPDATE_V2, so nothing
      // is guessed locally from the launch/leave answer.
      quiet = true;
      break;
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE:
      ev.ev.type = CCORD_NET_CHANNEL_OVERWRITE_UPSERT;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      ev.ev.overwrite_item = op->overwrite;
      have = true;
      break;
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE:
      ev.ev.type = CCORD_NET_CHANNEL_OVERWRITE_DELETE;
      ev.ev.guild_id = op->guild_id;
      ev.ev.channel_id = op->channel_id;
      ev.ev.overwrite_item = op->overwrite;
      have = true;
      break;
    case CCORD_ADMIN_LIST_CHANNEL_INVITES:
      have = body && ccord_live_json_channel_invites(body, len, op->guild_id, op->channel_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_VOICE_STATUS:
      if (op->channel_id != CCORD_ID_NONE) {
        const char *vs = op->voice_status ? op->voice_status : "";
        size_t slen = strlen(vs);
        ccord_arena_init(&ev.arena, slen + 32u);
        char *copy = ccord_arena_strndup(&ev.arena, vs, slen);
        if (copy) {
          ev.ev.type = CCORD_NET_VOICE_STATUS_UPDATE;
          ev.ev.guild_id = op->guild_id != CCORD_ID_NONE ? op->guild_id : l->selected_guild;
          ev.ev.channel_id = op->channel_id;
          ev.ev.voice_status = copy;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_STAGE_REQUEST_SPEAK: {
      ccord_id uid = op->user_id != CCORD_ID_NONE ? op->user_id : l->self_id;
      if (uid != CCORD_ID_NONE && op->guild_id != CCORD_ID_NONE && op->channel_id != CCORD_ID_NONE) {
        const ccord_voice_state *cached = live_voice_cache_find(l, op->guild_id, uid);
        bool self_active = uid == l->self_id && l->voice_guild == op->guild_id && l->voice_channel == op->channel_id;
        // Only synthesize when the participant's flags are known (cached dispatch or our own live state); an
        // unknown state must not be overwritten with zeroes — the gateway dispatch will carry the real flags.
        if (cached || self_active) {
          ccord_arena_init(&ev.arena, sizeof(ccord_voice_state) + 32u);
          ccord_voice_state *v = ccord_arena_alloc(&ev.arena, sizeof *v, _Alignof(ccord_voice_state));
          if (v) {
            if (cached) {
              *v = *cached;   // preserve server mute/deaf and self_* the transport does not own
            } else {
              memset(v, 0, sizeof *v);
            }
            v->guild_id = op->guild_id;
            v->channel_id = op->channel_id;
            v->user_id = uid;
            if (self_active) {
              v->self_mute = l->self_mute;
              v->self_deaf = l->self_deaf;
              v->self_video = l->self_video;
              v->self_stream = l->streaming;
            }
            v->request_to_speak_timestamp = op->request_to_speak_ms;
            v->omit_suppress = true;
            v->omit_request_to_speak = false;
            ev.voice_states = v;
            ev.voice_state_count = 1;
            ev.ev.type = CCORD_NET_VOICE_STATE_UPDATE;
            ev.ev.guild_id = v->guild_id;
            ev.ev.channel_id = v->channel_id;
            ev.ev.user_id = v->user_id;
            ev.ev.voice_state = v;
            have = true;
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_SET_SPEAKER: {
      if (op->user_id != CCORD_ID_NONE && op->guild_id != CCORD_ID_NONE && op->channel_id != CCORD_ID_NONE) {
        bool self_active = op->user_id == l->self_id && l->voice_guild == op->guild_id &&
                           l->voice_channel == op->channel_id;
        if (self_active) {
          l->self_suppressed = !op->stage_speaker;
          if (l->voice) ccord_voice_gateway_set_suppressed(l->voice, l->self_suppressed);
        }
        const ccord_voice_state *cached = live_voice_cache_find(l, op->guild_id, op->user_id);
        // Preserve the participant's known flags; never zero mute/deaf/self_* we do not own. If the state is
        // unknown, skip the synthesized overwrite and let the gateway dispatch deliver the authoritative flags.
        if (cached || self_active) {
          ccord_arena_init(&ev.arena, sizeof(ccord_voice_state) + 32u);
          ccord_voice_state *v = ccord_arena_alloc(&ev.arena, sizeof *v, _Alignof(ccord_voice_state));
          if (v) {
            if (cached) {
              *v = *cached;
            } else {
              memset(v, 0, sizeof *v);
            }
            v->guild_id = op->guild_id;
            v->channel_id = op->channel_id;
            v->user_id = op->user_id;
            if (self_active) {
              v->self_mute = l->self_mute;
              v->self_deaf = l->self_deaf;
              v->self_video = l->self_video;
              v->self_stream = l->streaming;
            }
            v->suppress = !op->stage_speaker;
            v->omit_suppress = false;
            if (op->stage_speaker) {
              v->request_to_speak_timestamp = 0;
              v->omit_request_to_speak = false;
            } else {
              v->omit_request_to_speak = true;
            }
            ev.voice_states = v;
            ev.voice_state_count = 1;
            ev.ev.type = CCORD_NET_VOICE_STATE_UPDATE;
            ev.ev.guild_id = v->guild_id;
            ev.ev.channel_id = v->channel_id;
            ev.ev.user_id = v->user_id;
            ev.ev.voice_state = v;
            have = true;
          }
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_CREATE:
    case CCORD_ADMIN_STAGE_TOPIC: {
      ccord_id mapped_guild = live_chan_guild_find(l, op->channel_id);
      ccord_id ghint = op->guild_id != CCORD_ID_NONE
                         ? op->guild_id
                         : (mapped_guild != CCORD_ID_NONE
                              ? mapped_guild
                              : (l->selected_guild != CCORD_ID_NONE ? l->selected_guild : l->voice_guild));
      if (body && len > 0) {
        have = ccord_live_json_stage_instance(body, len, ghint, op->channel_id, &ev, &err) == CCORD_OK;
      } else if (op->has_topic && op->channel_id != CCORD_ID_NONE) {
        const char *tp = op->topic ? op->topic : "";
        size_t tlen = strlen(tp);
        ccord_arena_init(&ev.arena, sizeof(ccord_stage_instance) + 32u);
        ccord_stage_instance *si = ccord_arena_alloc(&ev.arena, sizeof *si, _Alignof(ccord_stage_instance));
        if (si && tlen < sizeof si->topic) {
          memset(si, 0, sizeof *si);
          si->channel_id = op->channel_id;
          si->guild_id = ghint;
          if (op->admin_op == CCORD_ADMIN_STAGE_CREATE) si->privacy_level = 2u;
          memcpy(si->topic, tp, tlen + 1u);
          ev.ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
          ev.ev.guild_id = si->guild_id;
          ev.ev.channel_id = si->channel_id;
          ev.ev.stage_instance = si;
          ev.ev.stage_deleted = false;
          have = true;
        }
      }
      break;
    }
    case CCORD_ADMIN_STAGE_DELETE: {
      ccord_id mapped_guild = live_chan_guild_find(l, op->channel_id);
      ccord_id ghint = op->guild_id != CCORD_ID_NONE
                         ? op->guild_id
                         : (mapped_guild != CCORD_ID_NONE
                              ? mapped_guild
                              : (l->selected_guild != CCORD_ID_NONE ? l->selected_guild : l->voice_guild));
      if (body && len > 0 && ccord_live_json_stage_instance(body, len, ghint, op->channel_id, &ev, &err) == CCORD_OK) {
        ev.ev.stage_deleted = true;
        have = true;
      } else if (op->channel_id != CCORD_ID_NONE) {
        ccord_arena_init(&ev.arena, sizeof(ccord_stage_instance) + 32u);
        ccord_stage_instance *si = ccord_arena_alloc(&ev.arena, sizeof *si, _Alignof(ccord_stage_instance));
        if (si) {
          memset(si, 0, sizeof *si);
          si->channel_id = op->channel_id;
          si->guild_id = ghint;
          ev.ev.type = CCORD_NET_STAGE_INSTANCE_UPDATE;
          ev.ev.guild_id = ghint;
          ev.ev.channel_id = op->channel_id;
          ev.ev.stage_instance = si;
          ev.ev.stage_deleted = true;
          have = true;
        }
      }
      break;
    }
    case CCORD_ADMIN_EVENT_LIST:
      have = body && ccord_live_json_scheduled_event_list(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_EVENT_CREATE:
    case CCORD_ADMIN_EVENT_MODIFY:
      have = body && ccord_live_json_scheduled_event(body, len, op->guild_id, &ev, &err) == CCORD_OK;
      break;
    case CCORD_ADMIN_EVENT_DELETE:
      if (op->guild_id != CCORD_ID_NONE && op->event_id != CCORD_ID_NONE) {
        ccord_arena_init(&ev.arena, sizeof(ccord_scheduled_event) + 32u);
        ccord_scheduled_event *se = ccord_arena_alloc(&ev.arena, sizeof *se, _Alignof(ccord_scheduled_event));
        if (se) {
          memset(se, 0, sizeof *se);
          se->id = op->event_id;
          se->guild_id = op->guild_id;
          ev.ev.type = CCORD_NET_SCHEDULED_EVENT_UPDATE;
          ev.ev.guild_id = op->guild_id;
          ev.ev.scheduled_events = se;
          ev.ev.scheduled_event_count = 1;
          ev.ev.scheduled_event_id = op->event_id;
          ev.ev.scheduled_event_deleted = true;
          have = true;
        }
      }
      break;
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE:
      if (op->guild_id != CCORD_ID_NONE && op->event_id != CCORD_ID_NONE) {
        ev.ev.type = CCORD_NET_SCHEDULED_EVENT_USER_CHANGE;
        ev.ev.guild_id = op->guild_id;
        ev.ev.scheduled_event_id = op->event_id;
        ev.ev.user_id = l->self_id;
        ev.ev.scheduled_event_user_added = op->admin_op == CCORD_ADMIN_EVENT_RSVP_ADD;
        have = true;
      }
      break;
    case CCORD_ADMIN_USER_SETTINGS_PATCH:
      // PATCH /users/@me/settings answers with the full object: publish it as the account-settings event,
      // regardless of whether the same body also carries guild_folders.
      if (body && len > 0 && ccord_live_json_user_settings(body, len, &ev, &err) == CCORD_OK &&
          ev.has_user_settings) {
        ev.ev.type = CCORD_NET_USER_SETTINGS;
        ev.ev.user_settings = &ev.user_settings;
        have = true;
      }
      break;
    case CCORD_ADMIN_AUTH_SESSIONS_LIST:
      if (body && ccord_live_json_auth_sessions(body, len, &ev, &err) == CCORD_OK) {
        ev.ev.type = CCORD_NET_AUTH_SESSIONS;
        ev.ev.auth_sessions = ev.auth_sessions;
        ev.ev.auth_session_count = ev.auth_session_count;
        have = true;
      }
      break;
    case CCORD_ADMIN_AUTH_SESSIONS_LOGOUT:
      // Discord answers 204: the acknowledged hashes are the only payload, and they are exactly what it logged out.
      ev.ev.type = CCORD_NET_AUTH_SESSIONS_LOGOUT;
      ev.ev.session_hash_count = op->session_hash_count;
      for (size_t i = 0; i < op->session_hash_count; i++) {
        memcpy(ev.ev.session_hashes[i], op->session_hashes[i], sizeof ev.ev.session_hashes[i]);
      }
      have = true;
      break;
    case CCORD_ADMIN_CONNECTIONS_LIST:
      if (body && ccord_live_json_connections(body, len, &ev, &err) == CCORD_OK) {
        ev.ev.type = CCORD_NET_CONNECTIONS;
        ev.ev.connections = ev.connections;
        ev.ev.connection_count = ev.connection_count;
        have = true;
      }
      break;
    case CCORD_ADMIN_CONNECTION_EDIT:
      // PATCH answers with the one changed connection, never a fabricated full list.
      if (body && ccord_live_json_connections(body, len, &ev, &err) == CCORD_OK && ev.connection_count > 0) {
        ev.ev.type = CCORD_NET_CONNECTION_UPSERT;
        ev.ev.connection = ev.connections[0];
        have = true;
      }
      break;
    case CCORD_ADMIN_CONNECTION_DELETE:
      // 204: echo the request target so the UI removes the entry Discord accepted.
      ev.ev.type = CCORD_NET_CONNECTION_DELETE;
      memcpy(ev.ev.connection_name, op->connection_name, sizeof ev.ev.connection_name);
      memcpy(ev.ev.connection_id, op->connection_id, sizeof ev.ev.connection_id);
      have = true;
      break;
    case CCORD_ADMIN_CALL_STOP_RINGING: {
      // Discord answered 204: drop self from the cached ringing[] and publish the updated call row. This is a
      // REST decline, not a voice disconnect — no guild voice the user is in may be touched here.
      long ci = live_call_index(l, op->channel_id);
      if (ci >= 0) {
        ccord_call_state *call = &l->dm_calls[(size_t)ci];
        (void)live_call_drop_ringing(call, l->self_id);
        ccord_arena_init(&ev.arena, sizeof *call + 32u);
        ccord_call_state *copy = ccord_arena_alloc(&ev.arena, sizeof *copy, _Alignof(ccord_call_state));
        if (copy) {
          *copy = *call;
          ev.ev.type = CCORD_NET_CALL_UPDATE;
          ev.ev.guild_id = CCORD_DM_GUILD_ID;
          ev.ev.channel_id = op->channel_id;
          ev.ev.call = copy;
          have = true;
        }
      }
      break;
    }
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_OP_COUNT:
      break;
    default: break;  // ops whose REST answer needs no local parsing (the gateway dispatch brings the change)
  }
  if (have) {
    (void)live_push(l, &ev);
  } else {
    // Discord accepted it; we just could not read the answer. The gateway dispatch will bring the change in.
    ccord_live_event_free(&ev);
    if (!quiet) CCORD_LOG_W("live: admin %s response not applied", ccord_admin_op_verb(op->admin_op));
  }
}


static ccord_rest_method live_admin_method(ccord_admin_method m)
{
  switch (m) {
    case CCORD_ADMIN_POST: return CCORD_REST_POST;
    case CCORD_ADMIN_PATCH: return CCORD_REST_PATCH;
    case CCORD_ADMIN_PUT: return CCORD_REST_PUT;
    case CCORD_ADMIN_DELETE: return CCORD_REST_DELETE;
    case CCORD_ADMIN_GET: return CCORD_REST_GET;
  }
  return CCORD_REST_POST;
}

ccord_status live_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *err)
{
  live *l = (live *)t;
  ccord_error_clear(err);
  if (!req || req->op == CCORD_ADMIN_NONE || req->op >= CCORD_ADMIN_OP_COUNT || req->path[0] != '/') {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: bad admin request");
  }
  if (!l->restq) return ccord_error_set(err, CCORD_ERR_IO, "live: REST queue unavailable");
  // Launching an activity names the caller's gateway session in the body so Discord can join them to the
  // instance. The session layer never sees that id (it passes a stand-in), so rebuild the request with the real
  // READY session id; without a gateway session nothing is sent. Heap-allocated only for this rare op so
  // live_admin_action does not reserve ~98 KiB of stack on every admin call.
  ccord_admin_request *launch_buf = nullptr;
  if (req->op == CCORD_ADMIN_ACTIVITY_LAUNCH) {
    const char *sid = l->gw_session_id[0] ? l->gw_session_id : (l->gw ? ccord_gateway_session_id(l->gw) : nullptr);
    if (!sid || sid[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: not connected to the gateway yet");
    }
    launch_buf = ccord_calloc(1, sizeof *launch_buf);
    if (!launch_buf) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    ccord_status ls = ccord_admin_activity_launch(launch_buf, req->channel_id, req->application_id, sid, err);
    if (ls != CCORD_OK) {
      ccord_free(launch_buf);
      return ls;
    }
    req = launch_buf;
  }
  live_rest_op *op = live_action_op(l, LIVE_REST_ADMIN, req->channel_id, CCORD_ID_NONE, nullptr, 0);
  if (!op) {
    ccord_free(launch_buf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
  }
  op->guild_id = req->guild_id;
  op->user_id = req->user_id;
  op->role_id = req->role_id;
  op->expr_id = req->expr_id;
  op->event_id = req->event_id;
  op->target_channel_id = req->target_channel_id;
  op->overwrite = req->overwrite;
  op->prune_days = req->prune_days;
  (void)snprintf(op->invite_code, sizeof op->invite_code, "%s", req->code);
  op->admin_op = req->op;
  if (req->op == CCORD_ADMIN_REORDER_CHANNELS && req->reorder_count > 0) {
    size_t n = req->reorder_count <= CCORD_ADMIN_CHANNEL_REORDER_MAX
                 ? req->reorder_count
                 : CCORD_ADMIN_CHANNEL_REORDER_MAX;
    op->reorder = ccord_calloc(1, sizeof *op->reorder);
    if (!op->reorder) {
      live_rest_op_free(op);
      ccord_free(launch_buf);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    op->reorder->count = n;
    memcpy(op->reorder->ids, req->reorder_ids, n * sizeof op->reorder->ids[0]);
    memcpy(op->reorder->positions, req->reorder_positions, n * sizeof op->reorder->positions[0]);
    memcpy(op->reorder->parents, req->reorder_parents, n * sizeof op->reorder->parents[0]);
    memcpy(op->reorder->has_parent, req->reorder_has_parent, n);
  }
  if (req->op == CCORD_ADMIN_SET_GUILD_FOLDERS && req->folder_item_count > 0) {
    op->folder_items = ccord_alloc_array(req->folder_item_count, sizeof *op->folder_items);
    if (!op->folder_items) {
      live_rest_op_free(op);
      ccord_free(launch_buf);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
    memcpy(op->folder_items, req->folder_items, req->folder_item_count * sizeof *op->folder_items);
    op->folder_item_count = req->folder_item_count;
  }
  if (req->op == CCORD_ADMIN_SET_USER_NOTE) {
    memcpy(op->user_note, req->note, sizeof op->user_note);
  }
  if (req->op == CCORD_ADMIN_SET_CUSTOM_STATUS) {
    memcpy(op->custom_status_emoji, req->custom_status_emoji, sizeof op->custom_status_emoji);
    memcpy(op->custom_status_text, req->custom_status_text, sizeof op->custom_status_text);
    op->custom_status_emoji_id = req->custom_status_emoji_id;
    op->custom_status_expires_ms = req->until_ms;
    op->custom_status_clear = req->custom_status_clear;
  }
  if (req->op == CCORD_ADMIN_SET_INCIDENT_ACTIONS) {
    op->invites_disabled_until_ms = req->invites_disabled_until_ms;
    op->dms_disabled_until_ms = req->dms_disabled_until_ms;
  }
  if (req->op == CCORD_ADMIN_VOICE_STATUS && req->voice_status[0] != '\0') {
    op->voice_status = ccord_strdup(req->voice_status);
    if (!op->voice_status) {
      live_rest_op_free(op);
      ccord_free(launch_buf);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
    }
  }
  if (req->op == CCORD_ADMIN_STAGE_REQUEST_SPEAK) {
    op->request_to_speak_ms = req->request_to_speak_ms;
  }
  if (req->op == CCORD_ADMIN_STAGE_SET_SPEAKER) {
    op->stage_speaker = req->stage_speaker;
  }
  if (req->op == CCORD_ADMIN_STAGE_TOPIC || req->op == CCORD_ADMIN_STAGE_CREATE) {
    op->has_topic = req->has_topic;
    if (req->topic[0] != '\0') {
      op->topic = ccord_strdup(req->topic);
      if (!op->topic) {
        live_rest_op_free(op);
        ccord_free(launch_buf);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "live: out of memory");
      }
    }
  }
  if (req->op == CCORD_ADMIN_CONNECTION_EDIT || req->op == CCORD_ADMIN_CONNECTION_DELETE) {
    memcpy(op->connection_name, req->connection_name, sizeof op->connection_name);
    memcpy(op->connection_id, req->connection_id, sizeof op->connection_id);
  }
  if (req->op == CCORD_ADMIN_AUTH_SESSIONS_LOGOUT) {
    size_t n = req->session_hash_count <= CCORD_ADMIN_SESSION_LOGOUT_MAX
                 ? req->session_hash_count
                 : CCORD_ADMIN_SESSION_LOGOUT_MAX;
    op->session_hash_count = n;
    memcpy(op->session_hashes, req->session_hashes, n * sizeof op->session_hashes[0]);
  }
  ccord_rest_request r = ccord_rest_request_make(live_admin_method(req->method), req->path, live_kind_priority(op->kind));
  if (req->content_type[0] != '\0' || req->body_len > 0) {
    r.body = req->body_len > 0 ? req->body : (req->body[0] != '\0' ? req->body : nullptr);
    r.body_len = req->body_len > 0 ? req->body_len : strlen(req->body);
    r.content_type = req->content_type[0] != '\0' ? req->content_type : "application/json";
  } else {
    r.json_body = req->body[0] != '\0' ? req->body : nullptr;
  }
  r.audit_reason = req->reason[0] != '\0' ? req->reason : nullptr;
  r.cb = live_rest_done;
  r.ud = op;
  ccord_status st = ccord_rest_queue_submit(l->restq, &r, err);
  ccord_free(launch_buf);
  if (st != CCORD_OK) live_rest_op_free(op);
  return st;
}


