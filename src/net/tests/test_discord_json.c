// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// R1/R3 JSON decoders: message tts + forward snapshots, slash SUB_COMMAND/SUB_COMMAND_GROUP trees and
// USER/MESSAGE context commands, extended stage instances, guild scheduled events (REST + gateway), and
// embedded activities (shelf REST + EMBEDDED_ACTIVITY_UPDATE_V2 gateway).
// What matters: every model field is filled, missing/null input keeps the documented defaults, option
// parent links survive flattening, lists are bounded, and malformed payloads are rejected.
// Also components v2: the IS_COMPONENTS_V2 tree parses into the model with its placement rules and budget,
// legacy v1 rows parse unchanged, and the mock transport accepts a v2 rich message.
#include <stdio.h>
#include <string.h>

#include <ccord/model/rich.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/mock_transport.h>
#include <ccord/model/rich.h>

#include "ccord_test.h"

static ccord_status decode_dispatch(const char *type, const char *d_json, ccord_live_event *ev, ccord_error *err)
{
  char buf[8192];
  int n = snprintf(buf, sizeof buf, "{\"op\":0,\"s\":42,\"t\":\"%s\",\"d\":%s}", type, d_json);
  if (n < 0 || (size_t)n >= sizeof buf) return CCORD_ERR_RANGE;
  return ccord_live_gateway_decode(buf, (size_t)n, ev, err);
}

static void test_message_tts(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *tts_msg =
    "{\"id\":\"500\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
    "\"content\":\"hello\",\"tts\":true}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", tts_msg, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE && ev.message_count == 1);
  CT_CHECK(ev.messages[0].tts);
  ccord_live_event_free(&ev);

  // Absent tts means false, on both the gateway and the REST path.
  const char *plain_msg =
    "{\"id\":\"501\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"content\":\"hi\"}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", plain_msg, &ev, &err) == CCORD_OK);
  CT_CHECK(!ev.messages[0].tts);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_message(plain_msg, strlen(plain_msg), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(!ev.messages[0].tts);
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_message(tts_msg, strlen(tts_msg), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].tts);
  ccord_live_event_free(&ev);

  // MESSAGE_UPDATE carries tts too.
  const char *upd = "{\"id\":\"500\",\"channel_id\":\"400\",\"content\":\"hello again\",\"tts\":true}";
  CT_CHECK(decode_dispatch("MESSAGE_UPDATE", upd, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_UPDATE && ev.messages[0].tts);
  ccord_live_event_free(&ev);
}

// Voice messages: duration_secs + waveform parse on the gateway path and the REST path alike.
static void test_message_voice_waveform(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *voice_msg =
    "{\"id\":\"510\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
    "\"content\":\"\",\"flags\":8192,\"attachments\":[{\"id\":\"9\",\"filename\":\"voice-message.ogg\","
    "\"content_type\":\"audio/ogg\",\"url\":\"https://cdn.example/voice-message.ogg\",\"size\":3000,"
    "\"duration_secs\":4.5,\"waveform\":\"AAECAwQFBgcICQoLDA0ODw==\"}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", voice_msg, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].rich != nullptr);
  if (ev.message_count == 1 && ev.messages[0].rich) {
    const ccord_message_rich *r = ev.messages[0].rich;
    CT_CHECK((r->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0 && r->attachment_count == 1);
    CT_CHECK(r->attachments[0].duration_ms == 4500 &&
             strcmp(r->attachments[0].waveform_b64, "AAECAwQFBgcICQoLDA0ODw==") == 0);
  }
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_message(voice_msg, strlen(voice_msg), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].rich && ev.messages[0].rich->attachment_count == 1 &&
           strcmp(ev.messages[0].rich->attachments[0].waveform_b64, "AAECAwQFBgcICQoLDA0ODw==") == 0);
  ccord_live_event_free(&ev);

  // No waveform key, or one that is not a base64 string: an empty string, never null, and the rest still parses.
  const char *bare =
    "{\"id\":\"511\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
    "\"content\":\"\",\"attachments\":[{\"id\":\"9\",\"filename\":\"a.ogg\",\"url\":\"https://cdn.example/a.ogg\","
    "\"duration_secs\":1.25},{\"id\":\"10\",\"filename\":\"b.ogg\",\"url\":\"https://cdn.example/b.ogg\","
    "\"waveform\":\"bad wave\"}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", bare, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].rich && ev.messages[0].rich->attachment_count == 2);
  if (ev.message_count == 1 && ev.messages[0].rich && ev.messages[0].rich->attachment_count == 2) {
    const ccord_message_rich *r = ev.messages[0].rich;
    CT_CHECK(r->attachments[0].duration_ms == 1250 && r->attachments[0].waveform_b64 &&
             r->attachments[0].waveform_b64[0] == '\0');
    CT_CHECK(r->attachments[1].waveform_b64 && r->attachments[1].waveform_b64[0] == '\0');
  }
  ccord_live_event_free(&ev);
}

static void test_message_forward_snapshot(void)
{
  ccord_live_event ev;
  ccord_error err;
  // flags = CROSSPOSTED (1) | HAS_SNAPSHOT (1<<14); reference type 1 marks a forward, not a reply.
  const char *fwd =
    "{\"id\":\"510\",\"channel_id\":\"400\",\"guild_id\":\"10\","
    "\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"content\":\"\",\"flags\":16385,"
    "\"message_reference\":{\"type\":1,\"message_id\":\"490\",\"channel_id\":\"400\",\"guild_id\":\"10\"},"
    "\"message_snapshots\":[{\"message\":{\"content\":\"original text\","
    "\"timestamp\":\"2026-01-02T03:04:05.123000+00:00\","
    "\"attachments\":[{\"id\":\"1\",\"filename\":\"f.png\",\"url\":\"https://cdn.discordapp.com/f.png\",\"size\":10}],"
    "\"embeds\":[{\"title\":\"e\"}],\"sticker_items\":[{\"id\":\"9\",\"name\":\"wave\",\"format_type\":1}]}}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", fwd, &ev, &err) == CCORD_OK);
  const ccord_message *m = &ev.messages[0];
  CT_CHECK(m->reply_to_id == CCORD_ID_NONE);   // a forward is not a reply
  CT_CHECK(m->rich != nullptr);
  if (m->rich) {
    CT_CHECK(m->rich->reference.kind == CCORD_REF_FORWARD);
    CT_CHECK(m->rich->reference.message_id == 490);
    CT_CHECK((m->rich->flags & CCORD_MSGFLAG_CROSSPOSTED) != 0);
    CT_CHECK((m->rich->flags & CCORD_MSGFLAG_HAS_SNAPSHOT) != 0);
    CT_CHECK((m->rich->present & CCORD_RICH_HAS_FORWARD) != 0);
    CT_CHECK(m->rich->forward.present);
    CT_CHECK(strcmp(m->rich->forward.content, "original text") == 0);
    // 2026-01-02T03:04:05.123Z
    CT_CHECK(m->rich->forward.timestamp_ms == 1767312000000LL + 3 * 3600000LL + 4 * 60000LL + 5000LL + 123LL);
    CT_CHECK(m->rich->forward.attachment_count == 1);
    CT_CHECK(m->rich->forward.embed_count == 1);
    CT_CHECK(m->rich->forward.sticker_count == 1);
  }
  ccord_live_event_free(&ev);

  // A crossposted announcement without a snapshot keeps the flag and is not a forward.
  const char *crosspost =
    "{\"id\":\"511\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
    "\"content\":\"news\",\"flags\":1}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", crosspost, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && (ev.messages[0].rich->flags & CCORD_MSGFLAG_CROSSPOSTED) != 0);
  CT_CHECK(!ev.messages[0].rich->forward.present);
  ccord_live_event_free(&ev);
}

static void test_message_forward_reference_only(void)
{
  ccord_live_event ev;
  ccord_error err;
  // Type-1 reference without snapshots still marks the message as a forward (snapshot can arrive later).
  const char *fwd =
    "{\"id\":\"512\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"content\":\"\","
    "\"message_reference\":{\"type\":1,\"message_id\":\"490\",\"channel_id\":\"400\"}}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", fwd, &ev, &err) == CCORD_OK);
  const ccord_message *m = &ev.messages[0];
  CT_CHECK(m->reply_to_id == CCORD_ID_NONE);
  CT_CHECK(m->rich && m->rich->reference.kind == CCORD_REF_FORWARD);
  CT_CHECK(m->rich && m->rich->forward.present);
  CT_CHECK(m->rich && strcmp(m->rich->forward.content, "") == 0);
  CT_CHECK(m->rich && (m->rich->present & CCORD_RICH_HAS_FORWARD) != 0);
  ccord_live_event_free(&ev);

  // Regression: a type-0 reference stays a reply with its preview.
  const char *reply =
    "{\"id\":\"513\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"content\":\"re\","
    "\"message_reference\":{\"type\":0,\"message_id\":\"490\",\"channel_id\":\"400\"},"
    "\"referenced_message\":{\"id\":\"490\",\"content\":\"earlier\",\"author\":{\"id\":\"301\",\"username\":\"bob\"}}}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", reply, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].reply_to_id == 490);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->reference.kind == CCORD_REF_REPLY);
  CT_CHECK(ev.messages[0].rich && !ev.messages[0].rich->forward.present);
  ccord_live_event_free(&ev);
}

static void test_slash_subcommand_tree(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *json =
    "[{\"id\":\"9001\",\"application_id\":\"8001\",\"version\":\"9002\",\"type\":1,"
    "\"name\":\"moderate\",\"description\":\"Moderation tools\",\"options\":["
    "{\"type\":2,\"name\":\"user\",\"description\":\"User actions\",\"options\":["
    "{\"type\":1,\"name\":\"ban\",\"description\":\"Ban a user\",\"options\":["
    "{\"type\":6,\"name\":\"target\",\"description\":\"Who\",\"required\":true},"
    "{\"type\":4,\"name\":\"days\",\"description\":\"Delete days\"}]},"
    "{\"type\":1,\"name\":\"kick\",\"description\":\"Kick a user\",\"options\":["
    "{\"type\":6,\"name\":\"target\",\"description\":\"Who\",\"required\":true}]}]},"
    "{\"type\":1,\"name\":\"purge\",\"description\":\"Bulk delete\",\"options\":["
    "{\"type\":4,\"name\":\"count\",\"description\":\"How many\",\"required\":true,\"autocomplete\":true},"
    "{\"type\":3,\"name\":\"reason\",\"description\":\"Why\"}]}]}]";
  CT_CHECK(ccord_live_json_slash_commands(json, strlen(json), 10, 400, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SLASH_COMMANDS && ev.ev.slash_command_count == 1);
  const ccord_slash_command *cmd = ev.ev.slash_commands;
  CT_CHECK(cmd && cmd->command_type == 1);
  CT_CHECK(cmd && cmd->option_count == 9);
  if (cmd && cmd->option_count == 9) {
    // Depth-first flatten: group, its subcommands and their leaves, then the root subcommand's leaves.
    const struct { ccord_slash_opt_kind kind; const char *name; uint8_t parent; } want[9] = {
      {CCORD_SLASH_OPT_SUB_COMMAND_GROUP, "user", 0},
      {CCORD_SLASH_OPT_SUB_COMMAND, "ban", 1},
      {CCORD_SLASH_OPT_USER, "target", 2},
      {CCORD_SLASH_OPT_INTEGER, "days", 2},
      {CCORD_SLASH_OPT_SUB_COMMAND, "kick", 1},
      {CCORD_SLASH_OPT_USER, "target", 5},
      {CCORD_SLASH_OPT_SUB_COMMAND, "purge", 0},
      {CCORD_SLASH_OPT_INTEGER, "count", 7},
      {CCORD_SLASH_OPT_STRING, "reason", 7},
    };
    for (size_t i = 0; i < 9; i++) {
      CT_CHECK(cmd->options[i].kind == want[i].kind);
      CT_CHECK(strcmp(cmd->options[i].name, want[i].name) == 0);
      CT_CHECK(cmd->options[i].parent_plus_one == want[i].parent);
    }
    CT_CHECK(cmd->options[2].required && !cmd->options[3].required);
    CT_CHECK(cmd->options[7].required && cmd->options[7].autocomplete);
    CT_CHECK(!cmd->options[0].required && !cmd->options[1].autocomplete);
    CT_CHECK(ccord_slash_command_validate(cmd, &err) == CCORD_OK);
  }
  ccord_live_event_free(&ev);
}

static void test_slash_context_commands(void)
{
  ccord_live_event ev;
  ccord_error err;
  // USER / MESSAGE context-menu commands may have spaces in their names; unknown types are dropped;
  // options on a context command are ignored (Discord never sends them).
  const char *json =
    "[{\"id\":\"9101\",\"application_id\":\"8001\",\"type\":2,\"name\":\"Report User\",\"description\":\"\"},"
    "{\"id\":\"9102\",\"application_id\":\"8001\",\"type\":3,\"name\":\"Pin to Board\",\"description\":\"\"},"
    "{\"id\":\"9103\",\"application_id\":\"8001\",\"type\":4,\"name\":\"bogus\"},"
    "{\"id\":\"9104\",\"application_id\":\"8001\",\"type\":2,\"name\":\"With Options\","
    "\"options\":[{\"type\":3,\"name\":\"x\",\"description\":\"d\"}]}]";
  CT_CHECK(ccord_live_json_slash_commands(json, strlen(json), 10, 400, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.slash_command_count == 3);
  if (ev.ev.slash_command_count == 3) {
    CT_CHECK(ev.ev.slash_commands[0].id == 9101 && ev.ev.slash_commands[0].command_type == 2);
    CT_CHECK(strcmp(ev.ev.slash_commands[0].name, "Report User") == 0);
    CT_CHECK(ev.ev.slash_commands[1].id == 9102 && ev.ev.slash_commands[1].command_type == 3);
    CT_CHECK(strcmp(ev.ev.slash_commands[1].name, "Pin to Board") == 0);
    CT_CHECK(ev.ev.slash_commands[2].id == 9104 && ev.ev.slash_commands[2].command_type == 2);
    for (size_t i = 0; i < 3; i++) CT_CHECK(ev.ev.slash_commands[i].option_count == 0);
  }
  ccord_live_event_free(&ev);

  // A missing type still defaults to CHAT_INPUT.
  const char *dflt = "[{\"id\":\"9105\",\"application_id\":\"8001\",\"name\":\"plain\",\"description\":\"d\"}]";
  CT_CHECK(ccord_live_json_slash_commands(dflt, strlen(dflt), 10, 400, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.slash_command_count == 1 && ev.ev.slash_commands[0].command_type == 1);
  ccord_live_event_free(&ev);
}

static void test_slash_malformed_tree_and_cap(void)
{
  ccord_live_event ev;
  ccord_error err;
  // A group inside a group, a leaf directly in a group and a subcommand inside a subcommand are all
  // invalid nesting and get skipped; the well-formed parts survive. A command mixing root containers
  // with root leaves violates Discord's root rule and is dropped whole by validation.
  const char *json =
    "[{\"id\":\"9201\",\"application_id\":\"8001\",\"type\":1,\"name\":\"bad\",\"description\":\"d\",\"options\":["
    "{\"type\":2,\"name\":\"grp\",\"description\":\"g\",\"options\":["
    "{\"type\":2,\"name\":\"nested\",\"description\":\"n\",\"options\":["
    "{\"type\":1,\"name\":\"deep\",\"description\":\"d\"}]},"
    "{\"type\":3,\"name\":\"stray\",\"description\":\"leaf in group\"},"
    "{\"type\":1,\"name\":\"ok\",\"description\":\"sub\",\"options\":["
    "{\"type\":1,\"name\":\"subsub\",\"description\":\"not allowed\"},"
    "{\"type\":5,\"name\":\"flag\",\"description\":\"bool leaf\"}]}]}]},"
    "{\"id\":\"9202\",\"application_id\":\"8001\",\"type\":1,\"name\":\"mixed\",\"description\":\"d\",\"options\":["
    "{\"type\":1,\"name\":\"sub\",\"description\":\"s\",\"options\":[]},"
    "{\"type\":3,\"name\":\"leaf\",\"description\":\"l\"}]}]";
  CT_CHECK(ccord_live_json_slash_commands(json, strlen(json), 10, 400, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.slash_command_count == 1);
  const ccord_slash_command *cmd = ev.ev.slash_commands;
  CT_CHECK(cmd && cmd->option_count == 3);
  if (cmd && cmd->option_count == 3) {
    CT_CHECK(cmd->options[0].kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP && cmd->options[0].parent_plus_one == 0);
    CT_CHECK(cmd->options[1].kind == CCORD_SLASH_OPT_SUB_COMMAND && cmd->options[1].parent_plus_one == 1);
    CT_CHECK(strcmp(cmd->options[1].name, "ok") == 0);
    CT_CHECK(cmd->options[2].kind == CCORD_SLASH_OPT_BOOLEAN && cmd->options[2].parent_plus_one == 2);
    CT_CHECK(strcmp(cmd->options[2].name, "flag") == 0);
  }
  ccord_live_event_free(&ev);

  // The flattened list is bounded by CCORD_SLASH_MAX_OPTIONS.
  char opts[4096];
  size_t off = 0;
  for (int i = 0; i < 30; i++) {
    int n = snprintf(opts + off, sizeof opts - off, "%s{\"type\":3,\"name\":\"opt%d\",\"description\":\"d\"}",
                     i ? "," : "", i);
    CT_CHECK(n > 0 && (size_t)n < sizeof opts - off);
    off += (size_t)n;
  }
  char big[8192];
  int bn = snprintf(big, sizeof big,
                    "[{\"id\":\"9301\",\"application_id\":\"8001\",\"type\":1,\"name\":\"many\","
                    "\"description\":\"d\",\"options\":[%s]}]", opts);
  CT_CHECK(bn > 0 && (size_t)bn < sizeof big);
  CT_CHECK(ccord_live_json_slash_commands(big, strlen(big), 10, 400, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.slash_command_count == 1);
  CT_CHECK(ev.ev.slash_commands[0].option_count == CCORD_SLASH_MAX_OPTIONS);
  ccord_live_event_free(&ev);
}

static void test_stage_instance_extended(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *obj =
    "{\"id\":\"7001\",\"guild_id\":\"10\",\"channel_id\":\"400\",\"topic\":\"Launch party\","
    "\"privacy_level\":2,\"discoverable_disabled\":true,\"guild_scheduled_event_id\":\"7500\"}";
  CT_CHECK(decode_dispatch("STAGE_INSTANCE_CREATE", obj, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && !ev.ev.stage_deleted);
  const ccord_stage_instance *si = ev.ev.stage_instance;
  CT_CHECK(si && si->id == 7001 && si->guild_id == 10 && si->channel_id == 400);
  CT_CHECK(si && strcmp(si->topic, "Launch party") == 0);
  CT_CHECK(si && si->privacy_level == 2 && si->discoverable_disabled && si->guild_scheduled_event_id == 7500);
  ccord_live_event_free(&ev);

  CT_CHECK(decode_dispatch("STAGE_INSTANCE_DELETE", obj, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STAGE_INSTANCE_UPDATE && ev.ev.stage_deleted);
  CT_CHECK(ev.ev.stage_instance && ev.ev.stage_instance->id == 7001);
  ccord_live_event_free(&ev);

  // REST answer: hints fill what the payload omits; the payload's own ids win.
  const char *minimal = "{\"channel_id\":\"400\",\"topic\":\"Q&A\"}";
  CT_CHECK(ccord_live_json_stage_instance(minimal, strlen(minimal), 10, 400, &ev, &err) == CCORD_OK);
  si = ev.ev.stage_instance;
  CT_CHECK(si && si->guild_id == 10 && si->channel_id == 400 && si->id == CCORD_ID_NONE);
  CT_CHECK(si && si->privacy_level == 0 && !si->discoverable_disabled && si->guild_scheduled_event_id == CCORD_ID_NONE);
  CT_CHECK(si && strcmp(si->topic, "Q&A") == 0);
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_stage_instance(obj, strlen(obj), 99, 999, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.stage_instance && ev.ev.stage_instance->guild_id == 10 && ev.ev.stage_instance->channel_id == 400);
  ccord_live_event_free(&ev);

  const char *no_channel = "{\"topic\":\"no channel\"}";
  CT_CHECK(ccord_live_json_stage_instance(no_channel, strlen(no_channel), 10, CCORD_ID_NONE, &ev, &err) ==
           CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
}

static void test_scheduled_event_rest_single(void)
{
  ccord_live_event ev;
  ccord_error err;
  // EXTERNAL event: null channel_id, location lives in entity_metadata.
  const char *obj =
    "{\"id\":\"7500\",\"guild_id\":\"10\",\"channel_id\":null,\"creator_id\":\"300\","
    "\"name\":\"Community AMA\",\"description\":\"Ask us anything\","
    "\"scheduled_start_time\":\"2026-02-03T18:00:00.000000+00:00\","
    "\"scheduled_end_time\":\"2026-02-03T19:30:00.000000+00:00\","
    "\"privacy_level\":2,\"status\":1,\"entity_type\":3,"
    "\"entity_metadata\":{\"location\":\"https://example.com/ama\"},\"user_count\":42}";
  CT_CHECK(ccord_live_json_scheduled_event(obj, strlen(obj), 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_UPDATE && !ev.ev.scheduled_event_deleted);
  CT_CHECK(ev.ev.scheduled_event_count == 1 && ev.ev.scheduled_event_id == 7500 && ev.ev.guild_id == 10);
  const ccord_scheduled_event *se = ev.ev.scheduled_events;
  CT_CHECK(se && se->id == 7500 && se->guild_id == 10 && se->channel_id == CCORD_ID_NONE && se->creator_id == 300);
  CT_CHECK(se && strcmp(se->name, "Community AMA") == 0);
  CT_CHECK(se && strcmp(se->description, "Ask us anything") == 0);
  CT_CHECK(se && strcmp(se->location, "https://example.com/ama") == 0);
  CT_CHECK(se && se->scheduled_start_ms == 1770141600000LL);
  CT_CHECK(se && se->scheduled_end_ms == 1770147000000LL);
  CT_CHECK(se && se->privacy_level == 2 && se->status == CCORD_SCHEDULED_EVENT_SCHEDULED);
  CT_CHECK(se && se->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL && se->user_count == 42 && !se->me_rsvped);
  ccord_live_event_free(&ev);

  // guild hint fills a payload without guild_id; null end time and absent extras keep defaults.
  const char *hinted =
    "{\"id\":\"7502\",\"name\":\"Voice hangout\",\"channel_id\":\"420\","
    "\"scheduled_start_time\":\"2026-02-04T18:00:00Z\",\"scheduled_end_time\":null,"
    "\"status\":2,\"entity_type\":2}";
  CT_CHECK(ccord_live_json_scheduled_event(hinted, strlen(hinted), 10, &ev, &err) == CCORD_OK);
  se = ev.ev.scheduled_events;
  CT_CHECK(se && se->guild_id == 10 && se->channel_id == 420 && se->scheduled_end_ms == 0);
  CT_CHECK(se && se->status == CCORD_SCHEDULED_EVENT_ACTIVE && se->entity_type == CCORD_SCHEDULED_EVENT_VOICE);
  CT_CHECK(se && se->description[0] == '\0' && se->location[0] == '\0' && se->user_count == 0);
  ccord_live_event_free(&ev);

  // Out-of-range enums clamp to 0 (not reported); malformed objects are rejected.
  const char *clamped =
    "{\"id\":\"7503\",\"guild_id\":\"10\",\"name\":\"x\",\"scheduled_start_time\":\"2026-02-04T18:00:00Z\","
    "\"status\":9,\"entity_type\":7,\"privacy_level\":-3}";
  CT_CHECK(ccord_live_json_scheduled_event(clamped, strlen(clamped), 10, &ev, &err) == CCORD_OK);
  se = ev.ev.scheduled_events;
  CT_CHECK(se && se->status == 0 && se->entity_type == 0 && se->privacy_level == 0);
  ccord_live_event_free(&ev);

  const char *no_id = "{\"guild_id\":\"10\",\"name\":\"no id\"}";
  CT_CHECK(ccord_live_json_scheduled_event(no_id, strlen(no_id), 10, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
  const char *no_name = "{\"id\":\"7504\",\"guild_id\":\"10\"}";
  CT_CHECK(ccord_live_json_scheduled_event(no_name, strlen(no_name), 10, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_scheduled_event(nullptr, 0, 10, &ev, &err) == CCORD_ERR_INVALID);
}

static void test_scheduled_event_rest_list(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *list =
    "[{\"id\":\"7500\",\"guild_id\":\"10\",\"channel_id\":\"410\",\"name\":\"Stage talk\","
    "\"scheduled_start_time\":\"2026-02-03T18:00:00Z\",\"privacy_level\":2,\"status\":1,"
    "\"entity_type\":1,\"user_count\":7},"
    "{\"id\":\"7501\",\"guild_id\":\"10\",\"channel_id\":\"420\",\"name\":\"Voice hangout\","
    "\"scheduled_start_time\":\"2026-02-04T18:00:00Z\",\"privacy_level\":2,\"status\":2,\"entity_type\":2},"
    "{\"guild_id\":\"10\",\"name\":\"missing id\"}]";
  CT_CHECK(ccord_live_json_scheduled_event_list(list, strlen(list), 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_LIST && ev.ev.guild_id == 10);
  CT_CHECK(ev.ev.scheduled_event_count == 2);   // the malformed entry is dropped
  if (ev.ev.scheduled_event_count == 2) {
    CT_CHECK(ev.ev.scheduled_events[0].id == 7500 && ev.ev.scheduled_events[0].user_count == 7);
    CT_CHECK(ev.ev.scheduled_events[0].entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE);
    CT_CHECK(ev.ev.scheduled_events[1].id == 7501);
    CT_CHECK(ev.ev.scheduled_events[1].entity_type == CCORD_SCHEDULED_EVENT_VOICE);
  }
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_scheduled_event_list("[]", 2, 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_LIST && ev.ev.scheduled_event_count == 0);
  CT_CHECK(ev.ev.scheduled_events == nullptr);
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_scheduled_event_list("{}", 2, 10, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_scheduled_event_list(nullptr, 0, 10, &ev, &err) == CCORD_ERR_INVALID);

  // The list is bounded by CCORD_LIVE_MAX_SCHEDULED_EVENTS.
  char big[16384];
  size_t off = 0;
  int n = snprintf(big + off, sizeof big - off, "[");
  CT_CHECK(n > 0);
  off += (size_t)n;
  for (unsigned i = 0; i < CCORD_LIVE_MAX_SCHEDULED_EVENTS + 5u; i++) {
    n = snprintf(big + off, sizeof big - off,
                 "%s{\"id\":\"%u\",\"guild_id\":\"10\",\"name\":\"e%u\","
                 "\"scheduled_start_time\":\"2026-02-04T18:00:00Z\",\"status\":1,\"entity_type\":2}",
                 i ? "," : "", 8000u + i, i);
    CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
    off += (size_t)n;
  }
  n = snprintf(big + off, sizeof big - off, "]");
  CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
  off += (size_t)n;
  CT_CHECK(ccord_live_json_scheduled_event_list(big, off, 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.scheduled_event_count == CCORD_LIVE_MAX_SCHEDULED_EVENTS);
  ccord_live_event_free(&ev);
}

static void test_scheduled_event_gateway(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *obj =
    "{\"id\":\"7500\",\"guild_id\":\"10\",\"channel_id\":\"410\",\"creator_id\":\"300\","
    "\"name\":\"Stage talk\",\"description\":\"All about stages\","
    "\"scheduled_start_time\":\"2026-02-03T18:00:00Z\",\"scheduled_end_time\":null,"
    "\"privacy_level\":2,\"status\":1,\"entity_type\":1,\"user_count\":7}";
  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_CREATE", obj, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_UPDATE && !ev.ev.scheduled_event_deleted);
  CT_CHECK(ev.ev.guild_id == 10 && ev.ev.scheduled_event_count == 1 && ev.ev.scheduled_event_id == 7500);
  CT_CHECK(ev.ev.scheduled_events && strcmp(ev.ev.scheduled_events[0].name, "Stage talk") == 0);
  CT_CHECK(ev.ev.scheduled_events && ev.ev.scheduled_events[0].user_count == 7);
  ccord_live_event_free(&ev);

  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_UPDATE", obj, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_UPDATE && !ev.ev.scheduled_event_deleted);
  ccord_live_event_free(&ev);

  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_DELETE", obj, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_UPDATE && ev.ev.scheduled_event_deleted);
  CT_CHECK(ev.ev.scheduled_event_id == 7500 && ev.ev.guild_id == 10);
  ccord_live_event_free(&ev);

  // A delete may arrive with only the identifying fields.
  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_DELETE", "{\"id\":\"7500\",\"guild_id\":\"10\"}", &ev, &err) ==
           CCORD_OK);
  CT_CHECK(ev.ev.scheduled_event_deleted && ev.ev.scheduled_event_id == 7500);
  ccord_live_event_free(&ev);

  const char *user = "{\"guild_scheduled_event_id\":\"7500\",\"guild_id\":\"10\",\"user_id\":\"300\"}";
  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_USER_ADD", user, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_USER_CHANGE && ev.ev.scheduled_event_user_added);
  CT_CHECK(ev.ev.guild_id == 10 && ev.ev.scheduled_event_id == 7500 && ev.ev.user_id == 300);
  ccord_live_event_free(&ev);

  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_USER_REMOVE", user, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SCHEDULED_EVENT_USER_CHANGE && !ev.ev.scheduled_event_user_added);
  CT_CHECK(ev.ev.scheduled_event_id == 7500 && ev.ev.user_id == 300);
  ccord_live_event_free(&ev);

  // Malformed payloads are rejected, not half-decoded.
  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_CREATE", "{\"guild_id\":\"10\",\"name\":\"no id\"}", &ev, &err) ==
           CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_USER_ADD", "{\"guild_id\":\"10\",\"user_id\":\"300\"}", &ev,
                           &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
  CT_CHECK(decode_dispatch("GUILD_SCHEDULED_EVENT_USER_REMOVE",
                           "{\"guild_scheduled_event_id\":\"7500\",\"guild_id\":\"10\"}", &ev, &err) ==
           CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
}

// Embedded activities: the shelf answer joins configs to applications, keeps web entries and sorts by rank.
static void test_activity_shelf_rest(void)
{
  ccord_live_event ev;
  ccord_error err;
  // 9002/9001 are web configs joined to their apps; 9003 (android) and 9004 (ios/android list) are dropped;
  // 9005 names web in a platforms list and has no rank (sorts after the ranked entries); 9999 has no application
  // and is dropped.
  const char *shelf =
    "{\"activities\":["
    "{\"application_id\":\"9002\",\"shelf_rank\":2,\"platform\":\"web\"},"
    "{\"application_id\":\"9001\",\"shelf_rank\":1,\"platform\":\"web\"},"
    "{\"application_id\":\"9003\",\"shelf_rank\":0,\"platform\":\"android\"},"
    "{\"application_id\":\"9004\",\"platforms\":[\"ios\",\"android\"]},"
    "{\"application_id\":\"9005\",\"platforms\":[\"web\",\"ios\"]},"
    "{\"application_id\":\"9999\",\"platform\":\"web\"}],"
    "\"applications\":["
    "{\"id\":\"9001\",\"name\":\"Poker Night\",\"description\":\"Cards with friends\",\"icon\":\"ic1\","
    "\"cover_image\":\"cv1\",\"max_participants\":25},"
    "{\"id\":\"9002\",\"name\":\"Watch Together\",\"icon\":\"ic2\"},"
    "{\"id\":\"9005\",\"name\":\"Web List\"}],"
    "\"assets\":[]}";
  CT_CHECK(ccord_live_json_activity_shelf(shelf, strlen(shelf), 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ACTIVITY_SHELF && ev.ev.guild_id == 10);
  CT_CHECK(ev.ev.activity_app_count == 3);
  if (ev.ev.activity_app_count == 3) {
    const ccord_activity_app *apps = ev.ev.activity_apps;
    CT_CHECK(apps[0].application_id == 9001 && apps[0].shelf_rank == 1);
    CT_CHECK(strcmp(apps[0].name, "Poker Night") == 0);
    CT_CHECK(strcmp(apps[0].description, "Cards with friends") == 0);
    CT_CHECK(strcmp(apps[0].icon_hash, "ic1") == 0 && strcmp(apps[0].cover_hash, "cv1") == 0);
    CT_CHECK(apps[0].max_participants == 25);
    CT_CHECK(apps[1].application_id == 9002 && apps[1].shelf_rank == 2);
    CT_CHECK(strcmp(apps[1].name, "Watch Together") == 0 && strcmp(apps[1].icon_hash, "ic2") == 0);
    CT_CHECK(apps[1].cover_hash[0] == '\0' && apps[1].max_participants == 0);
    CT_CHECK(apps[2].application_id == 9005 && apps[2].shelf_rank == 0);
    CT_CHECK(strcmp(apps[2].name, "Web List") == 0);
    CT_CHECK(apps[2].description[0] == '\0' && apps[2].icon_hash[0] == '\0' && apps[2].max_participants == 0);
  }
  ccord_live_event_free(&ev);

  // Equal ranks keep their arrival order (stable sort); a config without platform info is kept; max_participants
  // and the icon/cover hashes may live on the config or under their alternate names.
  const char *ties =
    "{\"activities\":["
    "{\"application_id\":\"9001\",\"shelf_rank\":3},"
    "{\"application_id\":\"9002\",\"shelf_rank\":3,\"max_participants\":8}],"
    "\"applications\":["
    "{\"id\":\"9001\",\"name\":\"First\",\"icon_hash\":\"ih1\",\"cover_hash\":\"ch1\"},"
    "{\"id\":\"9002\",\"name\":\"Second\"}]}";
  CT_CHECK(ccord_live_json_activity_shelf(ties, strlen(ties), 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.activity_app_count == 2);
  if (ev.ev.activity_app_count == 2) {
    CT_CHECK(ev.ev.activity_apps[0].application_id == 9001);
    CT_CHECK(ev.ev.activity_apps[1].application_id == 9002 && ev.ev.activity_apps[1].max_participants == 8);
    CT_CHECK(strcmp(ev.ev.activity_apps[0].icon_hash, "ih1") == 0);
    CT_CHECK(strcmp(ev.ev.activity_apps[0].cover_hash, "ch1") == 0);
  }
  ccord_live_event_free(&ev);

  // Other platform shapes: supported_platforms lists and client_platform_config objects keyed by platform.
  const char *shapes =
    "{\"activities\":["
    "{\"application_id\":\"9001\",\"shelf_rank\":2,\"supported_platforms\":[\"android\",\"web\"]},"
    "{\"application_id\":\"9002\",\"shelf_rank\":1,\"client_platform_config\":{\"web\":{\"label_type\":0}}},"
    "{\"application_id\":\"9003\",\"shelf_rank\":3,\"supported_platforms\":[\"ios\"]},"
    "{\"application_id\":\"9004\",\"shelf_rank\":4,\"client_platform_config\":{\"android\":{}}}],"
    "\"applications\":["
    "{\"id\":\"9001\",\"name\":\"A\"},{\"id\":\"9002\",\"name\":\"B\"},"
    "{\"id\":\"9003\",\"name\":\"C\"},{\"id\":\"9004\",\"name\":\"D\"}]}";
  CT_CHECK(ccord_live_json_activity_shelf(shapes, strlen(shapes), 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.activity_app_count == 2);
  if (ev.ev.activity_app_count == 2) {
    CT_CHECK(ev.ev.activity_apps[0].application_id == 9002 && ev.ev.activity_apps[1].application_id == 9001);
  }
  ccord_live_event_free(&ev);

  // An empty shelf is a valid answer (it clears the session's shelf); malformed input is rejected.
  CT_CHECK(ccord_live_json_activity_shelf("{}", 2, 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ACTIVITY_SHELF && ev.ev.activity_app_count == 0);
  CT_CHECK(ev.ev.activity_apps == nullptr);
  ccord_live_event_free(&ev);
  const char *empty_shelf = "{\"activities\":[],\"applications\":[]}";
  CT_CHECK(ccord_live_json_activity_shelf(empty_shelf, strlen(empty_shelf), 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.activity_app_count == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_activity_shelf("[]", 2, 10, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_activity_shelf(nullptr, 0, 10, &ev, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_json_activity_shelf("{}", 2, CCORD_ID_NONE, &ev, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_json_activity_shelf("{}", 2, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_ERR_INVALID);

  // The shelf is bounded by CCORD_LIVE_MAX_ACTIVITY_APPS.
  char big[16384];
  size_t off = 0;
  int n = snprintf(big + off, sizeof big - off, "{\"activities\":[");
  CT_CHECK(n > 0);
  off += (size_t)n;
  for (unsigned i = 0; i < CCORD_LIVE_MAX_ACTIVITY_APPS + 5u; i++) {
    n = snprintf(big + off, sizeof big - off, "%s{\"application_id\":\"%u\",\"shelf_rank\":%u}",
                 i ? "," : "", 9000u + i, CCORD_LIVE_MAX_ACTIVITY_APPS + 5u - i);
    CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
    off += (size_t)n;
  }
  n = snprintf(big + off, sizeof big - off, "],\"applications\":[");
  CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
  off += (size_t)n;
  for (unsigned i = 0; i < CCORD_LIVE_MAX_ACTIVITY_APPS + 5u; i++) {
    n = snprintf(big + off, sizeof big - off, "%s{\"id\":\"%u\",\"name\":\"a%u\"}", i ? "," : "", 9000u + i, i);
    CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
    off += (size_t)n;
  }
  n = snprintf(big + off, sizeof big - off, "]}");
  CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
  off += (size_t)n;
  CT_CHECK(ccord_live_json_activity_shelf(big, off, 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.activity_app_count == CCORD_LIVE_MAX_ACTIVITY_APPS);
  if (ev.ev.activity_app_count == CCORD_LIVE_MAX_ACTIVITY_APPS) {
    // The kept entries are the first 50 configs, sorted by rank: the last kept config (rank 6) sorts first.
    CT_CHECK(ev.ev.activity_apps[0].application_id == 9000u + CCORD_LIVE_MAX_ACTIVITY_APPS - 1u);
    CT_CHECK(ev.ev.activity_apps[0].shelf_rank == 6);
  }
  ccord_live_event_free(&ev);
}

// EMBEDDED_ACTIVITY_UPDATE_V2: one instance object; empty or absent participants mean the instance ended.
static void test_activity_instance_gateway(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *obj =
    "{\"instance_id\":\"7001\",\"application_id\":\"9001\","
    "\"location\":{\"id\":\"400\",\"kind\":\"gc\",\"channel_id\":\"400\",\"guild_id\":\"10\"},"
    "\"participants\":[\"300\",\"301\"]}";
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", obj, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ACTIVITY_INSTANCE_UPDATE);
  CT_CHECK(ev.ev.guild_id == 10 && ev.ev.channel_id == 400);
  const ccord_activity_instance *inst = &ev.ev.activity_instance;
  CT_CHECK(inst->instance_id == 7001 && inst->application_id == 9001);
  CT_CHECK(inst->location_id == 400 && inst->guild_id == 10 && inst->channel_id == 400);
  CT_CHECK(inst->participant_count == 2 && inst->participants[0] == 300 && inst->participants[1] == 301);
  ccord_live_event_free(&ev);

  // Empty participants: the instance ended, but its ids still travel so the store can drop it.
  const char *ended =
    "{\"instance_id\":\"7001\",\"application_id\":\"9001\","
    "\"location\":{\"id\":\"400\",\"kind\":\"gc\",\"channel_id\":\"400\",\"guild_id\":\"10\"},\"participants\":[]}";
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", ended, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ACTIVITY_INSTANCE_UPDATE && ev.ev.activity_instance.participant_count == 0);
  CT_CHECK(ev.ev.activity_instance.instance_id == 7001 && ev.ev.channel_id == 400);
  ccord_live_event_free(&ev);

  // Absent participants mean ended too.
  const char *no_participants =
    "{\"instance_id\":\"7001\",\"application_id\":\"9001\","
    "\"location\":{\"id\":\"400\",\"kind\":\"gc\",\"channel_id\":\"400\",\"guild_id\":\"10\"}}";
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", no_participants, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.activity_instance.participant_count == 0);
  ccord_live_event_free(&ev);

  // A non-numeric location id falls back to the voice channel (the leave path segment); a missing location
  // falls back to top-level ids; participant objects and non-id entries are tolerated.
  const char *odd =
    "{\"instance_id\":\"7002\",\"application_id\":\"9001\","
    "\"location\":{\"id\":\"gc:400\",\"kind\":\"gc\",\"channel_id\":\"400\",\"guild_id\":\"10\"},"
    "\"participants\":[\"300\",{\"user_id\":\"301\"},{\"id\":\"302\"},\"not-an-id\",null]}";
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", odd, &ev, &err) == CCORD_OK);
  inst = &ev.ev.activity_instance;
  CT_CHECK(inst->location_id == 400 && inst->participant_count == 3);
  CT_CHECK(inst->participants[0] == 300 && inst->participants[1] == 301 && inst->participants[2] == 302);
  ccord_live_event_free(&ev);
  const char *no_location =
    "{\"instance_id\":\"7003\",\"channel_id\":\"410\",\"guild_id\":\"10\",\"participants\":[\"300\"]}";
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", no_location, &ev, &err) == CCORD_OK);
  inst = &ev.ev.activity_instance;
  CT_CHECK(inst->channel_id == 410 && inst->guild_id == 10 && inst->location_id == 410);
  CT_CHECK(inst->application_id == CCORD_ID_NONE && inst->participant_count == 1);
  ccord_live_event_free(&ev);

  // The participant list is bounded by CCORD_ACTIVITY_MAX_PARTICIPANTS.
  char many[2048];
  size_t off = 0;
  int n = snprintf(many + off, sizeof many - off,
                   "{\"instance_id\":\"7004\",\"location\":{\"channel_id\":\"400\",\"guild_id\":\"10\"},"
                   "\"participants\":[");
  CT_CHECK(n > 0 && (size_t)n < sizeof many - off);
  off += (size_t)n;
  for (unsigned i = 0; i < CCORD_ACTIVITY_MAX_PARTICIPANTS + 5u; i++) {
    n = snprintf(many + off, sizeof many - off, "%s\"%u\"", i ? "," : "", 300u + i);
    CT_CHECK(n > 0 && (size_t)n < sizeof many - off);
    off += (size_t)n;
  }
  n = snprintf(many + off, sizeof many - off, "]}");
  CT_CHECK(n > 0 && (size_t)n < sizeof many - off);
  off += (size_t)n;
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", many, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.activity_instance.participant_count == CCORD_ACTIVITY_MAX_PARTICIPANTS);
  CT_CHECK(ev.ev.activity_instance.participants[CCORD_ACTIVITY_MAX_PARTICIPANTS - 1u] ==
           300u + CCORD_ACTIVITY_MAX_PARTICIPANTS - 1u);
  ccord_live_event_free(&ev);

  // Without an instance id there is nothing to upsert or end: the payload is rejected, not half-decoded.
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", "{\"application_id\":\"9001\"}", &ev, &err) ==
           CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
  CT_CHECK(decode_dispatch("EMBEDDED_ACTIVITY_UPDATE_V2", "[]", &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
}

// ---------------------------------------------------------------- components v2

// Every v2 kind in one message: Container (with a nested Action Row holding a button and a string select),
// Section with a Thumbnail accessory, Text Display, Media Gallery, File and Separator.
static const char *V2_FULL_MSG =
  "{\"id\":\"600\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"bot\",\"bot\":true},"
  "\"content\":\"\",\"flags\":32768,"
  "\"attachments\":[{\"id\":\"7\",\"filename\":\"doc.pdf\","
  "\"url\":\"https://cdn.discordapp.com/attachments/400/7/doc.pdf\",\"size\":1234,"
  "\"content_type\":\"application/pdf\"}],"
  "\"components\":["
  "{\"type\":17,\"accent_color\":5793266,\"spoiler\":false,\"components\":["
  "{\"type\":10,\"content\":\"Container text\"},"
  "{\"type\":14,\"divider\":false,\"spacing\":2},"
  "{\"type\":1,\"components\":["
  "{\"type\":2,\"style\":1,\"custom_id\":\"v2_btn\",\"label\":\"Click\",\"emoji\":{\"name\":\"\xf0\x9f\x91\x8d\"}},"
  "{\"type\":3,\"custom_id\":\"v2_sel\",\"placeholder\":\"Pick one\",\"min_values\":1,\"max_values\":1,"
  "\"options\":[{\"label\":\"One\",\"value\":\"1\",\"description\":\"first\",\"default\":true},"
  "{\"label\":\"Two\",\"value\":\"2\",\"emoji\":{\"name\":\"two\",\"id\":\"99\"}}]}]}]},"
  "{\"type\":9,\"components\":[{\"type\":10,\"content\":\"# Section heading\"},"
  "{\"type\":10,\"content\":\"Section body\"}],"
  "\"accessory\":{\"type\":11,\"media\":{\"url\":\"https://cdn.discordapp.com/thumb.png\","
  "\"proxy_url\":\"https://media.discordapp.net/thumb.png\",\"width\":64,\"height\":64,"
  "\"content_type\":\"image/png\"},\"description\":\"alt text\",\"spoiler\":true}},"
  "{\"type\":10,\"content\":\"# Hello v2\"},"
  "{\"type\":12,\"items\":["
  "{\"media\":{\"url\":\"https://cdn.discordapp.com/a.png\",\"proxy_url\":\"https://media.discordapp.net/a.png\","
  "\"width\":800,\"height\":600,\"content_type\":\"image/png\"},\"description\":\"first image\",\"spoiler\":false},"
  "{\"media\":{\"url\":\"attachment://doc.pdf\"},\"description\":\"the doc\",\"spoiler\":true}]},"
  "{\"type\":13,\"file\":{\"url\":\"attachment://doc.pdf\"},\"spoiler\":true,\"name\":\"doc.pdf\",\"size\":1234},"
  "{\"type\":14}]}";

static void test_components_v2_full(void)
{
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", V2_FULL_MSG, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE && ev.message_count == 1);
  const ccord_message_rich *r = ev.messages[0].rich;
  CT_CHECK(r != nullptr);
  if (!r) { ccord_live_event_free(&ev); return; }
  CT_CHECK((r->flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0);
  CT_CHECK((r->present & CCORD_RICH_HAS_COMPONENTS) != 0);
  CT_CHECK(r->component_row_count == 0);          // the tree replaces the v1 rows
  CT_CHECK(r->v2_component_count == 6);
  if (r->v2_component_count != 6) { ccord_live_event_free(&ev); return; }
  const ccord_component *top = r->v2_components;
  CT_CHECK(top[0].kind == CCORD_COMP_CONTAINER);
  CT_CHECK(top[1].kind == CCORD_COMP_SECTION);
  CT_CHECK(top[2].kind == CCORD_COMP_TEXT_DISPLAY);
  CT_CHECK(top[3].kind == CCORD_COMP_MEDIA_GALLERY);
  CT_CHECK(top[4].kind == CCORD_COMP_FILE);
  CT_CHECK(top[5].kind == CCORD_COMP_SEPARATOR);

  // Container: accent colour, no spoiler, three children (text, separator, action row).
  const ccord_component *cont = &top[0];
  CT_CHECK(cont->has_accent_color && cont->accent_color == 5793266u);
  CT_CHECK(!cont->spoiler);
  CT_CHECK(cont->child_count == 3);
  if (cont->child_count == 3) {
    CT_CHECK(cont->children[0].kind == CCORD_COMP_TEXT_DISPLAY);
    CT_CHECK(strcmp(cont->children[0].content, "Container text") == 0);
    CT_CHECK(cont->children[1].kind == CCORD_COMP_SEPARATOR);
    CT_CHECK(!cont->children[1].divider && cont->children[1].spacing == 2);
    const ccord_component *row = &cont->children[2];
    CT_CHECK(row->kind == CCORD_COMP_ACTION_ROW && row->child_count == 2);
    if (row->child_count == 2) {
      const ccord_component *btn = &row->children[0];
      CT_CHECK(btn->kind == CCORD_COMP_BUTTON && btn->style == CCORD_BTN_PRIMARY);
      CT_CHECK(strcmp(btn->custom_id, "v2_btn") == 0);
      CT_CHECK(strcmp(btn->label, "Click") == 0);
      CT_CHECK(strcmp(btn->emoji, "\xf0\x9f\x91\x8d") == 0);
      CT_CHECK(!btn->disabled);
      const ccord_component *sel = &row->children[1];
      CT_CHECK(sel->kind == CCORD_COMP_STRING_SELECT);
      CT_CHECK(strcmp(sel->custom_id, "v2_sel") == 0);
      CT_CHECK(strcmp(sel->placeholder, "Pick one") == 0);
      CT_CHECK(sel->min_values == 1 && sel->max_values == 1);
      CT_CHECK(sel->option_count == 2);
      if (sel->option_count == 2) {
        CT_CHECK(strcmp(sel->options[0].label, "One") == 0);
        CT_CHECK(strcmp(sel->options[0].value, "1") == 0);
        CT_CHECK(strcmp(sel->options[0].description, "first") == 0);
        CT_CHECK(sel->options[0].default_);
        CT_CHECK(strcmp(sel->options[1].label, "Two") == 0);
        CT_CHECK(strcmp(sel->options[1].emoji, "two") == 0);
        CT_CHECK(sel->options[1].emoji_id == 99);
        CT_CHECK(!sel->options[1].default_);
      }
    }
  }

  // Section: two texts and a thumbnail accessory with its media, alt text and spoiler.
  const ccord_component *sec = &top[1];
  CT_CHECK(sec->child_count == 2);
  if (sec->child_count == 2) {
    CT_CHECK(strcmp(sec->children[0].content, "# Section heading") == 0);
    CT_CHECK(strcmp(sec->children[1].content, "Section body") == 0);
  }
  CT_CHECK(sec->accessory != nullptr);
  if (sec->accessory) {
    CT_CHECK(sec->accessory->kind == CCORD_COMP_THUMBNAIL);
    const ccord_media_ref *m = &sec->accessory->media;
    CT_CHECK(strcmp(m->url, "https://cdn.discordapp.com/thumb.png") == 0);
    CT_CHECK(strcmp(m->proxy_url, "https://media.discordapp.net/thumb.png") == 0);
    CT_CHECK(m->width == 64 && m->height == 64);
    CT_CHECK(strcmp(m->content_type, "image/png") == 0);
    CT_CHECK(strcmp(m->description, "alt text") == 0);
    CT_CHECK(m->spoiler);
  }

  CT_CHECK(strcmp(top[2].content, "# Hello v2") == 0);

  // Gallery: two items, the second an attachment reference.
  const ccord_component *gal = &top[3];
  CT_CHECK(gal->gallery_count == 2);
  if (gal->gallery_count == 2) {
    CT_CHECK(strcmp(gal->gallery[0].url, "https://cdn.discordapp.com/a.png") == 0);
    CT_CHECK(strcmp(gal->gallery[0].proxy_url, "https://media.discordapp.net/a.png") == 0);
    CT_CHECK(gal->gallery[0].width == 800 && gal->gallery[0].height == 600);
    CT_CHECK(strcmp(gal->gallery[0].content_type, "image/png") == 0);
    CT_CHECK(strcmp(gal->gallery[0].description, "first image") == 0);
    CT_CHECK(!gal->gallery[0].spoiler);
    CT_CHECK(strcmp(gal->gallery[1].url, "attachment://doc.pdf") == 0);
    CT_CHECK(strcmp(gal->gallery[1].description, "the doc") == 0);
    CT_CHECK(gal->gallery[1].spoiler);
  }

  // File: attachment reference resolved through the message's attachments.
  const ccord_component *file = &top[4];
  CT_CHECK(strcmp(file->media.url, "attachment://doc.pdf") == 0);
  CT_CHECK(file->spoiler);
  CT_CHECK(strcmp(file->file_name, "doc.pdf") == 0);
  CT_CHECK(file->file_size == 1234);
  CT_CHECK(strcmp(ccord_rich_media_url(r, &file->media),
                  "https://cdn.discordapp.com/attachments/400/7/doc.pdf") == 0);

  // A separator without fields keeps Discord's defaults: a small visible divider.
  CT_CHECK(top[5].divider && top[5].spacing == 1);

  // The parsed tree satisfies the model contract, and interaction lookup finds the nested button.
  CT_CHECK(ccord_message_rich_validate(r, &err) == CCORD_OK);
  const ccord_component *found = ccord_rich_find_component(r, "v2_btn");
  CT_CHECK(found && found->kind == CCORD_COMP_BUTTON);
  CT_CHECK(ccord_rich_find_component(r, "v2_sel") != nullptr);
  ccord_live_event_free(&ev);

  // The REST path decodes the same payload identically.
  CT_CHECK(ccord_live_json_message(V2_FULL_MSG, strlen(V2_FULL_MSG), CCORD_NET_MESSAGE_CREATE, &ev, &err) ==
           CCORD_OK);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->v2_component_count == 6);
  ccord_live_event_free(&ev);
}

static void test_components_v2_detection_and_rules(void)
{
  ccord_live_event ev;
  ccord_error err;

  // v2 kinds are detected even without the flag.
  const char *no_flag =
    "{\"id\":\"601\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
    "\"components\":[{\"type\":10,\"content\":\"no flag\"}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", no_flag, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->v2_component_count == 1);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->component_row_count == 0);
  CT_CHECK(ev.messages[0].rich && strcmp(ev.messages[0].rich->v2_components[0].content, "no flag") == 0);
  ccord_live_event_free(&ev);

  // The flag alone makes even an action-row-only payload a v2 tree.
  const char *flag_rows =
    "{\"id\":\"602\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"flags\":32768,"
    "\"components\":[{\"type\":1,\"components\":[{\"type\":2,\"style\":2,\"custom_id\":\"b\",\"label\":\"B\"}]}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", flag_rows, &ev, &err) == CCORD_OK);
  const ccord_message_rich *fr = ev.messages[0].rich;
  CT_CHECK(fr && fr->v2_component_count == 1 && fr->component_row_count == 0);
  CT_CHECK(fr && fr->v2_components[0].kind == CCORD_COMP_ACTION_ROW);
  CT_CHECK(fr && fr->v2_components[0].child_count == 1);
  CT_CHECK(fr && ccord_message_rich_validate(fr, &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // Unknown and modal-only kinds are skipped; misplaced nodes never enter the tree.
  const char *rules =
    "{\"id\":\"603\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"flags\":32768,"
    "\"components\":["
    "{\"type\":99,\"content\":\"from the future\"},"
    "{\"type\":18,\"label\":\"modal label\",\"component\":{\"type\":4,\"custom_id\":\"x\"}},"
    "{\"type\":11,\"media\":{\"url\":\"https://cdn.discordapp.com/top.png\"}},"   // thumbnail outside a section
    "{\"type\":17,\"components\":["
    "{\"type\":17,\"components\":[{\"type\":10,\"content\":\"nested container\"}]},"  // containers do not nest
    "{\"type\":2,\"custom_id\":\"loose\",\"label\":\"Loose\"},"                    // button outside an action row
    "{\"type\":10,\"content\":\"in\"}]},"
    "{\"type\":9,\"components\":[{\"type\":10,\"content\":\"no accessory\"}]},"    // section needs an accessory
    "{\"type\":12,\"items\":[{\"media\":{}}]},"                                   // gallery item without a url
    "{\"type\":13,\"file\":{},\"name\":\"x\"},"                                   // file without a url
    "{\"type\":14,\"divider\":false,\"spacing\":7},"                             // spacing clamps to small
    "{\"type\":10,\"content\":\"kept\"}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", rules, &ev, &err) == CCORD_OK);
  const ccord_message_rich *rr = ev.messages[0].rich;
  CT_CHECK(rr && rr->v2_component_count == 3);
  if (rr && rr->v2_component_count == 3) {
    CT_CHECK(rr->v2_components[0].kind == CCORD_COMP_CONTAINER);
    CT_CHECK(rr->v2_components[0].child_count == 1);
    CT_CHECK(strcmp(rr->v2_components[0].children[0].content, "in") == 0);
    CT_CHECK(rr->v2_components[1].kind == CCORD_COMP_SEPARATOR);
    CT_CHECK(!rr->v2_components[1].divider && rr->v2_components[1].spacing == 1);
    CT_CHECK(rr->v2_components[2].kind == CCORD_COMP_TEXT_DISPLAY);
    CT_CHECK(strcmp(rr->v2_components[2].content, "kept") == 0);
    CT_CHECK(ccord_message_rich_validate(rr, &err) == CCORD_OK);
  }
  ccord_live_event_free(&ev);

  // A partial MESSAGE_UPDATE carrying components parses the tree and marks the part present.
  const char *upd =
    "{\"id\":\"600\",\"channel_id\":\"400\",\"flags\":32768,"
    "\"components\":[{\"type\":10,\"content\":\"updated\"}]}";
  CT_CHECK(decode_dispatch("MESSAGE_UPDATE", upd, &ev, &err) == CCORD_OK);
  const ccord_message_rich *ur = ev.messages[0].rich;
  CT_CHECK(ur && (ur->present & CCORD_RICH_HAS_COMPONENTS) != 0);
  CT_CHECK(ur && ur->text_absent);
  CT_CHECK(ur && ur->v2_component_count == 1);
  CT_CHECK(ur && strcmp(ur->v2_components[0].content, "updated") == 0);
  ccord_live_event_free(&ev);
}

static void test_components_v2_budget(void)
{
  ccord_live_event ev;
  ccord_error err;
  char big[8192];

  // More top-level nodes than the budget: the tree truncates at CCORD_RICH_MAX_V2_NODES.
  size_t off = 0;
  int n = snprintf(big + off, sizeof big - off,
                   "{\"id\":\"610\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
                   "\"flags\":32768,\"components\":[");
  CT_CHECK(n > 0);
  off += (size_t)n;
  for (unsigned i = 0; i < CCORD_RICH_MAX_V2_NODES + 5u; i++) {
    n = snprintf(big + off, sizeof big - off, "%s{\"type\":10,\"content\":\"t%u\"}", i ? "," : "", i);
    CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
    off += (size_t)n;
  }
  n = snprintf(big + off, sizeof big - off, "]}");
  CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
  off += (size_t)n;
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", big, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->v2_component_count == CCORD_RICH_MAX_V2_NODES);
  CT_CHECK(ev.messages[0].rich && ccord_message_rich_validate(ev.messages[0].rich, &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // A dropped node gives back every slot it took: a section whose texts are all invalid is skipped, accessory included.
  off = 0;
  n = snprintf(big + off, sizeof big - off,
               "{\"id\":\"613\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
               "\"flags\":32768,\"components\":[{\"type\":9,\"components\":[{\"type\":14}],"
               "\"accessory\":{\"type\":2,\"style\":1,\"custom_id\":\"acc\",\"label\":\"A\"}}");
  CT_CHECK(n > 0);
  off += (size_t)n;
  for (unsigned i = 0; i < CCORD_RICH_MAX_V2_NODES; i++) {
    n = snprintf(big + off, sizeof big - off, ",{\"type\":10,\"content\":\"g%u\"}", i);
    CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
    off += (size_t)n;
  }
  n = snprintf(big + off, sizeof big - off, "]}");
  CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", big, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->v2_component_count == CCORD_RICH_MAX_V2_NODES);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->v2_components[0].kind == CCORD_COMP_TEXT_DISPLAY);
  ccord_live_event_free(&ev);

  // A container full of texts keeps its own slot: container + (budget - 1) children.
  off = 0;
  n = snprintf(big + off, sizeof big - off,
               "{\"id\":\"611\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},"
               "\"flags\":32768,\"components\":[{\"type\":17,\"components\":[");
  CT_CHECK(n > 0);
  off += (size_t)n;
  for (unsigned i = 0; i < CCORD_RICH_MAX_V2_NODES + 5u; i++) {
    n = snprintf(big + off, sizeof big - off, "%s{\"type\":10,\"content\":\"c%u\"}", i ? "," : "", i);
    CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
    off += (size_t)n;
  }
  n = snprintf(big + off, sizeof big - off, "]}]}");
  CT_CHECK(n > 0 && (size_t)n < sizeof big - off);
  off += (size_t)n;
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", big, &ev, &err) == CCORD_OK);
  const ccord_message_rich *cr = ev.messages[0].rich;
  CT_CHECK(cr && cr->v2_component_count == 1);
  CT_CHECK(cr && cr->v2_components[0].kind == CCORD_COMP_CONTAINER);
  CT_CHECK(cr && cr->v2_components[0].child_count == CCORD_RICH_MAX_V2_NODES - 1u);
  CT_CHECK(cr && ccord_message_rich_validate(cr, &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // Gallery items cap at 10; a section keeps at most 3 texts.
  const char *caps =
    "{\"id\":\"612\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"flags\":32768,"
    "\"components\":[{\"type\":12,\"items\":["
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/0.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/1.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/2.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/3.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/4.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/5.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/6.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/7.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/8.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/9.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/10.png\"}},"
    "{\"media\":{\"url\":\"https://cdn.discordapp.com/11.png\"}}]},"
    "{\"type\":9,\"components\":[{\"type\":10,\"content\":\"a\"},{\"type\":10,\"content\":\"b\"},"
    "{\"type\":10,\"content\":\"c\"},{\"type\":10,\"content\":\"d\"}],"
    "\"accessory\":{\"type\":2,\"style\":1,\"custom_id\":\"acc\",\"label\":\"A\"}}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", caps, &ev, &err) == CCORD_OK);
  const ccord_message_rich *gr = ev.messages[0].rich;
  CT_CHECK(gr && gr->v2_component_count == 2);
  CT_CHECK(gr && gr->v2_components[0].gallery_count == CCORD_RICH_MAX_V2_GALLERY_ITEMS);
  CT_CHECK(gr && gr->v2_components[1].child_count == CCORD_RICH_MAX_V2_SECTION_TEXTS);
  CT_CHECK(gr && gr->v2_components[1].accessory && gr->v2_components[1].accessory->kind == CCORD_COMP_BUTTON);
  CT_CHECK(gr && ccord_message_rich_validate(gr, &err) == CCORD_OK);
  ccord_live_event_free(&ev);
}

// The v1 path is untouched: rows of buttons and selects, with the same drops and defaults as before.
static void test_components_v1_unchanged(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *v1 =
    "{\"id\":\"620\",\"channel_id\":\"400\",\"author\":{\"id\":\"300\",\"username\":\"ann\"},\"content\":\"pick\","
    "\"components\":["
    "{\"type\":1,\"components\":["
    "{\"type\":2,\"style\":1,\"custom_id\":\"yes\",\"label\":\"Accept\"},"
    "{\"type\":2,\"style\":5,\"label\":\"Open\",\"url\":\"https://example.com\"},"
    "{\"type\":2,\"style\":4,\"custom_id\":\"no\",\"label\":\"Decline\",\"disabled\":true},"
    "{\"type\":4,\"custom_id\":\"modal_only\",\"label\":\"Text\"},"
    "{\"type\":2,\"style\":2,\"label\":\"No id\"}]},"
    "{\"type\":1,\"components\":["
    "{\"type\":3,\"custom_id\":\"pick\",\"placeholder\":\"Choose\",\"min_values\":0,\"max_values\":3,"
    "\"options\":[{\"label\":\"A\",\"value\":\"a\",\"default\":true},"
    "{\"label\":\"B\",\"value\":\"b\",\"description\":\"bee\"}]},"
    "{\"type\":5,\"custom_id\":\"who\"}]},"
    "{\"type\":1,\"components\":["
    "{\"type\":2,\"style\":5,\"label\":\"No url\"},"
    "{\"type\":2,\"custom_id\":\"plain\",\"label\":\"Plain\"}]},"
    "{\"type\":1,\"components\":[]},"
    "{\"type\":99},"
    "{\"type\":1,\"components\":[{\"type\":2,\"custom_id\":\"late\",\"label\":\"Late\"}]},"
    "{\"type\":1,\"components\":[{\"type\":2,\"custom_id\":\"later\",\"label\":\"Later\"}]}]}";
  CT_CHECK(decode_dispatch("MESSAGE_CREATE", v1, &ev, &err) == CCORD_OK);
  const ccord_message_rich *r = ev.messages[0].rich;
  CT_CHECK(r != nullptr);
  if (!r) { ccord_live_event_free(&ev); return; }
  CT_CHECK(r->v2_component_count == 0);
  CT_CHECK(r->component_row_count == 3);   // empty/unknown rows dropped; only the first 5 rows read
  if (r->component_row_count != 3) { ccord_live_event_free(&ev); return; }
  const ccord_component_row *rows = r->components;
  CT_CHECK(rows[0].component_count == 3);
  if (rows[0].component_count == 3) {
    CT_CHECK(rows[0].components[0].kind == CCORD_COMP_BUTTON);
    CT_CHECK(rows[0].components[0].style == CCORD_BTN_PRIMARY);
    CT_CHECK(strcmp(rows[0].components[0].custom_id, "yes") == 0);
    CT_CHECK(strcmp(rows[0].components[0].label, "Accept") == 0);
    CT_CHECK(rows[0].components[1].style == CCORD_BTN_LINK);
    CT_CHECK(strcmp(rows[0].components[1].url, "https://example.com") == 0);
    CT_CHECK(rows[0].components[2].disabled);
  }
  CT_CHECK(rows[1].component_count == 2);
  if (rows[1].component_count == 2) {
    const ccord_component *sel = &rows[1].components[0];
    CT_CHECK(sel->kind == CCORD_COMP_STRING_SELECT);
    CT_CHECK(strcmp(sel->custom_id, "pick") == 0);
    CT_CHECK(strcmp(sel->placeholder, "Choose") == 0);
    CT_CHECK(sel->min_values == 0 && sel->max_values == 3);
    CT_CHECK(sel->option_count == 2);
    if (sel->option_count == 2) {
      CT_CHECK(sel->options[0].default_ && strcmp(sel->options[0].value, "a") == 0);
      CT_CHECK(strcmp(sel->options[1].description, "bee") == 0);
    }
    CT_CHECK(rows[1].components[1].kind == CCORD_COMP_USER_SELECT);
    CT_CHECK(rows[1].components[1].min_values == 1 && rows[1].components[1].max_values == 1);
  }
  CT_CHECK(rows[2].component_count == 1);
  if (rows[2].component_count == 1) {
    CT_CHECK(strcmp(rows[2].components[0].custom_id, "plain") == 0);
    CT_CHECK(rows[2].components[0].style == CCORD_BTN_SECONDARY);   // the default when style is absent
  }
  CT_CHECK(ccord_message_rich_validate(r, &err) == CCORD_OK);
  ccord_live_event_free(&ev);
}

// The mock accepts a v2 rich struct: validation runs through rich.c and the clone keeps the whole tree.
static void test_mock_accepts_components_v2(void)
{
  ccord_error err;
  ccord_mock_config cfg = {.seed = 42, .latency_ms = 0};
  ccord_transport *t = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  for (int i = 0; i < 8; i++) {
    if (!ccord_transport_poll(t, &ev)) break;   // drain the connect snapshot
  }

  ccord_component texts[2] = {
    {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "Mock v2 heading"},
    {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "Mock v2 body"},
  };
  ccord_component thumb = {.kind = CCORD_COMP_THUMBNAIL,
                           .media = {.url = "https://cdn.discordapp.com/m.png"}};
  ccord_component section = {.kind = CCORD_COMP_SECTION, .children = texts, .child_count = 2, .accessory = &thumb};
  ccord_component btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .custom_id = "mock_btn",
                         .label = "Go"};
  ccord_component row = {.kind = CCORD_COMP_ACTION_ROW, .children = &btn, .child_count = 1};
  ccord_component top[2] = {section, row};
  ccord_message_rich rich = {0};
  rich.present = CCORD_RICH_HAS_TYPE | CCORD_RICH_HAS_COMPONENTS;
  rich.flags = CCORD_MSGFLAG_IS_COMPONENTS_V2;
  rich.v2_components = top;
  rich.v2_component_count = 2;

  ccord_id mid = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &rich, &mid, &err) == CCORD_OK);
  CT_CHECK(mid != CCORD_ID_NONE);
  bool seen = false;
  for (int i = 0; i < 16 && ccord_transport_poll(t, &ev); i++) {
    if (ev.type != CCORD_NET_MESSAGE_CREATE || ev.message_count != 1 || ev.messages[0].id != mid) continue;
    seen = true;
    const ccord_message_rich *r = ev.messages[0].rich;
    CT_CHECK(r && (r->flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0);
    CT_CHECK(r && r->v2_component_count == 2 && r->component_row_count == 0);
    if (r && r->v2_component_count == 2) {
      CT_CHECK(r->v2_components[0].kind == CCORD_COMP_SECTION);
      CT_CHECK(r->v2_components[0].child_count == 2);
      CT_CHECK(strcmp(r->v2_components[0].children[0].content, "Mock v2 heading") == 0);
      CT_CHECK(r->v2_components[0].accessory &&
               strcmp(r->v2_components[0].accessory->media.url, "https://cdn.discordapp.com/m.png") == 0);
      CT_CHECK(ccord_rich_find_component(r, "mock_btn") != nullptr);
      CT_CHECK(ccord_message_rich_validate(r, &err) == CCORD_OK);
    }
    break;
  }
  CT_CHECK(seen);

  // An invalid tree (section without accessory) is refused by the same validation.
  ccord_component bad_section = {.kind = CCORD_COMP_SECTION, .children = texts, .child_count = 2};
  ccord_message_rich bad = {0};
  bad.present = CCORD_RICH_HAS_COMPONENTS;
  bad.flags = CCORD_MSGFLAG_IS_COMPONENTS_V2;
  bad.v2_components = &bad_section;
  bad.v2_component_count = 1;
  CT_CHECK(ccord_mock_post_rich_message(t, 3111, 1011, "", &bad, &mid, &err) == CCORD_ERR_INVALID);
  ccord_transport_destroy(t);
}

int main(void)
{
  test_message_tts();
  test_message_voice_waveform();
  test_message_forward_snapshot();
  test_message_forward_reference_only();
  test_slash_subcommand_tree();
  test_slash_context_commands();
  test_slash_malformed_tree_and_cap();
  test_stage_instance_extended();
  test_scheduled_event_rest_single();
  test_scheduled_event_rest_list();
  test_scheduled_event_gateway();
  test_activity_shelf_rest();
  test_activity_instance_gateway();
  test_components_v2_full();
  test_components_v2_detection_and_rules();
  test_components_v2_budget();
  test_components_v1_unchanged();
  test_mock_accepts_components_v2();
  return CT_RESULT("test_discord_json");
}
