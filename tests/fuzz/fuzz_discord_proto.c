// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Fuzz harness for the FrecencyUserSettings protobuf reader (discord_proto.c) and its base64 and
// settings-JSON wrappers. Every input is decoded three ways; on success the returned strings are walked
// with strlen so a missing NUL terminator reads past the arena allocation and trips ASan.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/net/discord_proto.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static volatile uint64_t sink;

static void touch_strings(const ccord_user_expressions *x)
{
  for (size_t i = 0; i < x->gif_count; i++) {
    sink += strlen(x->gifs[i].url);
    sink += strlen(x->gifs[i].src);
  }
  for (size_t i = 0; i < x->favorite_emoji_count; i++) {
    sink += strlen(x->favorite_emojis[i]);
  }
  for (size_t i = 0; i < x->sticker_frecency_count; i++) {
    sink += strlen(x->sticker_frecency[i].key);
  }
  for (size_t i = 0; i < x->emoji_frecency_count; i++) {
    sink += strlen(x->emoji_frecency[i].key);
  }
}

static void decode_raw(const uint8_t *data, size_t size)
{
  ccord_user_expressions x;
  ccord_error err;
  ccord_error_clear(&err);
  if (ccord_user_expressions_decode(data, size, &x, &err) == CCORD_OK) {
    touch_strings(&x);
  }
  ccord_user_expressions_free(&x);
}

static void decode_b64(const uint8_t *data, size_t size)
{
  char text[8193];
  if (size > 8192u) { size = 8192u; }
  memcpy(text, data, size);
  text[size] = '\0';
  ccord_user_expressions x;
  ccord_error err;
  ccord_error_clear(&err);
  if (ccord_user_expressions_decode_b64(text, size, &x, &err) == CCORD_OK) {
    touch_strings(&x);
  }
  ccord_user_expressions_free(&x);
}

// Wrap the input as {"settings":"<escaped>"} so the JSON + base64 path is exercised too. Control bytes
// become \u00XX; other bytes pass through (invalid UTF-8 is then rejected by the JSON parser, which is a
// valid outcome). Worst-case expansion is 6x, bounded by the 4096-byte input cap.
static void decode_settings_json(const uint8_t *data, size_t size)
{
  static const char hex[] = "0123456789abcdef";
  char json[32768];
  if (size > 4096u) { size = 4096u; }
  size_t n = 0;
  memcpy(json, "{\"settings\":\"", 13);
  n = 13;
  for (size_t i = 0; i < size; i++) {
    uint8_t c = data[i];
    if (c == '"' || c == '\\') {
      json[n++] = '\\';
      json[n++] = (char)c;
    } else if (c < 0x20u) {
      json[n++] = '\\'; json[n++] = 'u'; json[n++] = '0'; json[n++] = '0';
      json[n++] = hex[c >> 4];
      json[n++] = hex[c & 0x0Fu];
    } else {
      json[n++] = (char)c;
    }
  }
  json[n++] = '"';
  json[n++] = '}';
  ccord_user_expressions x;
  ccord_error err;
  ccord_error_clear(&err);
  if (ccord_user_expressions_from_settings_json(json, n, &x, &err) == CCORD_OK) {
    touch_strings(&x);
  }
  ccord_user_expressions_free(&x);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  static const uint8_t empty = 0;
  if (data == nullptr) { data = &empty; size = 0; }
  decode_raw(data, size);
  decode_b64(data, size);
  decode_settings_json(data, size);
  return 0;
}
