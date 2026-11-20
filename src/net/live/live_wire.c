// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "live_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

ccord_status live_http_status_error(long status, ccord_error *err, const char *what)
{
  if (status >= 200 && status < 300) return CCORD_OK;
  if (status == 401 || status == 403) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "rest: %s denied", what);
  }
  if (status == 404) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "rest: %s not found", what);
  if (status == 429) return ccord_error_set(err, CCORD_ERR_RANGE, "rest: %s rate limited", what);
  return ccord_error_set(err, CCORD_ERR_PROTOCOL, "rest: %s failed with HTTP %ld", what, status);
}

ccord_status live_content_body(const char *utf8, size_t len, const char *nonce, size_t nonce_len,
                                 ccord_buf *body, ccord_error *err)
{
  if (!utf8) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null text");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text too long");
  if (memchr(utf8, 0, len) != nullptr) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text has NUL");
  ccord_jw w;
  ccord_jw_init(&w, body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, utf8, len);
  if (nonce && nonce_len > 0) {   // M0-3: Discord echoes the nonce back and dedupes retries by it
    ccord_jw_key(&w, "nonce");
    ccord_jw_string(&w, nonce, nonce_len);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, body->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  return CCORD_OK;
}

ccord_status ccord_live_build_reply_body(const char *utf8, size_t len, ccord_id reply_to,
                                         bool mention_author, ccord_buf *body, ccord_error *err)
{
  ccord_error_clear(err);
  if (!utf8 || !body) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null text");
  if (reply_to == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "live: reply id");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text too long");
  if (memchr(utf8, 0, len) != nullptr) return ccord_error_set(err, CCORD_ERR_INVALID, "live: text has NUL");
  ccord_jw w;
  ccord_jw_init(&w, body);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, utf8, len);
  ccord_jw_key(&w, "message_reference");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "message_id");
  ccord_jw_id(&w, reply_to);
  ccord_jw_end_object(&w);
  if (!mention_author) {
    ccord_jw_key(&w, "allowed_mentions");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "replied_user");
    ccord_jw_bool(&w, false);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, body->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  return CCORD_OK;
}

static void format_upload_wire_filename(const ccord_upload_file *f, char *out, size_t cap)
{
  const char *raw = (f && f->filename) ? f->filename : "file.bin";
  const char *slash = strrchr(raw, '/');
  const char *base = slash ? slash + 1 : raw;
  if (base[0] == '\0') base = "file.bin";
  if (f && f->spoiler && strncmp(base, "SPOILER_", 8) != 0) {
    (void)snprintf(out, cap, "SPOILER_%s", base);
  } else {
    (void)snprintf(out, cap, "%s", base);
  }
}

// A voice upload (duration_ms != 0) carries a base64 waveform Discord draws verbatim: reject anything that is not
// base64 text or is longer than the model keeps, so a bad waveform fails the send instead of a silent 400.
static bool voice_waveform_valid(const char *wf)
{
  if (!wf || wf[0] == '\0') return true;
  size_t n = strlen(wf);
  if (n > CCORD_RICH_WAVEFORM_MAX_BYTES) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)wf[i];
    bool ok = (c >= (unsigned char)'A' && c <= (unsigned char)'Z') || (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
              (c >= (unsigned char)'0' && c <= (unsigned char)'9') || c == (unsigned char)'+' || c == (unsigned char)'/' ||
              c == (unsigned char)'=';
    if (!ok) return false;
  }
  return true;
}

ccord_status live_upload_voice_check(const ccord_upload_file *f, ccord_error *err)
{
  if (f->duration_ms == 0) return CCORD_OK;   // ordinary file: any waveform is ignored
  if (!voice_waveform_valid(f->waveform_b64)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid voice waveform");
  }
  return CCORD_OK;
}

ccord_status ccord_live_build_upload_slots_body(const ccord_upload_file *files, size_t file_count,
                                                ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!files || !out || file_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: missing upload files");
  }
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many attachments");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "files");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < file_count; i++) {
    if (!files[i].filename || files[i].filename[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty filename");
    }
    if (!files[i].data || files[i].size == 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty file data");
    }
    if (files[i].size > 25u * 1024u * 1024u) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: file too large");
    }
    char wname[CCORD_RICH_SHORT_MAX_BYTES + 1];
    format_upload_wire_filename(&files[i], wname, sizeof wname);
    if (!ccord_utf8_valid_text(wname, strlen(wname))) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid filename");
    }
    char id_str[16];
    (void)snprintf(id_str, sizeof id_str, "%zu", i);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "filename");
    ccord_jw_cstr(&w, wname);
    ccord_jw_key(&w, "file_size");
    ccord_jw_int(&w, (int64_t)files[i].size);
    ccord_jw_key(&w, "id");
    ccord_jw_cstr(&w, id_str);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, out->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: upload slot json failed");
  }
  return CCORD_OK;
}

ccord_status ccord_live_build_upload_message_body(const char *utf8, size_t len, ccord_id reply_to,
                                                  bool mention_author, const ccord_upload_file *files,
                                                  const char uploaded_names[][256], size_t file_count,
                                                  ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!files || !uploaded_names || !out || file_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: missing upload message args");
  }
  if (file_count > CCORD_RICH_MAX_ATTACHMENTS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many attachments");
  }
  if (len > CCORD_SEND_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: text too long");
  }
  const char *txt = utf8 ? utf8 : "";
  if (len > 0 && (!utf8 || memchr(utf8, 0, len) != nullptr || !ccord_utf8_valid_text(utf8, len))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: text is not valid UTF-8");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "content");
  ccord_jw_string(&w, txt, len);
  if (reply_to != CCORD_ID_NONE) {
    ccord_jw_key(&w, "message_reference");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "message_id");
    ccord_jw_id(&w, reply_to);
    ccord_jw_end_object(&w);
    if (!mention_author) {
      ccord_jw_key(&w, "allowed_mentions");
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "replied_user");
      ccord_jw_bool(&w, false);
      ccord_jw_end_object(&w);
    }
  }
  bool voice = false;
  ccord_jw_key(&w, "attachments");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < file_count; i++) {
    if (uploaded_names[i][0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty uploaded_filename");
    }
    ccord_status vst = live_upload_voice_check(&files[i], err);
    if (vst != CCORD_OK) return vst;
    char wname[CCORD_RICH_SHORT_MAX_BYTES + 1];
    format_upload_wire_filename(&files[i], wname, sizeof wname);
    char id_str[16];
    (void)snprintf(id_str, sizeof id_str, "%zu", i);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id");
    ccord_jw_cstr(&w, id_str);
    ccord_jw_key(&w, "filename");
    ccord_jw_cstr(&w, wname);
    ccord_jw_key(&w, "uploaded_filename");
    ccord_jw_cstr(&w, uploaded_names[i]);
    if (files[i].duration_ms != 0) {
      // Voice message: Discord wants the length in float seconds and the waveform as base64 on the attachment.
      voice = true;
      ccord_jw_key(&w, "duration_secs");
      ccord_jw_double(&w, (double)files[i].duration_ms / 1000.0);
      if (files[i].waveform_b64 && files[i].waveform_b64[0] != '\0') {
        ccord_jw_key(&w, "waveform");
        ccord_jw_cstr(&w, files[i].waveform_b64);
      }
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  if (voice) {
    ccord_jw_key(&w, "flags");
    ccord_jw_int(&w, (int64_t)CCORD_MSGFLAG_VOICE_MESSAGE);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    return ccord_error_set(err, out->failed ? CCORD_ERR_RANGE : CCORD_ERR_INVALID, "live: upload message json failed");
  }
  return CCORD_OK;
}

static bool unreserved(unsigned char c)
{
  return (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
         (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
         (c >= (unsigned char)'0' && c <= (unsigned char)'9') || c == (unsigned char)'-' ||
         c == (unsigned char)'_' || c == (unsigned char)'.' || c == (unsigned char)'~';
}

ccord_status live_append_encoded(ccord_buf *out, const char *s, size_t len, ccord_error *err)
{
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (unreserved(c)) {
      if (ccord_buf_append(out, &c, 1) != CCORD_OK) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
      }
    } else {
      char esc[3] = {'%', hex[(c >> 4) & 0x0Fu], hex[c & 0x0Fu]};
      if (ccord_buf_append(out, esc, sizeof esc) != CCORD_OK) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "live: path too long");
      }
    }
  }
  return CCORD_OK;
}


ccord_status ccord_live_build_search_path(ccord_id guild_id, ccord_id channel_id,
                                          const ccord_search_query *query, ccord_buf *out,
                                          ccord_error *err)
{
  ccord_error_clear(err);
  if (!query || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null search query");
  size_t clen = strlen(query->content);
  if (clen > 0 && !ccord_utf8_valid_text(query->content, clen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid search query");
  }
  ccord_id eff_ch = query->channel_id != CCORD_ID_NONE ? query->channel_id : channel_id;
  bool is_guild = guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID;
  if (!is_guild && eff_ch == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: search requires guild or channel id");
  }
  bool has_any = clen > 0 || query->author_id != CCORD_ID_NONE || query->mentions_id != CCORD_ID_NONE ||
                 query->has_mask != 0 || query->min_id != CCORD_ID_NONE || query->max_id != CCORD_ID_NONE ||
                 query->pinned >= 0 || (is_guild && eff_ch != CCORD_ID_NONE);
  if (!has_any) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: empty search query");
  }

  if (is_guild) {
    (void)ccord_buf_appendf(out, "/guilds/%llu/messages/search", (unsigned long long)guild_id);
  } else {
    (void)ccord_buf_appendf(out, "/channels/%llu/messages/search", (unsigned long long)eff_ch);
  }
  char sep = '?';
  if (clen > 0) {
    (void)ccord_buf_appendf(out, "%ccontent=", sep);
    sep = '&';
    ccord_status st = live_append_encoded(out, query->content, clen, err);
    if (st != CCORD_OK) return st;
  }
  if (is_guild && eff_ch != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cchannel_id=%llu", sep, (unsigned long long)eff_ch);
    sep = '&';
  }
  if (query->author_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cauthor_id=%llu", sep, (unsigned long long)query->author_id);
    sep = '&';
  }
  if (query->mentions_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cmentions=%llu", sep, (unsigned long long)query->mentions_id);
    sep = '&';
  }
  static const struct { uint32_t bit; const char *name; } k_has[] = {
    {CCORD_SEARCH_HAS_LINK, "link"},
    {CCORD_SEARCH_HAS_EMBED, "embed"},
    {CCORD_SEARCH_HAS_FILE, "file"},
    {CCORD_SEARCH_HAS_VIDEO, "video"},
    {CCORD_SEARCH_HAS_IMAGE, "image"},
    {CCORD_SEARCH_HAS_SOUND, "sound"},
    {CCORD_SEARCH_HAS_STICKER, "sticker"},
    {CCORD_SEARCH_HAS_POLL, "poll"},
  };
  for (size_t i = 0; i < sizeof k_has / sizeof k_has[0]; i++) {
    if ((query->has_mask & k_has[i].bit) != 0) {
      (void)ccord_buf_appendf(out, "%chas=%s", sep, k_has[i].name);
      sep = '&';
    }
  }
  if (query->min_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cmin_id=%llu", sep, (unsigned long long)query->min_id);
    sep = '&';
  }
  if (query->max_id != CCORD_ID_NONE) {
    (void)ccord_buf_appendf(out, "%cmax_id=%llu", sep, (unsigned long long)query->max_id);
    sep = '&';
  }
  if (query->pinned >= 0) {
    (void)ccord_buf_appendf(out, "%cpinned=%s", sep, query->pinned ? "true" : "false");
    sep = '&';
  }
  if (query->offset > 0) {
    (void)ccord_buf_appendf(out, "%coffset=%u", sep, (unsigned)query->offset);
  }
  if (out->failed) return ccord_error_set(err, CCORD_ERR_RANGE, "live: search path too long");
  return CCORD_OK;
}


static bool valid_custom_id(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  return n <= CCORD_RICH_CUSTOM_ID_MAX_BYTES && ccord_utf8_valid_text(s, n);
}

ccord_status ccord_live_build_poll_vote_body(const uint32_t *answer_ids, size_t answer_count,
                                             ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null output buffer");
  if (answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many poll answers");
  }
  if (answer_count > 0 && !answer_ids) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: null poll answers");
  }
  for (size_t i = 0; i < answer_count; i++) {
    if (answer_ids[i] == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "live: zero poll answer id");
  }
  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "answer_ids");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < answer_count; i++) {
    ccord_jw_int(&w, (int64_t)answer_ids[i]);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_NOMEM, "live: poll vote json failed");
  return CCORD_OK;
}

static bool valid_slash_ident(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  if (n > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') return false;
  }
  return true;
}

static bool valid_context_cmd_name(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  if (n > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\n' || c == '\r') return false;
  }
  return true;
}

static bool parse_strict_i64(const char *s, int64_t *out)
{
  if (!s || s[0] == '\0') return false;
  if (s[0] == ' ' || s[0] == '\t') return false;
  errno = 0;
  char *end = nullptr;
  long long v = strtoll(s, &end, 10);
  if (errno != 0 || !end || end == s || *end != '\0') return false;
  *out = (int64_t)v;
  return true;
}

static bool parse_strict_f64(const char *s, double *out)
{
  if (!s || s[0] == '\0') return false;
  if (s[0] == ' ' || s[0] == '\t') return false;
  errno = 0;
  char *end = nullptr;
  double v = strtod(s, &end);
  if (errno != 0 || !end || end == s || *end != '\0') return false;
  *out = v;
  return true;
}

ccord_status ccord_live_build_interaction_body(const ccord_interaction_request *req,
                                               const char *session_id, const char *nonce,
                                               ccord_buf *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!req || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null interaction request");
  if (req->application_id == CCORD_ID_NONE || req->channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: interaction missing target ids");
  }
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    if (!valid_custom_id(req->custom_id)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid interaction custom_id");
    }
    if (req->message_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: component interaction missing message_id");
    }
    if (req->component_type < CCORD_COMP_BUTTON || req->component_type > CCORD_COMP_CHANNEL_SELECT ||
        req->component_type == CCORD_COMP_TEXT_INPUT) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid component_type");
    }
    if (req->value_count > CCORD_RICH_MAX_SELECT_OPTIONS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many select values");
    }
    if (req->value_count > 0 && !req->values) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: null select values");
    }
    for (size_t i = 0; i < req->value_count; i++) {
      if (!req->values[i]) return ccord_error_set(err, CCORD_ERR_INVALID, "live: null select value");
      size_t vn = strlen(req->values[i]);
      if (vn > CCORD_RICH_SHORT_MAX_BYTES || !ccord_utf8_valid_text(req->values[i], vn)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid select value");
      }
    }
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    if (!valid_custom_id(req->custom_id)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid interaction custom_id");
    }
    if (req->field_count == 0 || req->field_count > CCORD_RICH_MAX_COMPONENT_ROWS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: modal field count");
    }
    if (!req->field_ids || !req->field_values) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: null modal fields");
    }
    for (size_t i = 0; i < req->field_count; i++) {
      if (!valid_custom_id(req->field_ids[i])) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid modal field custom_id");
      }
      const char *fv = req->field_values[i] ? req->field_values[i] : "";
      size_t fvn = strlen(fv);
      if (fvn > CCORD_RICH_TEXT_MAX_BYTES || !ccord_utf8_valid_text(fv, fvn)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid modal field value");
      }
    }
  } else if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND ||
             req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    bool is_auto = req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE;
    uint8_t cmd_type = req->command_type != 0 ? req->command_type : 1u;
    bool has_group = req->subcommand_group && req->subcommand_group[0] != '\0';
    bool has_sub = req->subcommand && req->subcommand[0] != '\0';
    if (cmd_type < 1u || cmd_type > 3u) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid command_type");
    }
    if (req->command_id == CCORD_ID_NONE) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash command target");
    }
    if (cmd_type == 2u || cmd_type == 3u) {
      if (is_auto || !valid_context_cmd_name(req->command_name) || req->target_id == CCORD_ID_NONE ||
          req->option_count > 0 || has_group || has_sub || req->focused_option >= 0) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid context-menu command request");
      }
    } else {
      if (!valid_slash_ident(req->command_name) || req->target_id != CCORD_ID_NONE) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash command target");
      }
      if (has_group && (!has_sub || !valid_slash_ident(req->subcommand_group))) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash subcommand_group");
      }
      if (has_sub && !valid_slash_ident(req->subcommand)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash subcommand");
      }
    }
    if (req->option_count > CCORD_SLASH_MAX_OPTIONS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "live: too many slash options");
    }
    if (req->option_count > 0 && (!req->option_names || !req->option_kinds || !req->option_values)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: null slash options");
    }
    if (is_auto) {
      if (req->option_count == 0 || req->focused_option < 0 || (size_t)req->focused_option >= req->option_count) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid focused_option");
      }
    } else if (req->focused_option >= 0) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "live: unexpected focused_option");
    }
    for (size_t i = 0; i < req->option_count; i++) {
      if (!valid_slash_ident(req->option_names[i])) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash option name");
      }
      uint8_t k = req->option_kinds[i];
      if (k < CCORD_SLASH_OPT_STRING || k > CCORD_SLASH_OPT_NUMBER) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash option kind");
      }
      const char *ov = req->option_values[i] ? req->option_values[i] : "";
      size_t ovn = strlen(ov);
      if (ovn > CCORD_SEND_MAX_BYTES || !ccord_utf8_valid_text(ov, ovn)) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid slash option value");
      }
      bool focused = is_auto && i == (size_t)req->focused_option;
      if (!focused) {
        if (k == CCORD_SLASH_OPT_INTEGER) {
          int64_t iv = 0;
          if (!parse_strict_i64(ov, &iv)) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid integer slash option");
          }
        } else if (k == CCORD_SLASH_OPT_NUMBER) {
          double dv = 0.0;
          if (!parse_strict_f64(ov, &dv)) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid number slash option");
          }
        } else if (k == CCORD_SLASH_OPT_BOOLEAN) {
          if (strcmp(ov, "true") != 0 && strcmp(ov, "false") != 0 &&
              strcmp(ov, "1") != 0 && strcmp(ov, "0") != 0) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid boolean slash option");
          }
        } else if (k == CCORD_SLASH_OPT_USER || k == CCORD_SLASH_OPT_CHANNEL ||
                   k == CCORD_SLASH_OPT_ROLE || k == CCORD_SLASH_OPT_MENTIONABLE) {
          ccord_id sid = CCORD_ID_NONE;
          if (!ccord_id_parse(ov, ovn, &sid) || sid == CCORD_ID_NONE) {
            return ccord_error_set(err, CCORD_ERR_INVALID, "live: invalid snowflake slash option");
          }
        }
      } else if (k != CCORD_SLASH_OPT_STRING && k != CCORD_SLASH_OPT_INTEGER && k != CCORD_SLASH_OPT_NUMBER) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "live: autocomplete not supported on option kind");
      }
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "live: unsupported interaction type");
  }

  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "type");
  ccord_jw_int(&w, (int64_t)req->type);
  ccord_jw_key(&w, "application_id");
  ccord_jw_id(&w, req->application_id);
  if (req->guild_id != CCORD_ID_NONE && req->guild_id != CCORD_DM_GUILD_ID) {
    ccord_jw_key(&w, "guild_id");
    ccord_jw_id(&w, req->guild_id);
  }
  ccord_jw_key(&w, "channel_id");
  ccord_jw_id(&w, req->channel_id);
  ccord_jw_key(&w, "session_id");
  ccord_jw_cstr(&w, (session_id && session_id[0]) ? session_id : "ccord-session");
  ccord_jw_key(&w, "nonce");
  ccord_jw_cstr(&w, (nonce && nonce[0]) ? nonce : "1");
  if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT) {
    ccord_jw_key(&w, "message_id");
    ccord_jw_id(&w, req->message_id);
    ccord_jw_key(&w, "message_flags");
    ccord_jw_int(&w, (int64_t)req->message_flags);
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "component_type");
    ccord_jw_int(&w, (int64_t)req->component_type);
    ccord_jw_key(&w, "custom_id");
    ccord_jw_cstr(&w, req->custom_id);
    if (req->component_type != CCORD_COMP_BUTTON) {
      ccord_jw_key(&w, "values");
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < req->value_count; i++) {
        ccord_jw_cstr(&w, req->values[i]);
      }
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_object(&w);
  } else if (req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    if (req->modal_id != CCORD_ID_NONE) {
      ccord_jw_key(&w, "id");
      ccord_jw_id(&w, req->modal_id);
    }
    ccord_jw_key(&w, "custom_id");
    ccord_jw_cstr(&w, req->custom_id);
    ccord_jw_key(&w, "components");
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < req->field_count; i++) {
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_COMP_ACTION_ROW);
      ccord_jw_key(&w, "components");
      ccord_jw_begin_array(&w);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_COMP_TEXT_INPUT);
      ccord_jw_key(&w, "custom_id");
      ccord_jw_cstr(&w, req->field_ids[i]);
      ccord_jw_key(&w, "value");
      ccord_jw_cstr(&w, req->field_values[i] ? req->field_values[i] : "");
      ccord_jw_end_object(&w);
      ccord_jw_end_array(&w);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
    ccord_jw_end_object(&w);
  } else {
    bool is_auto = req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE;
    uint8_t cmd_type = req->command_type != 0 ? req->command_type : 1u;
    bool has_group = req->subcommand_group && req->subcommand_group[0] != '\0';
    bool has_sub = req->subcommand && req->subcommand[0] != '\0';
    ccord_id ver = req->command_version != CCORD_ID_NONE ? req->command_version : req->command_id;
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "version");
    ccord_jw_id(&w, ver);
    ccord_jw_key(&w, "id");
    ccord_jw_id(&w, req->command_id);
    ccord_jw_key(&w, "name");
    ccord_jw_cstr(&w, req->command_name);
    ccord_jw_key(&w, "type");
    ccord_jw_int(&w, (int64_t)cmd_type);
    if ((cmd_type == 2u || cmd_type == 3u) && req->target_id != CCORD_ID_NONE) {
      ccord_jw_key(&w, "target_id");
      ccord_jw_id(&w, req->target_id);
    }
    if (has_group) {
      ccord_jw_key(&w, "options");
      ccord_jw_begin_array(&w);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_SLASH_OPT_SUB_COMMAND_GROUP);
      ccord_jw_key(&w, "name");
      ccord_jw_cstr(&w, req->subcommand_group);
    }
    if (has_sub) {
      ccord_jw_key(&w, "options");
      ccord_jw_begin_array(&w);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, CCORD_SLASH_OPT_SUB_COMMAND);
      ccord_jw_key(&w, "name");
      ccord_jw_cstr(&w, req->subcommand);
    }
    ccord_jw_key(&w, "options");
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < req->option_count; i++) {
      uint8_t k = req->option_kinds[i];
      const char *ov = req->option_values[i] ? req->option_values[i] : "";
      bool focused = is_auto && i == (size_t)req->focused_option;
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "type");
      ccord_jw_int(&w, (int64_t)k);
      ccord_jw_key(&w, "name");
      ccord_jw_cstr(&w, req->option_names[i]);
      ccord_jw_key(&w, "value");
      if (focused) {
        int64_t iv = 0;
        double dv = 0.0;
        if (k == CCORD_SLASH_OPT_INTEGER && parse_strict_i64(ov, &iv)) ccord_jw_int(&w, iv);
        else if (k == CCORD_SLASH_OPT_NUMBER && parse_strict_f64(ov, &dv)) ccord_jw_double(&w, dv);
        else ccord_jw_cstr(&w, ov);
      } else if (k == CCORD_SLASH_OPT_INTEGER) {
        int64_t iv = 0;
        (void)parse_strict_i64(ov, &iv);
        ccord_jw_int(&w, iv);
      } else if (k == CCORD_SLASH_OPT_NUMBER) {
        double dv = 0.0;
        (void)parse_strict_f64(ov, &dv);
        ccord_jw_double(&w, dv);
      } else if (k == CCORD_SLASH_OPT_BOOLEAN) {
        ccord_jw_bool(&w, strcmp(ov, "true") == 0 || strcmp(ov, "1") == 0);
      } else if (k == CCORD_SLASH_OPT_USER || k == CCORD_SLASH_OPT_CHANNEL ||
                 k == CCORD_SLASH_OPT_ROLE || k == CCORD_SLASH_OPT_MENTIONABLE) {
        ccord_id sid = CCORD_ID_NONE;
        (void)ccord_id_parse(ov, strlen(ov), &sid);
        ccord_jw_id(&w, sid);
      } else {
        ccord_jw_cstr(&w, ov);
      }
      if (focused) {
        ccord_jw_key(&w, "focused");
        ccord_jw_bool(&w, true);
      }
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
    if (has_sub) {
      ccord_jw_end_object(&w);
      ccord_jw_end_array(&w);
    }
    if (has_group) {
      ccord_jw_end_object(&w);
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) return ccord_error_set(err, CCORD_ERR_INVALID, "live: interaction json failed");
  return CCORD_OK;
}


