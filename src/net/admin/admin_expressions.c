// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "admin_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>
#include <ccord/model/permissions.h>

// ---------------------------------------------------------------------------------------------------------------
// Guild expressions: emoji, stickers, soundboard (M4b)
// ---------------------------------------------------------------------------------------------------------------

static ccord_status check_emoji_name(const char *name, ccord_error *err)
{
  if (!name) return ccord_error_set(err, CCORD_ERR_INVALID, "Emoji name is required");
  size_t n = strlen(name);
  if (n < CCORD_ADMIN_EMOJI_NAME_MIN) return ccord_error_set(err, CCORD_ERR_INVALID, "Emoji name is too short");
  if (n > CCORD_ADMIN_EMOJI_NAME_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Emoji name is too long (max %u)", CCORD_ADMIN_EMOJI_NAME_MAX);
  }
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (!ok) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "Emoji name may only use letters, numbers, and underscores");
    }
  }
  return CCORD_OK;
}

static ccord_status check_nonempty_data_uri(const char *what, const char *prefix, const char *uri, ccord_error *err)
{
  if (!uri || uri[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "%s is required", what);
  size_t plen = strlen(prefix);
  const char *b64 = strncmp(uri, prefix, plen) == 0 ? strstr(uri + plen, ";base64,") : nullptr;
  if (!b64 || b64[8] == '\0' || admin_text_chars(uri) < 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "bad %s data URI", what);
  }
  return CCORD_OK;
}

ccord_status ccord_admin_list_emojis(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_EMOJIS, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/emojis", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

typedef struct create_emoji_args {
  const char *name;
  const char *image;
} create_emoji_args;

static void fill_create_emoji(ccord_jw *w, const void *p)
{
  const create_emoji_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_key(w, "image");
  ccord_jw_cstr(w, a->image);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_create_emoji(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                      const char *image_data_uri, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_EMOJI, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = check_emoji_name(name, err)) != CCORD_OK) return st;
  if ((st = check_nonempty_data_uri("emoji image", "data:image/", image_data_uri, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  create_emoji_args a = {.name = name, .image = image_data_uri};
  if ((st = admin_set_body(out, fill_create_emoji, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/emojis", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->emoji_animated = strncmp(image_data_uri, "data:image/gif;", 15) == 0;
  admin_set_name(out, name);
  return CCORD_OK;
}

static void fill_edit_emoji(ccord_jw *w, const void *p)
{
  const admin_name_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_emoji(ccord_admin_request *out, ccord_id guild_id, ccord_id emoji_id, const char *name,
                                    const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_EMOJI, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(emoji_id, "emoji", err)) != CCORD_OK) return st;
  if ((st = check_emoji_name(name, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  admin_name_args a = {.name = name};
  if ((st = admin_set_body(out, fill_edit_emoji, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/emojis/%llu", (unsigned long long)guild_id,
                     (unsigned long long)emoji_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = emoji_id;
  admin_set_name(out, name);
  return CCORD_OK;
}

ccord_status ccord_admin_delete_emoji(ccord_admin_request *out, ccord_id guild_id, ccord_id emoji_id,
                                      const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_EMOJI, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(emoji_id, "emoji", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/emojis/%llu", (unsigned long long)guild_id,
                     (unsigned long long)emoji_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = emoji_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_stickers(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_STICKERS, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/stickers", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

static bool append_form_field(ccord_buf *b, const char *boundary, const char *field_name, const char *value)
{
  return ccord_buf_append_str(b, "--") == CCORD_OK &&
         ccord_buf_append_str(b, boundary) == CCORD_OK &&
         ccord_buf_append_str(b, "\r\nContent-Disposition: form-data; name=\"") == CCORD_OK &&
         ccord_buf_append_str(b, field_name) == CCORD_OK &&
         ccord_buf_append_str(b, "\"\r\n\r\n") == CCORD_OK &&
         ccord_buf_append_str(b, value) == CCORD_OK &&
         ccord_buf_append_str(b, "\r\n") == CCORD_OK;
}

static ccord_status check_sticker_desc(const char *desc, ccord_error *err)
{
  if (!desc || desc[0] == '\0') return CCORD_OK;
  long n = admin_text_chars(desc);
  if (n < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Sticker description has invalid characters");
  if ((size_t)n > CCORD_ADMIN_STICKER_DESC_MAX || strlen(desc) >= 256u) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker description is too long (max %u)",
                           CCORD_ADMIN_STICKER_DESC_MAX);
  }
  return CCORD_OK;
}

static ccord_status check_sticker_tags(const char *tags, ccord_error *err)
{
  ccord_status st = admin_check_name("Sticker tags", tags, 1, CCORD_ADMIN_STICKER_TAGS_MAX, err);
  if (st != CCORD_OK) return st;
  if (strlen(tags) > CCORD_ADMIN_STICKER_TAGS_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker tags are too long");
  }
  return CCORD_OK;
}

ccord_status ccord_admin_create_sticker(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                        const char *description, const char *tags, const void *file_data,
                                        size_t file_size, const char *mime_type, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_STICKER, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_check_name("Sticker name", name, CCORD_ADMIN_STICKER_NAME_MIN, CCORD_ADMIN_STICKER_NAME_MAX, err)) != CCORD_OK) {
    return st;
  }
  if (strlen(name) > CCORD_NAME_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker name is too long");
  if (!description) description = "";
  if ((st = check_sticker_desc(description, err)) != CCORD_OK) return st;
  if ((st = check_sticker_tags(tags, err)) != CCORD_OK) return st;
  if (!file_data || file_size == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Sticker file is required");
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;

  const char *fname = nullptr;
  ccord_sticker_format fmt = CCORD_STICKER_PNG;
  if (!mime_type) return ccord_error_set(err, CCORD_ERR_INVALID, "unsupported sticker format");
  if (strcmp(mime_type, "image/png") == 0) {
    fname = "sticker.png";
    fmt = CCORD_STICKER_PNG;
  } else if (strcmp(mime_type, "image/apng") == 0) {
    fname = "sticker.png";
    fmt = CCORD_STICKER_APNG;
  } else if (strcmp(mime_type, "image/gif") == 0) {
    fname = "sticker.gif";
    fmt = CCORD_STICKER_GIF;
  } else if (strcmp(mime_type, "application/json") == 0) {
    fname = "sticker.json";
    fmt = CCORD_STICKER_LOTTIE;
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "unsupported sticker format");
  }

  static const char k_boundary[] = "----CCordStickerBoundary7MA4YWxkTrZu0gW";
  if (file_size >= sizeof out->body) return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  ccord_buf b;
  ccord_buf_init(&b, file_size + 768u);
  bool ok = append_form_field(&b, k_boundary, "name", name) &&
            append_form_field(&b, k_boundary, "description", description) &&
            append_form_field(&b, k_boundary, "tags", tags) &&
            ccord_buf_append_str(&b, "--") == CCORD_OK &&
            ccord_buf_append_str(&b, k_boundary) == CCORD_OK &&
            ccord_buf_append_str(&b, "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"") == CCORD_OK &&
            ccord_buf_append_str(&b, fname) == CCORD_OK &&
            ccord_buf_append_str(&b, "\"\r\nContent-Type: ") == CCORD_OK &&
            ccord_buf_append_str(&b, mime_type) == CCORD_OK &&
            ccord_buf_append_str(&b, "\r\n\r\n") == CCORD_OK &&
            ccord_buf_append(&b, file_data, file_size) == CCORD_OK &&
            ccord_buf_append_str(&b, "\r\n--") == CCORD_OK &&
            ccord_buf_append_str(&b, k_boundary) == CCORD_OK &&
            ccord_buf_append_str(&b, "--\r\n") == CCORD_OK;
  if (!ok || b.len >= sizeof out->body) {
    ccord_buf_free(&b);
    return ccord_error_set(err, CCORD_ERR_RANGE, "request body too large");
  }
  if ((st = admin_set_path(out, err, "/guilds/%llu/stickers", (unsigned long long)guild_id)) != CCORD_OK) {
    ccord_buf_free(&b);
    return st;
  }
  memcpy(out->body, b.data, b.len);
  out->body[b.len] = '\0';
  out->body_len = b.len;
  ccord_buf_free(&b);
  (void)snprintf(out->content_type, sizeof out->content_type, "multipart/form-data; boundary=%s", k_boundary);
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->sticker_format = (uint8_t)fmt;
  out->has_sticker_desc = true;
  out->has_sticker_tags = true;
  (void)snprintf(out->sticker_desc, sizeof out->sticker_desc, "%s", description);
  (void)snprintf(out->sticker_tags, sizeof out->sticker_tags, "%s", tags);
  admin_set_name(out, name);
  return CCORD_OK;
}

static void fill_edit_sticker(ccord_jw *w, const void *p)
{
  const ccord_admin_sticker_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->description) {
    ccord_jw_key(w, "description");
    ccord_jw_cstr(w, e->description);
  }
  if (e->tags) {
    ccord_jw_key(w, "tags");
    ccord_jw_cstr(w, e->tags);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_sticker(ccord_admin_request *out, ccord_id guild_id, ccord_id sticker_id,
                                      const ccord_admin_sticker_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_STICKER, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(sticker_id, "sticker", err)) != CCORD_OK) return st;
  if (!edit->name && !edit->description && !edit->tags) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name) {
    if ((st = admin_check_name("Sticker name", edit->name, CCORD_ADMIN_STICKER_NAME_MIN, CCORD_ADMIN_STICKER_NAME_MAX, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "Sticker name is too long");
    }
  }
  if (edit->description && (st = check_sticker_desc(edit->description, err)) != CCORD_OK) return st;
  if (edit->tags && (st = check_sticker_tags(edit->tags, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_edit_sticker, edit, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/stickers/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sticker_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sticker_id;
  if (edit->name) admin_set_name(out, edit->name);
  if (edit->description) {
    out->has_sticker_desc = true;
    (void)snprintf(out->sticker_desc, sizeof out->sticker_desc, "%s", edit->description);
  }
  if (edit->tags) {
    out->has_sticker_tags = true;
    (void)snprintf(out->sticker_tags, sizeof out->sticker_tags, "%s", edit->tags);
  }
  return CCORD_OK;
}

ccord_status ccord_admin_delete_sticker(ccord_admin_request *out, ccord_id guild_id, ccord_id sticker_id,
                                        const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_STICKER, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(sticker_id, "sticker", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/stickers/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sticker_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sticker_id;
  return CCORD_OK;
}

ccord_status ccord_admin_list_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_LIST_SOUNDBOARD, CCORD_ADMIN_GET);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/soundboard-sounds", (unsigned long long)guild_id)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  return CCORD_OK;
}

typedef struct create_sound_args {
  const char *name;
  const char *sound;
  int16_t volume_pct;
  ccord_id emoji_id;
  const char *emoji_name;
} create_sound_args;

static void fill_create_soundboard(ccord_jw *w, const void *p)
{
  const create_sound_args *a = p;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "name");
  ccord_jw_cstr(w, a->name);
  ccord_jw_key(w, "sound");
  ccord_jw_cstr(w, a->sound);
  if (a->volume_pct >= 0) {
    ccord_jw_key(w, "volume");
    ccord_jw_double(w, (double)a->volume_pct / 100.0);
  }
  if (a->emoji_id != CCORD_ID_NONE) {
    ccord_jw_key(w, "emoji_id");
    ccord_jw_id(w, a->emoji_id);
  } else if (a->emoji_name && a->emoji_name[0] != '\0') {
    ccord_jw_key(w, "emoji_name");
    ccord_jw_cstr(w, a->emoji_name);
  }
  ccord_jw_end_object(w);
}

static ccord_status check_sound_emoji(ccord_id emoji_id, const char *emoji_name, ccord_error *err)
{
  (void)emoji_id;
  if (!emoji_name || emoji_name[0] == '\0') return CCORD_OK;
  if (strlen(emoji_name) > CCORD_EMOJI_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "emoji is too long");
  if (admin_text_chars(emoji_name) < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "emoji has invalid characters");
  return CCORD_OK;
}

ccord_status ccord_admin_create_soundboard(ccord_admin_request *out, ccord_id guild_id, const char *name,
                                           const char *sound_data_uri, int16_t volume_pct, ccord_id emoji_id,
                                           const char *emoji_name, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_CREATE_SOUNDBOARD, CCORD_ADMIN_POST);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_check_name("Sound name", name, CCORD_ADMIN_SOUNDBOARD_NAME_MIN, CCORD_ADMIN_SOUNDBOARD_NAME_MAX, err)) != CCORD_OK) {
    return st;
  }
  if (strlen(name) > CCORD_NAME_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "Sound name is too long");
  if ((st = check_nonempty_data_uri("sound", "data:audio/", sound_data_uri, err)) != CCORD_OK) return st;
  if (volume_pct < -1 || volume_pct > 100) return ccord_error_set(err, CCORD_ERR_RANGE, "bad sound volume");
  if ((st = check_sound_emoji(emoji_id, emoji_name, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  create_sound_args a = {
    .name = name,
    .sound = sound_data_uri,
    .volume_pct = volume_pct,
    .emoji_id = emoji_id,
    .emoji_name = emoji_name,
  };
  if ((st = admin_set_body(out, fill_create_soundboard, &a, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/soundboard-sounds", (unsigned long long)guild_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->sound_volume_pct = volume_pct >= 0 ? volume_pct : 100;
  admin_set_name(out, name);
  if (emoji_id != CCORD_ID_NONE || (emoji_name && emoji_name[0] != '\0')) {
    out->has_sound_emoji = true;
    out->sound_emoji_id = emoji_id;
    if (emoji_id == CCORD_ID_NONE && emoji_name) {
      (void)snprintf(out->sound_emoji_name, sizeof out->sound_emoji_name, "%s", emoji_name);
    }
  }
  return CCORD_OK;
}

static void fill_edit_soundboard(ccord_jw *w, const void *p)
{
  const ccord_admin_soundboard_edit *e = p;
  ccord_jw_begin_object(w);
  if (e->name) {
    ccord_jw_key(w, "name");
    ccord_jw_cstr(w, e->name);
  }
  if (e->volume_pct >= 0) {
    ccord_jw_key(w, "volume");
    ccord_jw_double(w, (double)e->volume_pct / 100.0);
  }
  if (e->has_emoji) {
    ccord_jw_key(w, "emoji_id");
    if (e->emoji_id != CCORD_ID_NONE) ccord_jw_id(w, e->emoji_id);
    else ccord_jw_null(w);
    ccord_jw_key(w, "emoji_name");
    if (e->emoji_id == CCORD_ID_NONE && e->emoji_name && e->emoji_name[0] != '\0') ccord_jw_cstr(w, e->emoji_name);
    else ccord_jw_null(w);
  }
  ccord_jw_end_object(w);
}

ccord_status ccord_admin_edit_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_id sound_id,
                                         const ccord_admin_soundboard_edit *edit, const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_EDIT_SOUNDBOARD, CCORD_ADMIN_PATCH);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(sound_id, "sound", err)) != CCORD_OK) return st;
  if (!edit->name && edit->volume_pct < 0 && !edit->has_emoji) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "nothing to change");
  }
  if (edit->name) {
    if ((st = admin_check_name("Sound name", edit->name, CCORD_ADMIN_SOUNDBOARD_NAME_MIN, CCORD_ADMIN_SOUNDBOARD_NAME_MAX, err)) != CCORD_OK) {
      return st;
    }
    if (strlen(edit->name) > CCORD_NAME_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "Sound name is too long");
  }
  if (edit->volume_pct < -1 || edit->volume_pct > 100) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "bad sound volume");
  }
  if (edit->has_emoji && (st = check_sound_emoji(edit->emoji_id, edit->emoji_name, err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_body(out, fill_edit_soundboard, edit, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/soundboard-sounds/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sound_id)) != CCORD_OK) {
    out->body[0] = '\0';
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sound_id;
  if (edit->name) admin_set_name(out, edit->name);
  out->sound_volume_pct = edit->volume_pct;
  out->has_sound_emoji = edit->has_emoji;
  if (edit->has_emoji) {
    out->sound_emoji_id = edit->emoji_id;
    if (edit->emoji_id == CCORD_ID_NONE && edit->emoji_name) {
      (void)snprintf(out->sound_emoji_name, sizeof out->sound_emoji_name, "%s", edit->emoji_name);
    }
  }
  return CCORD_OK;
}

ccord_status ccord_admin_delete_soundboard(ccord_admin_request *out, ccord_id guild_id, ccord_id sound_id,
                                           const char *reason, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "null request");
  admin_init_request(out, CCORD_ADMIN_DELETE_SOUNDBOARD, CCORD_ADMIN_DELETE);
  ccord_status st = admin_need_guild(guild_id, err);
  if (st != CCORD_OK) return st;
  if ((st = admin_need_id(sound_id, "sound", err)) != CCORD_OK) return st;
  if ((st = admin_check_reason(reason, err)) != CCORD_OK) return st;
  if ((st = admin_set_path(out, err, "/guilds/%llu/soundboard-sounds/%llu", (unsigned long long)guild_id,
                     (unsigned long long)sound_id)) != CCORD_OK) {
    return st;
  }
  if ((st = admin_set_reason(out, reason, err)) != CCORD_OK) return st;
  out->guild_id = guild_id;
  out->expr_id = sound_id;
  return CCORD_OK;
}

