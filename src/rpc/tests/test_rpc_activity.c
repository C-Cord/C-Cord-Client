// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/rpc/activity.h>

#include <ccord/core/arena.h>
#include <ccord/core/json.h>

#include <stdio.h>
#include <string.h>

#include "ccord_test.h"

static ccord_arena g_arena;

static const ccord_json *parse_json(const char *text)
{
  ccord_error err = {0};
  return ccord_json_parse(&g_arena, text, strlen(text), nullptr, &err);
}

static const ccord_json *json_get(const ccord_json *obj, const char *key)
{
  return obj ? ccord_json_get(obj, key) : nullptr;
}

static const char *json_str(const ccord_json *obj, const char *key)
{
  const ccord_json *v = json_get(obj, key);
  return v && ccord_json_type_of(v) == CCORD_JSON_STRING ? ccord_json_string(v, nullptr) : nullptr;
}

static int64_t json_num(const ccord_json *obj, const char *key)
{
  const ccord_json *v = json_get(obj, key);
  int64_t out = -1;
  return v && ccord_json_int64(v, &out) ? out : -1;
}

static void put_le32(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
  p[2] = (unsigned char)((v >> 16) & 0xffu);
  p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void test_parse_and_defaults(void)
{
  ccord_rpc_activity a;
  ccord_error err = {0};
  const char *text =
    "{\"name\":\"Listening party\",\"type\":2,\"application_id\":\"123456789012345678\","
    "\"details\":\"Track\",\"state\":\"Album\","
    "\"timestamps\":{\"start\":1700000000,\"end\":1700000000000},"
    "\"assets\":{\"large_image\":\"cover\",\"large_text\":\"Cover\",\"small_image\":\"dot\",\"small_text\":\"Now\"},"
    "\"party\":{\"id\":\"party-1\",\"size\":[2,5]},"
    "\"secrets\":{\"join\":\"join-secret\",\"spectate\":\"spectate-secret\",\"match\":\"match-secret\"},"
    "\"buttons\":[{\"label\":\"Listen\",\"url\":\"https://example.test/listen\"},"
    "{\"label\":\"Site\",\"url\":\"http://example.test\"}],"
    "\"instance\":true}";
  const ccord_json *j = parse_json(text);
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_OK);
  CT_CHECK(strcmp(a.name, "Listening party") == 0);
  CT_CHECK(a.type == 2);
  CT_CHECK(strcmp(a.application_id, "123456789012345678") == 0);
  CT_CHECK(a.start_ms == 1700000000000LL);
  CT_CHECK(a.end_ms == 1700000000000LL);
  CT_CHECK(strcmp(a.large_image, "cover") == 0 && strcmp(a.small_text, "Now") == 0);
  CT_CHECK(strcmp(a.party_id, "party-1") == 0 && a.party_size_cur == 2 && a.party_size_max == 5);
  CT_CHECK(strcmp(a.secret_join, "join-secret") == 0 && strcmp(a.secret_match, "match-secret") == 0);
  CT_CHECK(a.button_count == 2);
  CT_CHECK(strcmp(a.button_labels[0], "Listen") == 0);
  CT_CHECK(strcmp(a.button_urls[1], "http://example.test") == 0);
  CT_CHECK(a.instance);

  // A payload without a name or type has an empty name (the app resolves it) / Playing and zeros every other field,
  // and its gateway JSON still carries the "Rich Presence" fallback name.
  j = parse_json("{}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_OK);
  CT_CHECK(a.name[0] == '\0');
  CT_CHECK(a.type == 0);
  CT_CHECK(a.start_ms == 0 && a.button_count == 0 && !a.instance);
  char wire[512];
  CT_CHECK(ccord_rpc_activity_to_gateway_json(&a, 1, wire, sizeof wire) == CCORD_OK);
  CT_CHECK(strstr(wire, "\"name\":\"Rich Presence\"") != nullptr);
}

static void test_rejections(void)
{
  ccord_rpc_activity a;
  ccord_error err = {0};
  const ccord_json *j = parse_json(
    "{\"name\":\"N\",\"buttons\":[{\"label\":\"a\",\"url\":\"https://a.test\"},"
    "{\"label\":\"b\",\"url\":\"https://b.test\"},{\"label\":\"c\",\"url\":\"https://c.test\"}]}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_ERR_RANGE);

  j = parse_json("{\"name\":\"N\",\"buttons\":[{\"label\":\"a\",\"url\":\"ftp://a.test\"}]}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_ERR_INVALID);

  j = parse_json("{\"name\":\"N\",\"buttons\":[{\"label\":\"a\",\"url\":\"javascript:alert(1)\"}]}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_ERR_INVALID);

  j = parse_json("{\"name\":\"N\",\"type\":4}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_ERR_INVALID);

  j = parse_json("{\"name\":\"N\",\"type\":6}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_ERR_INVALID);

  j = parse_json("{\"name\":\"N\",\"type\":\"playing\"}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_ERR_INVALID);

  // Exactly 2^63 is representable as a double but not as int64_t: reject before casting.
  j = parse_json("{\"name\":\"N\",\"timestamps\":{\"start\":9223372036854775808}}");
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_ERR_RANGE);
}

static void test_streaming(void)
{
  ccord_rpc_activity a;
  ccord_error err = {0};
  const ccord_json *in = parse_json(
    "{\"name\":\"Live\",\"type\":1,\"url\":\"https://twitch.tv/example\",\"details\":\"Ranked\",\"state\":\"Queue\"}");
  CT_CHECK(in != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(in, &a, &err) == CCORD_OK);
  CT_CHECK(a.type == 1);
  CT_CHECK(strcmp(a.url, "https://twitch.tv/example") == 0);
  CT_CHECK(strcmp(a.details, "Ranked") == 0);

  char out[1024];
  CT_CHECK(ccord_rpc_activity_to_gateway_json(&a, 1, out, sizeof out) == CCORD_OK);
  const ccord_json *root = parse_json(out);
  const ccord_json *act = ccord_json_array_at(root, 0);
  CT_CHECK(act != nullptr);
  CT_CHECK(json_num(act, "type") == 1);
  CT_CHECK(strcmp(json_str(act, "url"), "https://twitch.tv/example") == 0);
}

static void test_truncation(void)
{
  ccord_rpc_activity a;
  ccord_error err = {0};
  char raw[256];
  char text[512];

  // 200 ASCII bytes keep the first 128.
  memset(raw, 'a', 200);
  raw[200] = '\0';
  (void)snprintf(text, sizeof text, "{\"name\":\"%s\"}", raw);
  const ccord_json *j = parse_json(text);
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_OK);
  CT_CHECK(strlen(a.name) == CCORD_RPC_ACTIVITY_TEXT_MAX);

  // 127 ASCII bytes followed by two 2-byte UTF-8 characters: the 128-byte cut lands inside the first character,
  // so the whole character is dropped and the result is 127 bytes of valid UTF-8.
  memset(raw, 'a', 127);
  memcpy(raw + 127, "\xC3\xA9\xC3\xA9", 4);
  raw[131] = '\0';
  (void)snprintf(text, sizeof text, "{\"name\":\"%s\"}", raw);
  j = parse_json(text);
  CT_CHECK(j != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(j, &a, &err) == CCORD_OK);
  CT_CHECK(strlen(a.name) == 127);
  CT_CHECK(a.name[126] == 'a');
  CT_CHECK(a.name[127] == '\0');
}

static void test_gateway_json(void)
{
  ccord_rpc_activity a;
  ccord_error err = {0};
  const ccord_json *in = parse_json(
    "{\"name\":\"Game\",\"type\":0,\"application_id\":\"42\",\"details\":\"In match\",\"state\":\"Round 3\","
    "\"timestamps\":{\"start\":1700000000},\"assets\":{\"large_image\":\"map\"},"
    "\"party\":{\"id\":\"squad\",\"size\":[1,4]},"
    "\"buttons\":[{\"label\":\"Join\",\"url\":\"https://example.test/join\"}],\"instance\":true}");
  CT_CHECK(in != nullptr);
  CT_CHECK(ccord_rpc_activity_parse(in, &a, &err) == CCORD_OK);

  char out[2048];
  CT_CHECK(ccord_rpc_activity_to_gateway_json(&a, 1, out, sizeof out) == CCORD_OK);
  const ccord_json *root = parse_json(out);
  CT_CHECK(root != nullptr && ccord_json_type_of(root) == CCORD_JSON_ARRAY);
  CT_CHECK(ccord_json_array_len(root) == 1);
  const ccord_json *act = ccord_json_array_at(root, 0);
  CT_CHECK(act && ccord_json_type_of(act) == CCORD_JSON_OBJECT);
  CT_CHECK(strcmp(json_str(act, "name"), "Game") == 0);
  CT_CHECK(json_num(act, "type") == 0);
  CT_CHECK(strcmp(json_str(act, "application_id"), "42") == 0);

  const ccord_json *ts = json_get(act, "timestamps");
  CT_CHECK(ts && json_num(ts, "start") == 1700000000000LL);
  const ccord_json *assets = json_get(act, "assets");
  CT_CHECK(assets && strcmp(json_str(assets, "large_image"), "map") == 0);
  const ccord_json *party = json_get(act, "party");
  CT_CHECK(party && strcmp(json_str(party, "id"), "squad") == 0);
  const ccord_json *size = json_get(party, "size");
  CT_CHECK(size && ccord_json_array_len(size) == 2);

  const ccord_json *buttons = json_get(act, "buttons");
  CT_CHECK(buttons && ccord_json_array_len(buttons) == 1);
  CT_CHECK(buttons && strcmp(ccord_json_string(ccord_json_array_at(buttons, 0), nullptr), "Join") == 0);
  const ccord_json *metadata = json_get(act, "metadata");
  const ccord_json *urls = json_get(metadata, "button_urls");
  CT_CHECK(urls && ccord_json_array_len(urls) == 1);
  CT_CHECK(urls && strcmp(ccord_json_string(ccord_json_array_at(urls, 0), nullptr),
                          "https://example.test/join") == 0);

  const ccord_json *inst = json_get(act, "instance");
  CT_CHECK(inst && ccord_json_type_of(inst) == CCORD_JSON_BOOL && ccord_json_bool(inst, false));

  // Empty activity list serializes as [] and an undersized buffer is rejected.
  CT_CHECK(ccord_rpc_activity_to_gateway_json(nullptr, 0, out, sizeof out) == CCORD_OK);
  CT_CHECK(strcmp(out, "[]") == 0);
  char tiny[8];
  CT_CHECK(ccord_rpc_activity_to_gateway_json(&a, 1, tiny, sizeof tiny) == CCORD_ERR_RANGE);
}

static void test_frames(void)
{
  const char *payload = "{\"cmd\":\"SET_ACTIVITY\"}";
  size_t plen_in = strlen(payload);
  char frame[128];
  CT_CHECK(ccord_rpc_frame_encode(frame, sizeof frame, 0, payload, plen_in) == CCORD_OK);

  uint32_t op = 99;
  const char *decoded = nullptr;
  size_t plen = 0, consumed = 0;
  CT_CHECK(ccord_rpc_frame_decode(frame, 8 + plen_in, &op, &decoded, &plen, &consumed) == CCORD_RPC_FRAME_FRAME);
  CT_CHECK(op == 0 && plen == plen_in && consumed == 8 + plen_in);
  CT_CHECK(decoded && memcmp(decoded, payload, plen_in) == 0);

  // A header alone or a partial payload waits for more bytes.
  CT_CHECK(ccord_rpc_frame_decode(frame, 4, &op, &decoded, &plen, &consumed) == CCORD_RPC_FRAME_NEED_MORE);
  CT_CHECK(ccord_rpc_frame_decode(frame, 8 + plen_in - 2, &op, &decoded, &plen, &consumed) == CCORD_RPC_FRAME_NEED_MORE);
  CT_CHECK(ccord_rpc_frame_decode(frame, plen_in, &op, &decoded, &plen, &consumed) == CCORD_RPC_FRAME_NEED_MORE);

  // op outside 0..4 and a payload above the cap are protocol errors.
  unsigned char bad[8] = {0};
  put_le32(bad, 5);
  put_le32(bad + 4, 2);
  CT_CHECK(ccord_rpc_frame_decode((const char *)bad, sizeof bad, &op, &decoded, &plen, &consumed) == CCORD_RPC_FRAME_BAD);
  put_le32(bad, 0);
  put_le32(bad + 4, CCORD_RPC_FRAME_MAX_PAYLOAD + 1u);
  CT_CHECK(ccord_rpc_frame_decode((const char *)bad, sizeof bad, &op, &decoded, &plen, &consumed) == CCORD_RPC_FRAME_BAD);

  // Encoder enforces the same limits.
  CT_CHECK(ccord_rpc_frame_encode(frame, sizeof frame, 5, payload, plen_in) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_rpc_frame_encode(frame, sizeof frame, 0, payload, CCORD_RPC_FRAME_MAX_PAYLOAD + 1u) == CCORD_ERR_RANGE);
  CT_CHECK(ccord_rpc_frame_encode(frame, 8, 0, payload, plen_in) == CCORD_ERR_RANGE);
}

int main(void)
{
  ccord_arena_init(&g_arena, 0);
  test_parse_and_defaults();
  test_rejections();
  test_streaming();
  test_truncation();
  test_gateway_json();
  test_frames();
  ccord_arena_free(&g_arena);
  return CT_RESULT("test_rpc_activity");
}
