// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_transport_harness.h"

static void test_voice_message_upload_playback(void)
{
  ccord_error err;
  g_now = 35000;
  ccord_mock_config cfg = {.seed = 31, .big_channel_messages = 3, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  static const uint8_t ogg[] = {'O', 'g', 'g', 'S', 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xAA, 0xBB, 0xCC};
  const char *waveform = "AAECAwQFBgc=";
  ccord_upload_file voice = {
    .filename = "voice-message.ogg", .data = ogg, .size = sizeof ogg, .duration_ms = 3250, .waveform_b64 = waveform,
  };
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, false, "", 0, &voice, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.message_count == 1);
  const ccord_message_rich *r = ev.messages[0].rich;
  CT_CHECK(r != nullptr);
  CT_CHECK(r && (r->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0);
  CT_CHECK(r && r->attachment_count == 1);
  CT_CHECK(r && strcmp(r->attachments[0].filename, "voice-message.ogg") == 0);
  CT_CHECK(r && strcmp(r->attachments[0].content_type, "audio/ogg") == 0);
  CT_CHECK(r && r->attachments[0].duration_ms == 3250);
  CT_CHECK(r && strcmp(r->attachments[0].waveform_b64, waveform) == 0);
  CT_CHECK(r && ccord_attachment_is_audio(&r->attachments[0]));
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);

  char url[512];
  (void)snprintf(url, sizeof url, "%s", r->attachments[0].url);
  CT_CHECK(ccord_transport_fetch_media(t, url, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
  CT_CHECK(ev.media_url != nullptr && strcmp(ev.media_url, url) == 0);
  CT_CHECK(ev.media_data != nullptr && ev.media_len == sizeof ogg && memcmp(ev.media_data, ogg, sizeof ogg) == 0);

  // A later, ordinary upload still has no voice flag and its bytes are served back too.
  const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  ccord_upload_file plain = {.filename = "shot.png", .data = png, .size = sizeof png};
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, false, "", 0, &plain, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && (ev.messages[0].rich->flags & CCORD_MSGFLAG_VOICE_MESSAGE) == 0);
  (void)snprintf(url, sizeof url, "%s", ev.messages[0].rich->attachments[0].url);
  CT_CHECK(ccord_transport_fetch_media(t, url, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
  CT_CHECK(ev.media_data != nullptr && ev.media_len == sizeof png && memcmp(ev.media_data, png, sizeof png) == 0);

  // The mock URL only carries the filename, so re-uploading the same voice name must serve the newest bytes.
  static const uint8_t ogg2[] = {'O', 'g', 'g', 'S', 0x11, 0x22, 0x33};
  ccord_upload_file voice2 = {
    .filename = "voice-message.ogg", .data = ogg2, .size = sizeof ogg2, .duration_ms = 1000, .waveform_b64 = waveform,
  };
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, false, "", 0, &voice2, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  (void)snprintf(url, sizeof url, "%s", ev.messages[0].rich->attachments[0].url);
  CT_CHECK(ccord_transport_fetch_media(t, url, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MEDIA_LOADED, &ev));
  CT_CHECK(ev.media_data != nullptr && ev.media_len == sizeof ogg2 && memcmp(ev.media_data, ogg2, sizeof ogg2) == 0);

  ccord_transport_destroy(t);
}

static void test_mock_polls_components_and_modals(void)
{
  ccord_error err;
  g_now = 40000;
  ccord_mock_config cfg = {.seed = 23, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, 1, nullptr, 0, &err) == CCORD_ERR_INVALID); // not connected
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Single-select poll: vote, switch answer (removal before addition), unvote, validation errors.
  ccord_message_rich r_poll = {
    .present = CCORD_RICH_ALL,
    .poll = {
      .present = true,
      .question = "Best allocator?",
      .answer_count = 3,
      .answers = {
        {.id = 1, .text = "Arena", .votes = 2},
        {.id = 2, .text = "jemalloc", .votes = 5},
        {.id = 3, .text = "System", .votes = 1},
      },
      .multiselect = false,
      .finalized = false,
    },
  };
  ccord_id poll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &r_poll, &poll_mid, &err) == CCORD_OK && poll_mid != 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].id == poll_mid && ev.messages[0].rich != nullptr);

  uint32_t a1 = 1;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &a1, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.channel_id == 3111 && ev.message_id == poll_mid && ev.user_id == 1001 &&
           ev.poll_answer_id == 1 && ev.poll_vote_added);

  // Switch vote from 1 -> 2: mock emits REMOVE(1) followed by ADD(2).
  uint32_t a2 = 2;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &a2, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.poll_answer_id == 1 && !ev.poll_vote_added);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.poll_answer_id == 2 && ev.poll_vote_added);

  // Unvote (0 answers) emits REMOVE(2).
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev));
  CT_CHECK(ev.poll_answer_id == 2 && !ev.poll_vote_added);

  // Reject multi-answer on single-select, unknown answer, duplicate answer, unknown message.
  uint32_t multi[2] = {1, 2};
  uint32_t dup[2] = {1, 1};
  uint32_t unk = 99;
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, multi, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &unk, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_vote_poll(t, 999999, poll_mid, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, 999999, &a1, 1, &err) == CCORD_ERR_NOT_FOUND);

  // Multiselect poll accepts multiple answers, rejects duplicate answer IDs.
  r_poll.poll.multiselect = true;
  ccord_id mpoll_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &r_poll, &mpoll_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ccord_transport_vote_poll(t, 3111, mpoll_mid, dup, 2, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, mpoll_mid, multi, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev) && ev.poll_answer_id == 1 && ev.poll_vote_added);
  CT_CHECK(poll_for(t, CCORD_NET_POLL_VOTE, &ev) && ev.poll_answer_id == 2 && ev.poll_vote_added);

  // Finalized poll rejects votes.
  r_poll.poll.finalized = true;
  ccord_id closed_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &r_poll, &closed_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ccord_transport_vote_poll(t, 3111, closed_mid, &a1, 1, &err) == CCORD_ERR_INVALID);

  // 2. Message components + modal interactions.
  ccord_component row_items[2] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Deploy", .custom_id = "deploy:prod"},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_SECONDARY, .label = "Feedback", .custom_id = "modal:feedback"},
  };
  ccord_component_row row = {.components = row_items, .component_count = 2};
  ccord_message_rich r_comp = {
    .present = CCORD_RICH_ALL,
    .application_id = 777001,
    .components = &row,
    .component_row_count = 1,
  };
  ccord_id comp_mid = 0;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "Controls", &r_comp, &comp_mid, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));

  ccord_interaction_request ireq = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 777001,
    .guild_id = 2001,
    .channel_id = 3111,
    .message_id = comp_mid,
    .component_type = CCORD_COMP_BUTTON,
    .custom_id = "deploy:prod",
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_interaction_count(t) == 1);
  CT_CHECK(ccord_mock_last_interaction_type(t) == CCORD_INTERACTION_MESSAGE_COMPONENT);
  CT_CHECK(ccord_mock_last_interaction_component_type(t) == CCORD_COMP_BUTTON);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(t), "deploy:prod") == 0);
  CT_CHECK(ccord_mock_last_interaction_value_count(t) == 0);

  // String select values recorded in inspection hooks.
  const char *sel_vals[2] = {"linux", "bsd"};
  ireq.component_type = CCORD_COMP_STRING_SELECT;
  ireq.custom_id = "pick_os";
  ireq.values = sel_vals;
  ireq.value_count = 2;
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_interaction_count(t) == 2 && ccord_mock_last_interaction_value_count(t) == 2);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 0), "linux") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 1), "bsd") == 0);

  // "modal:feedback" triggers CCORD_NET_INTERACTION_MODAL, and MODAL_SUBMIT posts an echo message.
  ireq.component_type = CCORD_COMP_BUTTON;
  ireq.custom_id = "modal:feedback";
  ireq.values = nullptr;
  ireq.value_count = 0;
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_INTERACTION_MODAL, &ev));
  CT_CHECK(ev.modal != nullptr && strcmp(ev.modal->custom_id, "feedback") == 0 && ev.modal->field_count == 2);

  const char *fids[2] = {"subject", "details"};
  const char *fvals[2] = {"Smooth render", "60fps locked"};
  ccord_interaction_request mreq = {
    .type = CCORD_INTERACTION_MODAL_SUBMIT,
    .application_id = 777001,
    .guild_id = 2001,
    .channel_id = 3111,
    .custom_id = "feedback",
    .field_ids = fids,
    .field_values = fvals,
    .field_count = 2,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &mreq, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_last_interaction_type(t) == CCORD_INTERACTION_MODAL_SUBMIT);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 0), "Smooth render") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(t, 1), "60fps locked") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(strcmp(ev.messages[0].content, "Modal submitted: Smooth render") == 0);

  // Refused writes surface CCORD_NET_ACTION_FAILED for both poll votes and interactions.
  ccord_mock_set_refuse_writes(t, true);
  CT_CHECK(ccord_transport_vote_poll(t, 3111, poll_mid, &a1, 1, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_POLL_VOTE);
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_INTERACTION);
  ccord_mock_set_refuse_writes(t, false);

  // 3. Slash commands index, /ping ephemeral (not persisted in history), /echo public, /topic autocomplete.
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_SLASH_INDEX) == 0);
  CT_CHECK(ccord_transport_request_slash_commands(t, 2001, 3111, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_SLASH_INDEX) == 1);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_COMMANDS, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.channel_id == 3111 && ev.slash_command_count == 7);

  ccord_interaction_request ping_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9101, .command_name = "ping", .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ping_req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_id ephem_id = ev.messages[0].id;
  CT_CHECK(ccord_message_is_ephemeral(&ev.messages[0]));
  // Ephemeral message is NOT persisted in server channel history.
  CT_CHECK(ccord_transport_request_history(t, 3111, 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev));
  for (size_t i = 0; i < ev.message_count; i++) {
    CT_CHECK(ev.messages[i].id != ephem_id);
  }

  const char *ac_name = "subject", *ac_val = "sdl";
  uint8_t ac_kind = CCORD_SLASH_OPT_STRING;
  ccord_interaction_request ac_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = 1011, .guild_id = 2001, .channel_id = 3111,
    .command_id = 9103, .command_name = "topic",
    .option_names = &ac_name, .option_kinds = &ac_kind, .option_values = &ac_val, .option_count = 1,
    .focused_option = 0,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ac_req, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SLASH_AUTOCOMPLETE, &ev));
  CT_CHECK(ev.slash_choice_count == 1 && strcmp(ev.slash_choices[0].value, "sdl3") == 0);

  // 4. send_reply_ex mention_author true vs false, multi-file upload with spoiler, and slowmode enforcement.
  CT_CHECK(ccord_transport_request_history(t, 3114, 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count > 0);
  ccord_id other_mid = 0;
  for (size_t i = 0; i < ev.message_count; i++) {
    if (ev.messages[i].author_id != 1001) { other_mid = ev.messages[i].id; break; }
  }
  CT_CHECK(other_mid != 0);
  CT_CHECK(ccord_transport_send_reply_ex(t, 3114, other_mid, true, "ping reply", 10, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->mention_user_count == 1);
  // 3114 has 5 s slowmode, but self is Admin in guild 2001 -> immediate second send succeeds!
  CT_CHECK(ccord_transport_send_reply_ex(t, 3114, other_mid, false, "silent reply", 12, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && ev.messages[0].rich &&
           ev.messages[0].rich->mention_user_count == 0);

  const uint8_t png[4] = {0x89, 0x50, 0x4E, 0x47};
  ccord_upload_file ufiles[2] = {
    {.filename = "shot1.png", .data = png, .size = sizeof png, .spoiler = false},
    {.filename = "secret.png", .data = png, .size = sizeof png, .spoiler = true},
  };
  CT_CHECK(ccord_transport_send_message_with_files(t, 3111, CCORD_ID_NONE, true, "pics", 4, ufiles, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->attachment_count == 2 &&
           !ev.messages[0].rich->attachments[0].spoiler && ev.messages[0].rich->attachments[1].spoiler);

  // In channel 3315 (#off-topic in Pixel Garden, 5 s slowmode, self is plain Member), immediate second send fails.
  g_now = 100000;
  ccord_transport_destroy(t);
  ccord_mock_config scfg = {.now_ms = fake_now};
  t = ccord_mock_transport_create(&scfg, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ccord_transport_send_message(t, 3315, "first", 5, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  g_now += 2000;
  CT_CHECK(ccord_transport_send_message(t, 3315, "too fast", 8, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.action == CCORD_ACTION_SEND &&
           ev.draft && strcmp(ev.draft, "too fast") == 0);
  g_now += 3000; // total 5000 ms elapsed -> allowed again
  CT_CHECK(ccord_transport_send_message(t, 3315, "allowed now", 11, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && strcmp(ev.messages[0].content, "allowed now") == 0);

  ccord_transport_destroy(t);
}

static void test_mock_search_pins_reactions_and_threads(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.big_channel_messages = 120};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));

  // 1. Structured search with pagination + operators across guild channels.
  ccord_search_query sq;
  ccord_search_parse("in", 2, &sq);
  sq.offset = 0;
  CT_CHECK(ccord_transport_search_messages_ex(t, 2001, CCORD_ID_NONE, &sq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SEARCH_RESULTS, &ev));
  CT_CHECK(ev.search_total_results > CCORD_SEARCH_PAGE_SIZE && ev.message_count == CCORD_SEARCH_PAGE_SIZE &&
           ev.search_offset == 0);
  ccord_id p0_first = ev.messages[0].id;

  sq.offset = CCORD_SEARCH_PAGE_SIZE;
  CT_CHECK(ccord_transport_search_messages_ex(t, 2001, CCORD_ID_NONE, &sq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SEARCH_RESULTS, &ev));
  CT_CHECK(ev.search_offset == CCORD_SEARCH_PAGE_SIZE && ev.message_count > 0 && ev.messages[0].id != p0_first);

  // in:dev narrows guild search to #dev (3113).
  CT_CHECK(ccord_transport_send_message(t, 3113, "unique-dev-token", 16, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));
  ccord_search_parse("in:dev unique-dev-token", 23, &sq);
  CT_CHECK(ccord_transport_search_messages_ex(t, 2001, CCORD_ID_NONE, &sq, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_SEARCH_RESULTS, &ev));
  CT_CHECK(ev.search_total_results == 1 && ev.message_count == 1 && ev.messages[0].channel_id == 3113);

  // 2. Paginated pins via request_pins_before.
  CT_CHECK(ccord_transport_request_history(t, 3111, 10, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_HISTORY, &ev) && ev.message_count >= 4);
  ccord_id m0 = ev.messages[0].id, m1 = ev.messages[1].id, m2 = ev.messages[2].id;
  CT_CHECK(ccord_transport_toggle_pin(t, 3111, m0, true, &err) == CCORD_OK && poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));
  CT_CHECK(ccord_transport_toggle_pin(t, 3111, m1, true, &err) == CCORD_OK && poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));
  CT_CHECK(ccord_transport_toggle_pin(t, 3111, m2, true, &err) == CCORD_OK && poll_for(t, CCORD_NET_MESSAGE_PIN_UPDATE, &ev));

  CT_CHECK(ccord_transport_request_pins_before(t, 3111, nullptr, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_PIN_LIST, &ev));
  CT_CHECK(!ev.pins_append && ev.message_count == 2 && ev.pins_has_more && ev.pins_next_cursor && ev.pins_next_cursor[0] != '\0');
  char cur_copy[64];
  snprintf(cur_copy, sizeof cur_copy, "%s", ev.pins_next_cursor);

  CT_CHECK(ccord_transport_request_pins_before(t, 3111, cur_copy, 2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_PIN_LIST, &ev));
  CT_CHECK(ev.pins_append && ev.message_count >= 1);

  // 3. Reaction users endpoint.
  CT_CHECK(ccord_transport_toggle_reaction(t, 3111, m0, "\xF0\x9F\x94\xA5", 4, true, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_REACTION_UPDATE, &ev));
  CT_CHECK(ccord_transport_request_reaction_users(t, 3111, m0, "\xF0\x9F\x94\xA5", 4, 50, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_REACTION_USERS) == 1);
  CT_CHECK(poll_for(t, CCORD_NET_REACTION_USERS, &ev));
  CT_CHECK(ev.channel_id == 3111 && ev.message_id == m0 && ev.user_count >= 1);

  // 4. Thread from message + join/leave/archive.
  ccord_admin_request req;
  CT_CHECK(ccord_admin_create_thread_from_message(&req, 2001, 3111, m0, "Issue Thread", &err) == CCORD_OK);
  CT_CHECK(admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  ccord_id tid = ev.channel->id;
  CT_CHECK(tid != CCORD_ID_NONE && ev.channel->parent_id == 3111 && ev.channel->thread_joined && !ev.channel->thread_archived);

  CT_CHECK(ccord_admin_leave_thread(&req, 2001, tid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_THREAD_MEMBER_SELF, &ev) && ev.channel_id == tid && !ev.thread_joined);

  CT_CHECK(ccord_admin_join_thread(&req, 2001, tid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_THREAD_MEMBER_SELF, &ev) && ev.channel_id == tid && ev.thread_joined);

  CT_CHECK(ccord_admin_archive_thread(&req, 2001, tid, true, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == tid && ev.channel->thread_archived);

  ccord_transport_destroy(t);
}

static void test_mock_m3b_folders_reorder_invites_and_discovery(void)
{
  ccord_error err;
  g_now = 60000;
  ccord_mock_config cfg = {.seed = 31, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Guild folders reorder & metadata.
  ccord_admin_request req;
  ccord_guild_folder_item fitems[2] = {
    {.guild_id = 2003, .folder_id = 700, .folder_name = "Art", .folder_color = 0x57F287},
    {.guild_id = 2001, .folder_id = 700, .folder_name = "Art", .folder_color = 0x57F287},
  };
  CT_CHECK(ccord_admin_set_guild_folders(&req, fitems, 2, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_LAYOUT, &ev));
  CT_CHECK(ev.folder_item_count == 2 && ev.folder_items[0].guild_id == 2003 && ev.folder_items[0].folder_id == 700 &&
           strcmp(ev.folder_items[0].folder_name, "Art") == 0);

  // 2. Channel reorder & permission/parent validation.
  ccord_id cids[2] = {3112, 3111};
  int32_t cpos[2] = {0, 1};
  ccord_id cpars[2] = {3110, 3110};
  uint8_t chas[2] = {1, 1};
  CT_CHECK(ccord_admin_reorder_channels(&req, 2001, cids, cpos, cpars, chas, 2, "swap", &err) == CCORD_OK);
  CT_CHECK(admin_call(t, &req) && poll_for(t, CCORD_NET_CHANNEL_POSITIONS, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.reorder_count == 2 && ev.reorder_ids[0] == 3112 && ev.reorder_positions[1] == 1);

  cpars[0] = 3111; // non-category parent -> invalid
  CT_CHECK(ccord_admin_reorder_channels(&req, 2001, cids, cpos, cpars, chas, 2, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_REORDER_CHANNELS);

  // 3. Create guild, preview invite, accept invite, unknown invite failure.
  CT_CHECK(ccord_admin_create_guild(&req, "C23 Forge", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev));
  CT_CHECK(ev.guild && strcmp(ev.guild->name, "C23 Forge") == 0 && ev.admin_op == CCORD_ADMIN_CREATE_GUILD);

  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/vulkan-lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_PREVIEW, &ev));
  CT_CHECK(ev.invite_preview && ev.invite_preview->guild_id == 9004 &&
           strcmp(ev.invite_preview->guild_name, "Vulkan & Graphics Lab") == 0);

  CT_CHECK(ccord_admin_accept_invite(&req, "vulkan-lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild && ev.guild->id == 9004);
  CT_CHECK(poll_for(t, CCORD_NET_INVITE_ACCEPTED, &ev) && ev.guild_id == 9004 && ev.channel_id != CCORD_ID_NONE);

  CT_CHECK(ccord_admin_preview_invite(&req, "nope-unknown", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_PREVIEW_INVITE);

  // 4. Discoverable guilds filtering by category and case-insensitive substring query.
  CT_CHECK(ccord_transport_request_discoverable_guilds(t, "", 0, 0, 24, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_DISCOVERY) == 1);
  CT_CHECK(poll_for(t, CCORD_NET_DISCOVERY_RESULTS, &ev) && ev.discovery_total >= 5 && ev.discovery_count >= 5);

  CT_CHECK(ccord_transport_request_discoverable_guilds(t, "C23", 3, 0, 24, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_DISCOVERY_RESULTS, &ev));
  CT_CHECK(ev.discovery_total == 1 && ev.discovery_count == 1 && ev.discovery_guilds[0].guild_id == 9003);

  ccord_transport_destroy(t);
}

static void test_mock_m3c_group_dms_requests_profiles_and_status(void)
{
  ccord_error err;
  g_now = 70000;
  ccord_mock_config cfg = {.seed = 37, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Group DM create (default recipient-list name + custom name), add/remove recipients, edit name/icon, leave.
  ccord_admin_request req;
  ccord_id r2[2] = {1002, 1003};
  CT_CHECK(ccord_admin_create_group_dm(&req, r2, 2, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  ccord_id gdm_id = ev.channel->id;
  CT_CHECK(ev.channel->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM && ev.channel->owner_id == 1001 &&
           ev.channel->recipient_count == 2 && strstr(ev.channel->name, "Ada") != nullptr);

  CT_CHECK(ccord_admin_add_dm_recipient(&req, gdm_id, 1004, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_DM_RECIPIENT_CHANGE, &ev));
  CT_CHECK(ev.channel_id == gdm_id && ev.user_id == 1004 && ev.recipient_added && ev.user_count == 1);

  CT_CHECK(ccord_admin_remove_dm_recipient(&req, gdm_id, 1003, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_DM_RECIPIENT_CHANGE, &ev));
  CT_CHECK(ev.channel_id == gdm_id && ev.user_id == 1003 && !ev.recipient_added);

  ccord_admin_group_dm_edit gedit = {.name = "Kernel Crew", .icon_data_uri = "data:image/png;base64,AAAA"};
  CT_CHECK(ccord_admin_edit_group_dm(&req, gdm_id, &gedit, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  CT_CHECK(strcmp(ev.channel->name, "Kernel Crew") == 0 && ev.channel->icon_url && ev.channel->icon_url[0] != '\0');

  ccord_admin_group_dm_edit gclear = {.name = "", .icon_data_uri = ""};
  CT_CHECK(ccord_admin_edit_group_dm(&req, gdm_id, &gclear, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel != nullptr);
  CT_CHECK(strstr(ev.channel->name, "Ada") != nullptr && ev.channel->icon_url[0] == '\0');

  // Removing self leaves the Group DM (CHANNEL_DELETE).
  CT_CHECK(ccord_admin_remove_dm_recipient(&req, gdm_id, 1001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_DELETE, &ev) && ev.channel_id == gdm_id);

  // 2. Message Requests (regular + spam), accept, reject.
  ccord_id req_cid = 0, spam_cid = 0;
  CT_CHECK(ccord_mock_push_message_request(t, 1008, "hello there!", false, &req_cid, &err) == CCORD_OK && req_cid != 0);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->is_message_request && !ev.channel->is_spam);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev) && strcmp(ev.messages[0].content, "hello there!") == 0);

  CT_CHECK(ccord_mock_push_message_request(t, 1009, "free nitro scam", true, &spam_cid, &err) == CCORD_OK && spam_cid != 0);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->is_message_request && ev.channel->is_spam);
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_CREATE, &ev));

  CT_CHECK(ccord_admin_accept_message_request(&req, req_cid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_MESSAGE_REQUEST_ACCEPTED, &ev) && ev.channel_id == req_cid);

  CT_CHECK(ccord_admin_reject_message_request(&req, spam_cid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_DELETE, &ev) && ev.channel_id == spam_cid);

  // 3. User note update & clear, custom status set & clear, presence_ex.
  CT_CHECK(ccord_admin_set_user_note(&req, 1002, "Met at SystemsConf", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_USER_NOTE_UPDATE, &ev) && ev.user_id == 1002 && strcmp(ev.text, "Met at SystemsConf") == 0);

  CT_CHECK(ccord_admin_set_user_note(&req, 1002, "", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_USER_NOTE_UPDATE, &ev) && ev.user_id == 1002 && ev.text[0] == '\0');

  CT_CHECK(ccord_admin_set_custom_status(&req, "\xF0\x9F\x9A\x80", 0, "Shipping M3c", 1900000000000LL, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CUSTOM_STATUS_UPDATE, &ev) && !ev.custom_status_clear &&
           strcmp(ev.custom_status_text, "Shipping M3c") == 0 && ev.custom_status_expires_ms == 1900000000000LL);

  CT_CHECK(ccord_transport_set_presence_ex(t, CCORD_PRESENCE_DND, "\xF0\x9F\x9A\x80", 0, "Shipping M3c", &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_PRESENCE_UPDATE, &ev) && ev.user_id == 1001 && ev.presence == CCORD_PRESENCE_DND &&
           strcmp(ev.custom_status_text, "Shipping M3c") == 0);

  CT_CHECK(ccord_admin_set_custom_status(&req, "", 0, "", 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CUSTOM_STATUS_UPDATE, &ev) && ev.custom_status_clear);

  // 4. Full user profile via request_profile_ex (connections, mutual guilds, mutual friends, clan tag, call count).
  size_t pc0 = ccord_mock_call_count(t, CCORD_MOCK_CALL_PROFILE);
  CT_CHECK(ccord_transport_request_profile_ex(t, 1002, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_call_count(t, CCORD_MOCK_CALL_PROFILE) == pc0 + 1);
  CT_CHECK(poll_for(t, CCORD_NET_USER_PROFILE, &ev) && ev.user_profile != nullptr);
  CT_CHECK(ev.user_profile->user_id == 1002 && ev.user_profile->connection_count >= 2 &&
           ev.user_profile->mutual_guild_count >= 1 && ev.user_profile->mutual_friend_count >= 1 &&
           ev.user_profile->clan_tag[0] != '\0');

  ccord_transport_destroy(t);
}

static void test_mock_m4a_guild_settings_safety_vanity_audit_and_danger(void)
{
  ccord_error err;
  g_now = 80000;
  ccord_mock_config cfg = {.seed = 41, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  // 1. Extended CCORD_ADMIN_EDIT_GUILD: icon & banner upload/clear, system/AFK channels, flags, notify, timeout, safety.
  ccord_admin_request req;
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.name = "C-Cord HQ Prime";
  ge.icon_data_uri = "data:image/png;base64,AAAA";
  ge.banner_data_uri = "data:image/png;base64,BBBB";
  ge.has_system_channel = true;
  ge.system_channel_id = 3112;
  ge.system_channel_flags =
      (int32_t)(CCORD_SYSCHAN_SUPPRESS_JOIN_NOTIFICATIONS | CCORD_SYSCHAN_SUPPRESS_GUILD_REMINDER_NOTIFICATIONS);
  ge.default_notifications = 1;
  ge.has_afk_channel = true;
  ge.afk_channel_id = 3121;
  ge.afk_timeout = 1800;
  ge.verification_level = 3;
  ge.explicit_content_filter = 2;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, "Full config", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  CT_CHECK(strcmp(ev.guild->name, "C-Cord HQ Prime") == 0 &&
           ev.guild->icon_url && strstr(ev.guild->icon_url, "/icons/2001/") != nullptr &&
           strstr(ev.guild->banner_url, "/banners/2001/") != nullptr &&
           ev.guild->system_channel_id == 3112 && ev.guild->system_channel_flags == 5 &&
           ev.guild->default_message_notifications == 1 &&
           ev.guild->afk_channel_id == 3121 && ev.guild->afk_timeout == 1800 &&
           ev.guild->verification_level == 3 && ev.guild->explicit_content_filter == 2);

  // Clear icon, banner, system channel, and AFK channel.
  ccord_admin_guild_edit gclr = CCORD_ADMIN_GUILD_EDIT_INIT;
  gclr.icon_data_uri = "";
  gclr.banner_data_uri = "";
  gclr.has_system_channel = true;
  gclr.system_channel_id = CCORD_ID_NONE;
  gclr.has_afk_channel = true;
  gclr.afk_channel_id = CCORD_ID_NONE;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &gclr, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  CT_CHECK(ev.guild->icon_url[0] == '\0' && ev.guild->banner_url[0] == '\0' &&
           ev.guild->system_channel_id == CCORD_ID_NONE && ev.guild->afk_channel_id == CCORD_ID_NONE);

  // Invalid system channel (voice 3121) or invalid AFK channel (text 3111) refused.
  ccord_admin_guild_edit gbad = CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.has_system_channel = true;
  gbad.system_channel_id = 3121;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &gbad, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EDIT_GUILD);

  gbad = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  gbad.has_afk_channel = true;
  gbad.afk_channel_id = 3111;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &gbad, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_EDIT_GUILD);

  // 2. Incident actions & Vanity URL (2001 has tier 3 -> allowed; 2002 lacks MANAGE_GUILD / tier -> refused).
  CT_CHECK(ccord_admin_set_incident_actions(&req, 2001, 1800000000000LL, 1800010000000LL, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_INCIDENTS, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.invites_disabled_until_ms == 1800000000000LL &&
           ev.dms_disabled_until_ms == 1800010000000LL);

  CT_CHECK(ccord_admin_set_vanity_url(&req, 2001, "ccord-prime", "Rebrand", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_VANITY, &ev));
  CT_CHECK(ev.guild_id == 2001 && ev.invite_code && strcmp(ev.invite_code, "ccord-prime") == 0);

  CT_CHECK(ccord_admin_set_vanity_url(&req, 2002, "lua-lounge", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_SET_VANITY_URL);

  // 3. Audit log filtering by user_id and action_type + structured changes[] emitted on guild edit (newest first).
  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 1001, 1, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev));
  CT_CHECK(ev.audit_count >= 3 && ev.audit[0].user_id == 1001 && ev.audit[0].action == 1 &&
           ev.audit[0].change_count >= 1 && ev.audit[2].change_count >= 2);

  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 999999, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count == 0);

  // 4. Leave Server (non-owner succeeds on 2003; owner refused on newly created guild), Transfer Ownership, Delete Server.
  CT_CHECK(ccord_admin_leave_guild(&req, 2003, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_DELETE, &ev) && ev.guild_id == 2003 && ev.admin_op == CCORD_ADMIN_LEAVE_GUILD);

  // Non-owner cannot transfer or delete 2002 (owned by 1002).
  CT_CHECK(ccord_admin_transfer_ownership(&req, 2002, 1003, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_TRANSFER_OWNERSHIP);
  CT_CHECK(ccord_admin_delete_guild(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_DELETE_GUILD);

  // Create an owned server (owner_id == 1001): owner cannot leave, CAN transfer or delete!
  CT_CHECK(ccord_admin_create_guild(&req, "Owned Lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  ccord_id owned_gid = ev.guild->id;
  while (ccord_transport_poll(t, &ev)) {
  }

  CT_CHECK(ccord_admin_leave_guild(&req, owned_gid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LEAVE_GUILD);

  CT_CHECK(ccord_admin_delete_guild(&req, owned_gid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_DELETE, &ev) && ev.guild_id == owned_gid && ev.admin_op == CCORD_ADMIN_DELETE_GUILD);

  ccord_transport_destroy(t);
}

static void test_mock_m4b_expressions(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  // SNAPSHOT strips uploader user_id/user_name just like Discord's READY payload!
  CT_CHECK(ev.emoji_count > 0 && ev.sticker_count > 0 && ev.soundboard_count > 0);
  for (size_t i = 0; i < ev.emoji_count; i++) CT_CHECK(ev.emojis[i].user_id == CCORD_ID_NONE);
  for (size_t i = 0; i < ev.sticker_count; i++) CT_CHECK(ev.stickers[i].user_id == CCORD_ID_NONE);
  for (size_t i = 0; i < ev.soundboard_count; i++) CT_CHECK(ev.soundboard[i].user_id == CCORD_ID_NONE);
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  // Admin list endpoints DO populate uploader user_id + user_name and set expressions_admin.
  CT_CHECK(ccord_admin_list_emojis(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_EXPRESSIONS, &ev) && ev.expressions_admin && ev.emoji_count >= 2 &&
           ev.emojis[0].user_id != CCORD_ID_NONE);

  CT_CHECK(ccord_admin_list_stickers(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_EXPRESSIONS, &ev) && ev.expressions_admin && ev.sticker_count >= 2 &&
           ev.stickers[0].user_id != CCORD_ID_NONE);

  CT_CHECK(ccord_admin_list_soundboard(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_EXPRESSIONS, &ev) && ev.expressions_admin && ev.soundboard_count >= 2 &&
           ev.soundboard[0].user_id != CCORD_ID_NONE);

  // Full CRUD + audit log actions 60/61/62, 90/91/92, 130/131/132 on guild 2001.
  CT_CHECK(ccord_admin_create_emoji(&req, 2001, "c23_fast", "data:image/png;base64,iVBORw==", "speed", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_EMOJI_UPSERT, &ev) && ev.emoji_count == 1 && ev.emojis[0].user_id == 1001);
  ccord_id em_id = ev.emojis[0].id;
  CT_CHECK(ccord_admin_edit_emoji(&req, 2001, em_id, "c23_turbo", nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_EMOJI_UPSERT, &ev) && strcmp(ev.emojis[0].name, "c23_turbo") == 0);
  CT_CHECK(ccord_admin_delete_emoji(&req, 2001, em_id, "done", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_EMOJI_DELETE, &ev) && ev.expr_id == em_id);

  const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  CT_CHECK(ccord_admin_create_sticker(&req, 2001, "Turbo Cat", "Zoom", "cat", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STICKER_UPSERT, &ev) && ev.sticker_count == 1 && ev.stickers[0].user_id == 1001);
  ccord_id st_id = ev.stickers[0].id;
  ccord_admin_sticker_edit se = {.name = "Hyper Cat", .description = "Fast", .tags = "zoom"};
  CT_CHECK(ccord_admin_edit_sticker(&req, 2001, st_id, &se, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STICKER_UPSERT, &ev) && strcmp(ev.stickers[0].name, "Hyper Cat") == 0);
  CT_CHECK(ccord_admin_delete_sticker(&req, 2001, st_id, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_STICKER_DELETE, &ev) && ev.expr_id == st_id);

  CT_CHECK(ccord_admin_create_soundboard(&req, 2001, "Tada", "data:audio/ogg;base64,T2dnUw==", 50,
                                         CCORD_ID_NONE, "🎉", nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SOUNDBOARD_UPSERT, &ev) && ev.soundboard_count == 1 &&
           ev.soundboard[0].volume_pct == 50 && ev.soundboard[0].user_id == 1001);
  ccord_id sb_id = ev.soundboard[0].id;
  ccord_admin_soundboard_edit sbe = CCORD_ADMIN_SOUNDBOARD_EDIT_INIT;
  sbe.name = "Big Tada";
  sbe.volume_pct = 100;
  CT_CHECK(ccord_admin_edit_soundboard(&req, 2001, sb_id, &sbe, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SOUNDBOARD_UPSERT, &ev) && ev.soundboard[0].volume_pct == 100);
  CT_CHECK(ccord_admin_delete_soundboard(&req, 2001, sb_id, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_SOUNDBOARD_DELETE, &ev) && ev.expr_id == sb_id);

  // Verify audit log entries recorded 132, 131, 130, 92, 91, 90, 62, 61, 60 (newest first).
  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 1001, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count >= 9);
  CT_CHECK(ev.audit[0].action == 132 && ev.audit[1].action == 131 && ev.audit[2].action == 130 &&
           ev.audit[3].action == 92 && ev.audit[4].action == 91 && ev.audit[5].action == 90 &&
           ev.audit[6].action == 62 && ev.audit[7].action == 61 && ev.audit[8].action == 60);

  // Permission refusal on guild 2002 (Lua Lounge: self has neither CREATE_EXPRESSIONS nor MANAGE_EXPRESSIONS).
  CT_CHECK(ccord_admin_list_emojis(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_EMOJIS &&
           ev.status == CCORD_ERR_PERMISSION);

  // Slot limit enforcement on tier-0 guild 2003: grant self MANAGE_EXPRESSIONS on a fresh tier-0 guild (owned_gid).
  CT_CHECK(ccord_admin_create_guild(&req, "Tier0 Expr", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild != nullptr);
  ccord_id g0 = ev.guild->id;
  while (ccord_transport_poll(t, &ev)) {
  }
  // Tier 0 allows 5 stickers: 5 succeed, 6th fails with slot limit!
  for (int i = 0; i < 5; i++) {
    char snm[16];
    snprintf(snm, sizeof snm, "St%d", i);
    CT_CHECK(ccord_admin_create_sticker(&req, g0, snm, "", "tag", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
             admin_call(t, &req));
    CT_CHECK(poll_for(t, CCORD_NET_STICKER_UPSERT, &ev));
  }
  CT_CHECK(ccord_admin_create_sticker(&req, g0, "StOverflow", "", "tag", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_CREATE_STICKER);

  ccord_transport_destroy(t);
}

static void test_mock_m4c_prune_and_automod(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  CT_CHECK(ev.snapshot && ev.snapshot->member_count > 0);
  for (size_t i = 0; i < ev.snapshot->member_count; i++) CT_CHECK(ev.snapshot->members[i].joined_at_ms > 0);
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  // 1. Prune preview on HQ (2001): 30d catches 1009 (35d inactive, roleless); 7d catches 1009 + 1010 (10d).
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 30, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.guild_id == 2001 && ev.prune_info.has_result &&
           !ev.prune_info.executed && ev.prune_info.days == 30 && ev.prune_info.pruned == 1);

  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.prune_info.pruned == 2);

  // Execute 30d prune -> removes 1009 and emits PRUNE_RESULT(executed=true, pruned=1) + audit action 21:
  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 30, true, nullptr, 0, "30d sweep", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_MEMBER_REMOVE, &ev) && ev.guild_id == 2001 && ev.user_id == 1009);
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.prune_info.executed && ev.prune_info.pruned == 1);

  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_PRUNE_RESULT, &ev) && ev.prune_info.pruned == 1);

  // Ordinary member on guild 2003 cannot preview prune:
  CT_CHECK(ccord_admin_preview_prune(&req, 2003, 7, nullptr, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_PREVIEW_PRUNE &&
           ev.status == CCORD_ERR_PERMISSION);

  // 2. AutoMod CRUD on HQ (2001) + audit actions 140/141/142 + permission refusal on 2002 (no MANAGE_GUILD):
  CT_CHECK(ccord_admin_list_automod_rules(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_LIST, &ev) && ev.guild_id == 2001 && ev.automod_rule_count == 2 &&
           ev.automod_rules[0].id == 91001 && ev.automod_rules[1].id == 91002);

  const char *kws[1] = {"phish.example"};
  ccord_automod_rule nr = {
    .name = "Block Phish",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
    .enabled = true,
    .keywords = kws, .keyword_count = 1,
    .action_block = true, .custom_message = "No phishing",
  };
  CT_CHECK(ccord_admin_create_automod_rule(&req, 2001, &nr, "new rule", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_UPSERT, &ev) && ev.automod_rule_count == 1 &&
           ev.automod_rules[0].creator_id == 1001 && strcmp(ev.automod_rules[0].name, "Block Phish") == 0);
  ccord_id rid = ev.automod_rules[0].id;

  nr.name = "Block Phish Updated";
  nr.enabled = false;
  CT_CHECK(ccord_admin_edit_automod_rule(&req, 2001, rid, &nr, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_UPSERT, &ev) && !ev.automod_rules[0].enabled &&
           strcmp(ev.automod_rules[0].name, "Block Phish Updated") == 0);

  CT_CHECK(ccord_admin_delete_automod_rule(&req, 2001, rid, "retired", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUTOMOD_DELETE, &ev) && ev.guild_id == 2001 && ev.expr_id == rid);

  CT_CHECK(ccord_admin_list_audit_log(&req, 2001, 1001, 0, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_AUDIT_LOG, &ev) && ev.audit_count >= 4);
  CT_CHECK(ev.audit[0].action == 142 && ev.audit[1].action == 141 && ev.audit[2].action == 140 &&
           ev.audit[3].action == 21);

  CT_CHECK(ccord_admin_list_automod_rules(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_AUTOMOD_RULES &&
           ev.status == CCORD_ERR_PERMISSION);

  ccord_transport_destroy(t);
}

static void test_mock_m4c_integrations_and_community(void)
{
  ccord_error err;
  ccord_transport *t = ccord_mock_transport_create(nullptr, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;
  // 1. Webhooks CRUD + audit 50/51/52 + permission refusal on 2003:
  CT_CHECK(ccord_admin_list_webhooks(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_LIST, &ev) && ev.guild_id == 2001 && ev.webhook_count == 1);

  CT_CHECK(ccord_admin_create_webhook(&req, 2001, 3111, "Release Bot", nullptr, "new wh", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_UPSERT, &ev) && ev.webhook_count == 1 &&
           strcmp(ev.webhooks[0].name, "Release Bot") == 0 && ev.webhooks[0].channel_id == 3111);
  ccord_id wh_id = ev.webhooks[0].id;

  ccord_admin_webhook_edit we = {.name = "Release Bot v2", .channel_id = 3112};
  CT_CHECK(ccord_admin_edit_webhook(&req, 2001, wh_id, &we, "move wh", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_UPSERT, &ev) && ev.webhooks[0].id == wh_id &&
           ev.webhooks[0].channel_id == 3112 && strcmp(ev.webhooks[0].name, "Release Bot v2") == 0);

  CT_CHECK(ccord_admin_delete_webhook(&req, 2001, wh_id, "drop wh", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WEBHOOK_DELETE, &ev) && ev.guild_id == 2001 && ev.expr_id == wh_id);

  CT_CHECK(ccord_admin_list_webhooks(&req, 2003, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_WEBHOOKS &&
           ev.status == CCORD_ERR_PERMISSION);

  // 2. Follow announcement channel (3112 #announcements -> 3111 #general succeeds; non-announcement 3113 -> 3111 fails):
  CT_CHECK(ccord_admin_follow_channel(&req, 2001, 3113, 3111, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_FOLLOW_CHANNEL &&
           ev.status == CCORD_ERR_INVALID);

  CT_CHECK(ccord_admin_follow_channel(&req, 2001, 3112, 3111, "follow ann", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_FOLLOWER_ADDED, &ev) && ev.guild_id == 2001 &&
           ev.channel_id == 3112 && ev.target_channel_id == 3111 && ev.expr_id != CCORD_ID_NONE);

  // 3. Application command permissions overview + refusal on 2002 (no MANAGE_GUILD):
  CT_CHECK(ccord_admin_list_app_cmd_perms(&req, 2001, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_APP_CMD_PERMS_LIST, &ev) && ev.guild_id == 2001 && ev.app_cmd_perms_count >= 1);
  CT_CHECK(ccord_admin_list_app_cmd_perms(&req, 2002, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_LIST_APP_CMD_PERMS &&
           ev.status == CCORD_ERR_PERMISSION);

  // 4. Community wizard + Welcome Screen + Onboarding on a fresh non-community server:
  CT_CHECK(ccord_admin_create_guild(&req, "Mock Comm Lab", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild && !ev.guild->feature_community);
  ccord_id cgid = ev.guild->id;
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_admin_create_channel(&req, cgid, "rules", 0, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel);
  ccord_id ch1 = ev.channel->id;
  CT_CHECK(ccord_admin_create_channel(&req, cgid, "updates", 0, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel);
  ccord_id ch2 = ev.channel->id;

  // Before COMMUNITY is enabled, Welcome Screen and Onboarding reject with CCORD_ERR_INVALID:
  CT_CHECK(ccord_admin_get_welcome_screen(&req, cgid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);
  CT_CHECK(ccord_admin_get_onboarding(&req, cgid, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  // Enable Community without rules/updates channels fails locally; foreign channel fails on server; valid channels succeed:
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_community = true;
  ge.feature_community = true;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, cgid, &ge, nullptr, &err) == CCORD_ERR_INVALID);

  ge.has_rules_channel = true;
  ge.rules_channel_id = 3111;   // belongs to guild 2001, not cgid
  ge.has_public_updates_channel = true;
  ge.public_updates_channel_id = ch2;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, cgid, &ge, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  ge.rules_channel_id = ch1;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, cgid, &ge, "enable community", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_UPSERT, &ev) && ev.guild && ev.guild->feature_community &&
           ev.guild->rules_channel_id == ch1 && ev.guild->public_updates_channel_id == ch2);

  ccord_welcome_channel ws_ch[1] = {{.channel_id = ch1, .description = "Read rules", .emoji_name = "📜"}};
  ccord_welcome_screen ws = {.enabled = true, .description = "Hello Mock Comm", .channels = ws_ch, .channel_count = 1};
  CT_CHECK(ccord_admin_edit_welcome_screen(&req, cgid, &ws, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_WELCOME_SCREEN, &ev) && ev.welcome_screen && ev.welcome_screen->enabled &&
           ev.welcome_screen->channel_count == 1);

  ccord_onboarding_option ob_opts[1] = {{.title = "Coding", .channel_count = 1, .channel_ids = {ch2}}};
  ccord_onboarding_prompt ob_prompts[1] = {{
    .title = "Interests", .required = true, .in_onboarding = true, .options = ob_opts, .option_count = 1,
  }};
  ccord_onboarding ob = {
    .guild_id = cgid, .enabled = true, .mode = CCORD_ONBOARDING_DEFAULT,
    .default_channel_count = 1, .default_channel_ids = {ch1},
    .prompts = ob_prompts, .prompt_count = 1,
  };
  CT_CHECK(ccord_admin_put_onboarding(&req, cgid, &ob, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ONBOARDING, &ev) && ev.onboarding && ev.onboarding->enabled &&
           ev.onboarding->prompt_count == 1 && ev.onboarding->prompts[0].id != 0);

  ccord_transport_destroy(t);
}

static void test_mock_m4d_channel_overwrites_clone_invites_and_voice_mod(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 97};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_for(t, CCORD_NET_SNAPSHOT, &ev));
  while (ccord_transport_poll(t, &ev)) {
  }

  ccord_admin_request req;

  // 1. Extended EDIT_CHANNEL on voice channel 3121 (bitrate + user_limit) + permission refusal on 2003:
  ccord_admin_channel_edit ed = CCORD_ADMIN_CHANNEL_EDIT_INIT;
  ed.bitrate = 96000;
  ed.user_limit = 8;
  CT_CHECK(ccord_admin_edit_channel(&req, 2001, 3121, &ed, "vc edit", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == 3121 &&
           ev.channel->bitrate == 96000 && ev.channel->user_limit == 8);

  CT_CHECK(ccord_admin_edit_channel(&req, 2003, 3311, &ed, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);

  // 2. PUT & DELETE channel permission overwrites + escalation refusal on 2002 (no MANAGE_GUILD):
  ccord_overwrite ow = {.id = 5003, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_VIEW_CHANNEL, .deny = CCORD_PERM_SEND_MESSAGES};
  CT_CHECK(ccord_admin_put_channel_overwrite(&req, 2001, 3113, &ow, "ow put", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_OVERWRITE_UPSERT, &ev) && ev.channel_id == 3113 &&
           ev.overwrite_item.id == 5003 && ev.overwrite_item.deny == CCORD_PERM_SEND_MESSAGES);

  ccord_overwrite esc_ow = {.id = 5013, .kind = CCORD_OVERWRITE_ROLE, .allow = CCORD_PERM_MANAGE_GUILD, .deny = 0};
  CT_CHECK(ccord_admin_put_channel_overwrite(&req, 2002, 3211, &esc_ow, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.admin_op == CCORD_ADMIN_PUT_CHANNEL_OVERWRITE &&
           ev.status == CCORD_ERR_PERMISSION);

  CT_CHECK(ccord_admin_delete_channel_overwrite(&req, 2001, 3113, 5003, "ow del", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_OVERWRITE_DELETE, &ev) && ev.channel_id == 3113 &&
           ev.overwrite_item.id == 5003);

  // 3. Channel-scoped invites list + refusal on 2003:
  CT_CHECK(ccord_admin_list_channel_invites(&req, 2001, 3111, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_INVITE_LIST, &ev) && ev.channel_id == 3111 && ev.invite_count >= 1 &&
           ev.invites[0].channel_id == 3111);
  CT_CHECK(ccord_admin_list_channel_invites(&req, 2003, 3311, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_PERMISSION);

  // 4. Clone Channel (with overwrites & forum tags):
  ccord_forum_tag ftag = {.name = "question", .moderated = false, .emoji_name = "❓"};
  ccord_admin_channel_clone cl = {
    .name = "help-forum", .discord_type = 15, .topic = "Ask here", .nsfw = true, .slowmode_s = 10,
    .default_auto_archive_duration = 4320, .overwrites = &ow, .overwrite_count = 1,
    .available_tags = &ftag, .available_tag_count = 1,
  };
  CT_CHECK(ccord_admin_clone_channel(&req, 2001, &cl, "clone", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel &&
           strcmp(ev.channel->name, "help-forum") == 0 && ev.channel->nsfw &&
           ev.channel->rate_limit_per_user == 10 && ev.overwrite_count == 1 && ev.overwrites[0].count == 1 &&
           ev.forum_meta_count == 1 && ev.forum_metas[0].tag_count == 1);

  // 5. Voice Moderation on seeded participant Ada (1002 in 3121 on 2001):
  CT_CHECK(ccord_admin_set_voice_mute(&req, 2001, 1002, true, "mute ada", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.guild_members[0].user_id == 1002 &&
           ev.guild_members[0].mute);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->mute);

  CT_CHECK(ccord_admin_set_voice_deaf(&req, 2001, 1002, true, "deaf ada", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_GUILD_MEMBERS, &ev) && ev.guild_members[0].deaf);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->deaf);

  CT_CHECK(ccord_admin_create_channel(&req, 2001, "AFK Lounge", 2, 0, nullptr, nullptr, &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel);
  ccord_id vc2 = ev.channel->id;

  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, vc2, "move ada", &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->channel_id == vc2);

  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, CCORD_ID_NONE, "disc ada", &err) == CCORD_OK &&
           admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state &&
           ev.voice_state->channel_id == CCORD_ID_NONE);

  // Moving a user who is disconnected fails with CCORD_ERR_INVALID:
  CT_CHECK(ccord_admin_move_voice_member(&req, 2001, 1002, vc2, nullptr, &err) == CCORD_OK && admin_call(t, &req));
  CT_CHECK(poll_for(t, CCORD_NET_ACTION_FAILED, &ev) && ev.status == CCORD_ERR_INVALID);

  ccord_transport_destroy(t);
}

// ---- M5: DM calls, stage, voice status, soundboard, Go Live ----

static void test_m5_dm_calls(void)
{
  ccord_error err;
  g_now = 40000;
  ccord_mock_config cfg = {.seed = 21, .big_channel_messages = 10, .now_ms = fake_now};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_call_action(t, 901002, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // not connected
  (void)ccord_transport_connect(t, &err);
  ccord_net_event ev;
  while (ccord_transport_poll(t, &ev)) {
  }

  CT_CHECK(ccord_transport_call_action(t, 999999, CCORD_CALL_JOIN, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_transport_call_action(t, 3111, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // guild text channel
  CT_CHECK(ccord_transport_call_action(t, 3121, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // guild voice channel

  // Seeded incoming call: the DM channel, the ring and the caller's voice state arrive unprompted.
  ccord_id call_ch = 0;
  CT_CHECK(ccord_mock_push_incoming_call(t, 1002, &call_ch, &err) == CCORD_OK && call_ch == 901002);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == 901002 &&
           ev.channel->guild_id == CCORD_DM_GUILD_ID);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.guild_id == CCORD_DM_GUILD_ID && ev.call &&
           ev.call->channel_id == 901002 && ev.call->active &&
           ev.call->ringing_count == 1 && ev.call->ringing[0] == 1001 && strcmp(ev.call->region, "rotterdam") == 0);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1002 &&
           ev.voice_state->channel_id == 901002 && ev.voice_state->guild_id == CCORD_DM_GUILD_ID);

  // Accept: self stops ringing, joins voice and gets a voice server.
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_UPDATE, &ev) && ev.call && ev.call->ringing_count == 0 && ev.call->active);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == 901002);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_SERVER_UPDATE, &ev) && ev.voice_server &&
           ev.voice_server->guild_id == CCORD_DM_GUILD_ID && ev.voice_server->channel_id == 901002 &&
           ev.voice_server->token && ev.voice_server->token[0] != '\0' &&
           ev.voice_server->endpoint && ev.voice_server->endpoint[0] != '\0');
  ccord_rtc_stats rtc;
  ccord_dave_status dave;
  ccord_mock_rtc_stats(t, &rtc);
  CT_CHECK(rtc.state == CCORD_VOICE_ESTABLISHED && rtc.ssrc != 0);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(dave.active && !dave.verified && dave.protocol_version == 1);
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_JOIN, &err) == CCORD_ERR_INVALID); // already in

  // Hang up: self leaves, but the caller keeps the call alive (no CALL_DELETE).
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_CALL_DELETE);
  }
  ccord_mock_rtc_stats(t, &rtc);
  ccord_mock_dave_status(t, &dave);
  CT_CHECK(rtc.state == CCORD_VOICE_IDLE && !dave.active);

  // The remote side hangs up: the caller leaves and the call ends.
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1002 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901002 && !ev.call);
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_ERR_NOT_FOUND);

  // Decline while ringing: self drops out of ringing[] without ever joining.
  ccord_id ch2 = 0;
  CT_CHECK(ccord_mock_push_incoming_call(t, 1003, &ch2, &err) == CCORD_OK && ch2 == 901003);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.call && ev.call->ringing_count == 1);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_call_action(t, ch2, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_UPDATE, &ev) && ev.call && ev.call->ringing_count == 0);
  while (ccord_transport_poll(t, &ev)) {
    CT_CHECK(ev.type != CCORD_NET_CALL_DELETE); // the caller is still in the call
  }

  // Outgoing call while ch2's caller is still connected: joining a DM with no call rings its recipients;
  // leaving as the last participant of THIS call ends it, even though another call is live.
  CT_CHECK(ccord_transport_open_dm(t, 1004, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CHANNEL_UPSERT, &ev) && ev.channel && ev.channel->id == 901004);
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.call && ev.call->channel_id == 901004 && ev.call->active &&
           ev.call->ringing_count == 1 && ev.call->ringing[0] == 1004);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901004);
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_LEAVE, &err) == CCORD_ERR_NOT_FOUND);

  // The ended call's row is gone: calling the same DM again rings fresh.
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_JOIN, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_CREATE, &ev) && ev.call && ev.call->channel_id == 901004 &&
           ev.call->ringing_count == 1 && ev.call->ringing[0] == 1004);
  CT_CHECK(ccord_transport_call_action(t, 901004, CCORD_CALL_LEAVE, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901004);

  // Now the remote side of ch2 hangs up.
  CT_CHECK(ccord_mock_end_call(t, ch2, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901003);

  // Remote hangup while self is connected: self is disconnected and the call row goes.
  CT_CHECK(ccord_mock_push_incoming_call(t, 1002, &call_ch, &err) == CCORD_OK && call_ch == 901002);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_transport_call_action(t, call_ch, CCORD_CALL_JOIN, &err) == CCORD_OK);
  while (ccord_transport_poll(t, &ev)) {
  }
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_OK);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1002 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  CT_CHECK(poll_for(t, CCORD_NET_VOICE_STATE_UPDATE, &ev) && ev.voice_state && ev.voice_state->user_id == 1001 &&
           ev.voice_state->channel_id == CCORD_ID_NONE);
  CT_CHECK(poll_for(t, CCORD_NET_CALL_DELETE, &ev) && ev.channel_id == 901002);
  ccord_mock_rtc_stats(t, &rtc);
  CT_CHECK(rtc.state == CCORD_VOICE_IDLE);
  CT_CHECK(ccord_mock_end_call(t, call_ch, &err) == CCORD_ERR_NOT_FOUND);

  CT_CHECK(ccord_mock_push_incoming_call(t, 1001, nullptr, &err) == CCORD_ERR_NOT_FOUND); // self
  CT_CHECK(ccord_mock_push_incoming_call(t, 424242, nullptr, &err) == CCORD_ERR_NOT_FOUND); // unknown user
  ccord_transport_destroy(t);
}


int main(void)
{
  test_voice_message_upload_playback();
  test_mock_polls_components_and_modals();
  test_mock_search_pins_reactions_and_threads();
  test_mock_m3b_folders_reorder_invites_and_discovery();
  test_mock_m3c_group_dms_requests_profiles_and_status();
  test_mock_m4a_guild_settings_safety_vanity_audit_and_danger();
  test_mock_m4b_expressions();
  test_mock_m4c_prune_and_automod();
  test_mock_m4c_integrations_and_community();
  test_mock_m4d_channel_overwrites_clone_invites_and_voice_mod();
  test_m5_dm_calls();
  return CT_RESULT("test_mock_admin");
}
