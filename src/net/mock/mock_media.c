// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

ccord_status mock_set_presence_ex(ccord_transport *t, ccord_presence presence, const char *emoji_name,
                                         ccord_id emoji_id, const char *status_text, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: presence");
  const char *em = emoji_name ? emoji_name : "";
  const char *st = status_text ? status_text : "";
  size_t em_len = strlen(em);
  size_t st_len = strlen(st);
  if (em_len > CCORD_EMOJI_MAX_BYTES || !ccord_utf8_valid_text(em, em_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom status emoji");
  }
  if (st_len > CCORD_ACTIVITY_TEXT_MAX_BYTES || !ccord_utf8_valid_text(st, st_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid custom status text");
  }
  m->users[0].presence = presence;
  m->op3_count++;
  m->op3_presence = presence;
  (void)snprintf(m->op3_text, sizeof m->op3_text, "%s", st);
  if (emoji_id != CCORD_ID_NONE && em_len > 0) {   // the store keeps a custom emoji as name:id, like a gateway presence
    (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%.40s:%llu", em,
                   (unsigned long long)emoji_id);
  } else {
    (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%s", em);
  }
  (void)snprintf(m->users[0].custom_status_text, sizeof m->users[0].custom_status_text, "%s", st);
  mock_item it = mk_make_item(CCORD_NET_PRESENCE_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = m->self_id;
  it.ev.presence = presence;
  if (st_len > 0) {
    char *copy = ccord_strndup(st, st_len);
    if (copy) {
      mk_retire_content(m, copy);
      it.ev.status_text = copy;
      m->users[0].status_text = copy;
    }
  } else {
    it.ev.status_text = "";
    m->users[0].status_text = "";
  }
  it.ev.has_presence_detail = true;
  (void)snprintf(it.ev.custom_status_emoji, sizeof it.ev.custom_status_emoji, "%s", m->users[0].custom_status_emoji);
  (void)snprintf(it.ev.custom_status_text, sizeof it.ev.custom_status_text, "%s", m->users[0].custom_status_text);
  it.ev.custom_status_expires_ms = m->users[0].custom_status_expires_ms;
  it.ev.has_activity = m->users[0].has_activity;
  it.ev.activity_type = (ccord_activity_type)m->users[0].activity_type;
  (void)snprintf(it.ev.activity_name, sizeof it.ev.activity_name, "%s", m->users[0].activity_name);
  (void)snprintf(it.ev.activity_details, sizeof it.ev.activity_details, "%s", m->users[0].activity_details);
  (void)snprintf(it.ev.activity_state, sizeof it.ev.activity_state, "%s", m->users[0].activity_state);
  it.ev.activity_start_ms = m->users[0].activity_start_ms;
  it.ev.activity_end_ms = m->users[0].activity_end_ms;
  if (m->own_presence_silent) return CCORD_OK;   // the server does not echo the account's own presence back
  return mk_push_item(m, &it, err);
}

ccord_status mock_set_presence(ccord_transport *t, ccord_presence presence, const char *status_text,
                                      ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (presence > CCORD_PRESENCE_DND) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: presence");
  m->users[0].presence = presence;
  m->op3_count++;
  m->op3_presence = presence;
  (void)snprintf(m->op3_text, sizeof m->op3_text, "%s", status_text ? status_text : "");
  mock_item it = mk_make_item(CCORD_NET_PRESENCE_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = m->self_id;
  it.ev.presence = presence;
  if (status_text && status_text[0] != '\0') {
    char *copy = ccord_strndup(status_text, strlen(status_text));
    if (copy) {
      mk_retire_content(m, copy);
      it.ev.status_text = copy;
    }
  }
  if (m->own_presence_silent) return CCORD_OK;   // the server does not echo the account's own presence back
  return mk_push_item(m, &it, err);
}

// Same contract as the live gateway's ccord_gateway_set_rich_activities.
static void mock_set_self_activity_fields(mock *m, const char *name);

static ccord_status mock_rich_activities_valid(const char *json, size_t len, ccord_error *err)
{
  if (len > CCORD_GATEWAY_RICH_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: rich activities too large");
  }
  ccord_arena a;
  ccord_arena_init(&a, len + 256u);
  const ccord_json *j = ccord_json_parse(&a, json, len, nullptr, err);
  ccord_status st = CCORD_OK;
  if (!j) {
    st = err && err->code != CCORD_OK ? err->code : CCORD_ERR_PARSE;
  } else if (ccord_json_type_of(j) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: rich activities must be an array");
  } else if (ccord_json_array_len(j) > CCORD_GATEWAY_RICH_MAX_ACTIVITIES) {
    st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: at most %u rich activities",
                         (unsigned)CCORD_GATEWAY_RICH_MAX_ACTIVITIES);
  } else {
    for (size_t i = 0; i < ccord_json_array_len(j); i++) {
      const ccord_json *e = ccord_json_array_at(j, i);
      const ccord_json *name = ccord_json_get(e, "name");
      const ccord_json *type = ccord_json_get(e, "type");
      int64_t t;
      if (ccord_json_type_of(e) != CCORD_JSON_OBJECT || !name || ccord_json_type_of(name) != CCORD_JSON_STRING ||
          !type || !ccord_json_int64(type, &t)) {
        st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: rich activity needs a name and a type");
        break;
      }
    }
  }
  ccord_arena_free(&a);
  return st;
}

ccord_status mock_set_rich_activities(ccord_transport *t, const char *json, size_t len, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  char *copy = nullptr;
  if (json && len > 0) {
    ccord_status st = mock_rich_activities_valid(json, len, err);
    if (st != CCORD_OK) return st;
    copy = ccord_malloc(len + 1);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    memcpy(copy, json, len);
    copy[len] = '\0';
  }
  ccord_free(m->rich_json);
  m->rich_json = copy;
  m->rich_len = copy ? len : 0;
  // Like Discord, withdrawing the activities ends the account's Playing line: the next presence echo must not bring it back.
  if (!copy) mock_set_self_activity_fields(m, nullptr);
  return CCORD_OK;
}


static const uint8_t k_mock_png_1x1[67] = {
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
  0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x01, 0xE5, 0x27, 0xDE, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
  0x42, 0x60, 0x82
};

static void ensure_cache_dirs(const char *file_path)
{
  size_t len = strlen(file_path);
  if (len == 0 || len >= 4096) return;
  char tmp[4096];
  memcpy(tmp, file_path, len + 1);
  char *slash = strrchr(tmp, '/');
#if defined(_WIN32)
  char *bs = strrchr(tmp, '\\');
  if (bs && (!slash || bs > slash)) slash = bs;
#endif
  if (!slash || slash == tmp) return;
  *slash = '\0';
  ccord_error derr;
  (void)ccord_mkdir_p(tmp, 0700, &derr);
}

static bool url_has_wav_ext(const char *url)
{
  const char *end = url + strcspn(url, "?#");
  return (size_t)(end - url) >= 4u &&
         (end[-4] == '.') &&
         (end[-3] == 'w' || end[-3] == 'W') &&
         (end[-2] == 'a' || end[-2] == 'A') &&
         (end[-1] == 'v' || end[-1] == 'V');
}

static bool url_has_video_ext(const char *url)
{
  const char *end = url + strcspn(url, "?#");
  size_t len = (size_t)(end - url);
  if (len >= 4u && end[-4] == '.' &&
      (end[-3] == 'm' || end[-3] == 'M') &&
      (end[-2] == 'p' || end[-2] == 'P') &&
      end[-1] == '4') {
    return true;
  }
  return len >= 5u && end[-5] == '.' &&
         (end[-4] == 'w' || end[-4] == 'W') &&
         (end[-3] == 'e' || end[-3] == 'E') &&
         (end[-2] == 'b' || end[-2] == 'B') &&
         (end[-1] == 'm' || end[-1] == 'M');
}

static const uint8_t k_mock_video_gif[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
  0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
  0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
  0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
  0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
  0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
  0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
  0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
  0x02, 0x00, 0x3b,
};

static size_t build_mock_wav(uint8_t *buf, size_t cap)
{
  const uint32_t rate = 48000u;
  const uint32_t samples = 24000u;   // 500 ms mono S16LE
  const uint32_t data_bytes = samples * 2u;
  if (cap < 44u + data_bytes) return 0;
  uint32_t riff_sz = 36u + data_bytes;
  uint32_t fmt_sz = 16u;
  uint16_t audio_fmt = 1u, channels = 1u, block_align = 2u, bits = 16u;
  uint32_t byte_rate = rate * 2u;
  memcpy(buf + 0, "RIFF", 4);
  memcpy(buf + 4, &riff_sz, 4);
  memcpy(buf + 8, "WAVEfmt ", 8);
  memcpy(buf + 16, &fmt_sz, 4);
  memcpy(buf + 20, &audio_fmt, 2);
  memcpy(buf + 22, &channels, 2);
  memcpy(buf + 24, &rate, 4);
  memcpy(buf + 28, &byte_rate, 4);
  memcpy(buf + 32, &block_align, 2);
  memcpy(buf + 34, &bits, 2);
  memcpy(buf + 36, "data", 4);
  memcpy(buf + 40, &data_bytes, 4);
  for (uint32_t i = 0; i < samples; i++) {
    int16_t v = ((i % 96u) < 48u) ? (int16_t)6000 : (int16_t)-6000;
    memcpy(buf + 44u + i * 2u, &v, 2);
  }
  return 44u + data_bytes;
}

// Mock transport must never write synthetic stubs into the live media cache ($XDG_CACHE_HOME/ccord/media/),
// because real CDN URLs requested during tests or --mock runs (such as Discord's built-in Wumpus Wave
// sticker) would permanently poison the live cache on disk.
static ccord_status mock_media_cache_path(const char *url, char *out_path, size_t cap, ccord_error *err)
{
  char live_path[4096];
  ccord_status st = ccord_media_cache_path(url, live_path, sizeof live_path, err);
  if (st != CCORD_OK) return st;
  const char *slash = strrchr(live_path, '/');
  if (!slash || (size_t)(slash - live_path) < 6u || memcmp(slash - 6, "/media", 6) != 0) {
    return ccord_error_set(err, CCORD_ERR_INTERNAL, "mock: unexpected media cache layout");
  }
  size_t prefix_len = (size_t)(slash - 6 - live_path);
  static const char k_mock_seg[] = "/mock-media";
  size_t mock_seg_len = sizeof k_mock_seg - 1u;
  size_t tail_len = strlen(slash);
  if (prefix_len + mock_seg_len + tail_len + 1u > cap) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: cache path buffer too small");
  }
  memcpy(out_path, live_path, prefix_len);
  memcpy(out_path + prefix_len, k_mock_seg, mock_seg_len);
  memcpy(out_path + prefix_len + mock_seg_len, slash, tail_len + 1u);
  return CCORD_OK;
}

ccord_status mock_fetch_media(ccord_transport *t, const char *url, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!url || url[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty media url");
  char cache_path[4096];
  ccord_status st = mock_media_cache_path(url, cache_path, sizeof cache_path, err);
  if (st != CCORD_OK) return st;

  if (m->fail_media[0] && strstr(url, m->fail_media)) {
    // A download that does not arrive: the same failure event the live transport reports.
    char *failed_url = ccord_strndup(url, strlen(url));
    if (!failed_url) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    mk_retire_content(m, failed_url);
    mock_item fit = mk_make_item(CCORD_NET_MEDIA_LOADED, mk_now_of(m) + m->cfg.latency_ms);
    fit.ev.status = CCORD_ERR_IO;
    fit.ev.media_url = failed_url;
    fit.ev.media_path = "";
    return mk_push_item(m, &fit, err);
  }

  const mock_upload *uploaded = mk_upload_cache_find(m, url);
  bool is_wav = url_has_wav_ext(url);
  bool is_vid = !is_wav && url_has_video_ext(url);
  size_t payload_len = uploaded ? uploaded->len
                               : (is_wav ? (44u + 24000u * 2u)
                                         : (is_vid ? sizeof k_mock_video_gif : sizeof k_mock_png_1x1));
  uint8_t *bytes = ccord_alloc_array(payload_len, 1);
  char *url_copy = ccord_strndup(url, strlen(url));
  char *path_copy = ccord_strndup(cache_path, strlen(cache_path));
  if (!url_copy || !path_copy || !bytes) {
    ccord_free(url_copy);
    ccord_free(path_copy);
    ccord_free(bytes);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  if (uploaded) memcpy(bytes, uploaded->data, payload_len);
  else if (is_wav) (void)build_mock_wav(bytes, payload_len);
  else if (is_vid) memcpy(bytes, k_mock_video_gif, sizeof k_mock_video_gif);
  else memcpy(bytes, k_mock_png_1x1, sizeof k_mock_png_1x1);

  ensure_cache_dirs(cache_path);
  FILE *fp = fopen(cache_path, "wb");
  if (fp) {
    (void)fwrite(bytes, 1, payload_len, fp);
    fclose(fp);
  }
  mk_retire_content(m, url_copy);
  mk_retire_content(m, path_copy);

  mock_item it = mk_make_item(CCORD_NET_MEDIA_LOADED, mk_now_of(m) + m->cfg.latency_ms);
  it.media_data = bytes;
  it.ev.media_url = url_copy;
  it.ev.media_path = path_copy;
  it.ev.media_data = bytes;
  it.ev.media_len = payload_len;
  return mk_push_item(m, &it, err);
}

// ---- Ranged media ----
// Registers a virtual file: `total` bytes synthesised by f->read, so a file of any size needs no buffer. A second
// registration of the same url_part replaces the first and resets its request count.
ccord_status ccord_mock_serve_range_file(ccord_transport *t, const ccord_mock_range_file *f)
{
  mock *m = (mock *)t;
  if (!m || !f || !f->url_part || !f->url_part[0] || !f->read || f->total < 0)
    return CCORD_ERR_INVALID;
  if (strlen(f->url_part) >= MOCK_RANGE_URL_PART_MAX) return CCORD_ERR_INVALID;
  mock_range_slot *slot = nullptr;
  for (size_t i = 0; i < m->nrange_files; i++) {
    if (strcmp(m->range_files[i].url_part, f->url_part) == 0) slot = &m->range_files[i];
  }
  if (!slot) {
    if (m->nrange_files == MOCK_RANGE_FILES_MAX) return CCORD_ERR_NOMEM;
    slot = &m->range_files[m->nrange_files++];
  }
  slot->f = *f;
  snprintf(slot->url_part, sizeof slot->url_part, "%s", f->url_part);
  slot->f.url_part = slot->url_part;
  slot->f.etag[sizeof slot->f.etag - 1] = '\0';
  slot->requests = 0;
  return CCORD_OK;
}

void ccord_mock_range_stats_get(const ccord_transport *t, ccord_mock_range_stats *out)
{
  if (!out) return;
  const mock *m = (const mock *)t;
  if (m) *out = m->range_stats;
  else memset(out, 0, sizeof *out);
}

static void mock_range_log(mock *m, int64_t start, size_t len)
{
  ccord_mock_range_stats *st = &m->range_stats;
  if (st->log_count == MOCK_RANGE_LOG) {   // full: drop the oldest
    memmove(st->log_start, st->log_start + 1, (MOCK_RANGE_LOG - 1u) * sizeof st->log_start[0]);
    memmove(st->log_len, st->log_len + 1, (MOCK_RANGE_LOG - 1u) * sizeof st->log_len[0]);
    st->log_count--;
  }
  st->log_start[st->log_count] = start;
  st->log_len[st->log_count] = (uint32_t)len;
  st->log_count++;
}

// One MEDIA_RANGE answer. Takes ownership of `bytes` (may be null with n == 0).
static ccord_status mock_range_answer(mock *m, const ccord_media_range_req *req, ccord_status status, uint8_t *bytes,
                                      size_t n, int64_t total, uint32_t flags, int32_t http, uint8_t hops,
                                      const char *etag, ccord_error *err)
{
  char *url_copy = ccord_strndup(req->url, strlen(req->url));
  if (!url_copy) {
    ccord_free(bytes);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  mk_retire_content(m, url_copy);
  mock_item it = mk_make_item(CCORD_NET_MEDIA_RANGE, mk_now_of(m) + m->cfg.latency_ms);
  it.media_data = bytes;
  it.ev.status = status;
  it.ev.media_url = url_copy;
  it.ev.media_path = "";
  it.ev.media_data = bytes;
  it.ev.media_len = n;
  it.ev.range_id = req->id;
  it.ev.range_start = req->start;
  it.ev.range_total = total;
  it.ev.range_flags = flags;
  it.ev.range_http = http;
  it.ev.range_redirects = hops;
  if (etag) snprintf(it.ev.range_etag, sizeof it.ev.range_etag, "%s", etag);
  return mk_push_item(m, &it, err);
}

ccord_status mock_fetch_media_range(ccord_transport *t, const ccord_media_range_req *req, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || !req->url || !req->url[0]) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty media url");
  if (req->start < 0 || req->len < 1 || req->len > CCORD_STREAM_RANGE_MAX_BYTES)
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad media range");
  // The live gate (ccord_live_user_video_url_allowed) is not linked into the mock; it only insists on https.
  if (strncmp(req->url, "https://", 8) != 0)
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "mock: media url blocked");

  mock_range_slot *slot = nullptr;
  for (size_t i = 0; i < m->nrange_files && !slot; i++) {
    if (strstr(req->url, m->range_files[i].url_part)) slot = &m->range_files[i];
  }
  // Not registered: answered like a server that ignores Range, so the caller falls back to the whole-file path.
  if (!slot)
    return mock_range_answer(m, req, CCORD_OK, nullptr, 0, -1, CCORD_RANGE_UNRANGED, 200, 0, nullptr, err);

  const ccord_mock_range_file *f = &slot->f;
  ccord_mock_range_stats *st = &m->range_stats;
  slot->requests++;
  st->requests++;
  if ((int64_t)req->len > st->max_len) st->max_len = (int64_t)req->len;
  mock_range_log(m, req->start, req->len);

  if (f->flags & CCORD_MOCK_RANGE_REDIRECT_HTTP)   // the chain ends at http://: curl refuses it
    return mock_range_answer(m, req, CCORD_ERR_IO, nullptr, 0, -1, 0, 0, f->redirect_hops, nullptr, err);
  if (f->fail_after_requests && slot->requests >= f->fail_after_requests) {
    ccord_status fs = (f->fail_status == 401 || f->fail_status == 403 || f->fail_status == 410) ? CCORD_ERR_PERMISSION
                                                                                              : CCORD_ERR_IO;
    return mock_range_answer(m, req, fs, nullptr, 0, -1, 0, f->fail_status, f->redirect_hops, nullptr, err);
  }
  const bool unknown = (f->flags & CCORD_MOCK_RANGE_UNKNOWN_TOTAL) != 0;
  const int64_t shown_total = unknown ? -1 : f->total;
  if (f->flags & CCORD_MOCK_RANGE_IGNORE)
    return mock_range_answer(m, req, CCORD_OK, nullptr, 0, shown_total, CCORD_RANGE_UNRANGED, 200, f->redirect_hops,
                             f->etag, err);
  if (req->start >= f->total)   // 416: the start is at or past the end; the total comes from Content-Range: bytes */T
    return mock_range_answer(m, req, CCORD_OK, nullptr, 0, f->total, CCORD_RANGE_ACCEPTS | CCORD_RANGE_EOF, 416,
                             f->redirect_hops, f->etag, err);

  const int64_t left = f->total - req->start;
  const size_t n = left < (int64_t)req->len ? (size_t)left : req->len;
  uint8_t *bytes = ccord_alloc_array(n, 1);
  if (!bytes) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  f->read(f->ud, req->start, bytes, n);
  st->bytes_served += n;
  uint32_t flags = CCORD_RANGE_ACCEPTS;
  // With an unknown total a short body is the only sign of the end.
  if (unknown ? n < req->len : req->start + (int64_t)n == f->total) flags |= CCORD_RANGE_EOF;
  return mock_range_answer(m, req, CCORD_OK, bytes, n, shown_total, flags, 206, f->redirect_hops, f->etag, err);
}

ccord_status ccord_mock_serve_video_resolve(ccord_transport *t, const ccord_mock_video_resolve *r)
{
  mock *m = (mock *)t;
  if (!m || !r || !r->url_part || !r->url_part[0]) return CCORD_ERR_INVALID;
  if (strlen(r->url_part) >= MOCK_RANGE_URL_PART_MAX) return CCORD_ERR_INVALID;
  mock_video_resolve_slot *slot = nullptr;
  for (size_t i = 0; i < m->nvideo_resolves; i++) {
    if (strcmp(m->video_resolves[i].url_part, r->url_part) == 0) slot = &m->video_resolves[i];
  }
  if (!slot) {
    if (m->nvideo_resolves == MOCK_VIDEO_RESOLVE_MAX) return CCORD_ERR_NOMEM;
    slot = &m->video_resolves[m->nvideo_resolves++];
  }
  memset(slot, 0, sizeof *slot);
  snprintf(slot->url_part, sizeof slot->url_part, "%s", r->url_part);
  if (r->video_url) snprintf(slot->video_url, sizeof slot->video_url, "%s", r->video_url);
  if (r->audio_url) snprintf(slot->audio_url, sizeof slot->audio_url, "%s", r->audio_url);
  if (r->error) snprintf(slot->error, sizeof slot->error, "%s", r->error);
  slot->duration_ms = r->duration_ms;
  slot->status = r->status;
  slot->missing_helper = r->missing_helper;
  return CCORD_OK;
}

ccord_status mock_resolve_video(ccord_transport *t, uint64_t req_id, const char *url, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!url || !url[0]) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: empty video url");
  if (strncmp(url, "https://", 8) != 0) return ccord_error_set(err, CCORD_ERR_PERMISSION, "mock: video url blocked");

  const mock_video_resolve_slot *slot = nullptr;
  for (size_t i = 0; i < m->nvideo_resolves; i++) {
    if (strstr(url, m->video_resolves[i].url_part)) {
      slot = &m->video_resolves[i];
      break;
    }
  }
  const char *vurl = slot ? slot->video_url : "https://mock.ccord.invalid/resolved/clip.mp4";
  const char *aurl = slot ? slot->audio_url : "";
  const char *etext = slot ? slot->error : "";

  char *page_copy = ccord_strdup(url);
  char *v_copy = ccord_strdup(vurl);
  char *a_copy = ccord_strdup(aurl);
  char *e_copy = ccord_strdup(etext);
  if (!page_copy || !v_copy || !a_copy || !e_copy) {
    ccord_free(page_copy); ccord_free(v_copy); ccord_free(a_copy); ccord_free(e_copy);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  mk_retire_content(m, page_copy);
  mk_retire_content(m, v_copy);
  mk_retire_content(m, a_copy);
  mk_retire_content(m, e_copy);

  mock_item it = mk_make_item(CCORD_NET_VIDEO_RESOLVED, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.status = slot ? slot->status : CCORD_OK;
  it.ev.resolve_id = req_id;
  it.ev.media_url = page_copy;
  it.ev.resolved_video_url = v_copy;
  it.ev.resolved_audio_url = a_copy;
  it.ev.resolved_duration_ms = slot ? slot->duration_ms : 0u;
  it.ev.resolved_missing_helper = slot ? slot->missing_helper : false;
  it.ev.text = e_copy;
  return mk_push_item(m, &it, err);
}


ccord_status mock_set_relationship(ccord_transport *t, ccord_id user_id, ccord_relationship_kind kind,
                                          ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == CCORD_ID_NONE || user_id == m->self_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid relationship user id");
  }
  if (kind > CCORD_RELATIONSHIP_PENDING_OUTGOING) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid relationship kind");
  }
  bool user_known = false;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == user_id) {
      user_known = true;
      break;
    }
  }
  if (!user_known) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown user");

  if (kind == CCORD_RELATIONSHIP_NONE) {
    for (size_t i = 0; i < m->nrelationships; i++) {
      if (m->relationships[i].user_id == user_id) {
        memmove(&m->relationships[i], &m->relationships[i + 1],
                (m->nrelationships - i - 1) * sizeof *m->relationships);
        m->nrelationships--;
        m->snap.relationship_count = m->nrelationships;
        break;
      }
    }
  } else {
    bool updated = false;
    for (size_t i = 0; i < m->nrelationships; i++) {
      if (m->relationships[i].user_id == user_id) {
        m->relationships[i].kind = kind;
        updated = true;
        break;
      }
    }
    if (!updated) {
      if (m->nrelationships >= MOCK_NUM_USERS) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many relationships");
      }
      m->relationships[m->nrelationships++] = (ccord_relationship){.user_id = user_id, .kind = kind};
      m->snap.relationship_count = m->nrelationships;
    }
  }

  mock_item it = mk_make_item(CCORD_NET_RELATIONSHIP_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = user_id;
  it.ev.relationship_kind = kind;
  return mk_push_item(m, &it, err);
}

// Profile extras the mock answers with. Only the second seeded user, Ada (users[1]), carries them, so a test can tell a
// rich profile from a bare one (Chen, users[3], has none). Per-server values apply when the request names the first
// guild (HQ), where Ada is a member, and the per-server theme replaces the user-level one (as the live parser does).
static void mock_profile_extras(const mock *m, ccord_user_profile *p, ccord_id guild_id)
{
  p->has_banner_color = true;
  p->banner_color = 0x2B6CB0u;
  p->has_theme = true;
  p->theme_primary = 0x5865F2u;
  p->theme_accent = 0xEB459Eu;
  p->badge_count = 2;
  (void)snprintf(p->badges[0].id, sizeof p->badges[0].id, "%s", "early_supporter");
  (void)snprintf(p->badges[0].description, sizeof p->badges[0].description, "%s", "Early Supporter");
  (void)snprintf(p->badges[0].icon_url, sizeof p->badges[0].icon_url, "%s",
                 "https://cdn.discordapp.com/badge-icons/0f2a6c1e9b7d4a35.png");
  (void)snprintf(p->badges[1].id, sizeof p->badges[1].id, "%s", "bug_hunter");
  (void)snprintf(p->badges[1].description, sizeof p->badges[1].description, "%s", "Bug Hunter");
  (void)snprintf(p->badges[1].icon_url, sizeof p->badges[1].icon_url, "%s",
                 "https://cdn.discordapp.com/badge-icons/5c8e3b0a2d9f4e71.png");
  p->avatar_decoration_url =
    "https://cdn.discordapp.com/avatar-decoration-presets/9e41c7d2b0a8f356.png?size=160&passthrough=false";
  if (guild_id == CCORD_ID_NONE || guild_id != m->guilds[0].id) return;
  const ccord_member *row = nullptr;
  for (size_t mi = 0; mi < m->nmembers && !row; mi++) {
    if (m->members[mi].guild_id == guild_id && m->members[mi].user_id == p->user_id) row = &m->members[mi];
  }
  if (!row) return;
  p->guild_id = guild_id;
  p->guild_nick = "Ada the Builder";
  p->guild_avatar_url = "https://cdn.discordapp.com/guilds/2001/users/1002/avatars/7d3f0a9e5c21b84f.png?size=64";
  p->guild_banner_url = "https://cdn.discordapp.com/guilds/2001/users/1002/banners/c4b19e2d70a5f368.png?size=64";
  p->guild_bio = "Runs the HQ build bots.";
  p->guild_pronouns = "she/they";
  p->theme_primary = 0xFAA61Au;
  p->theme_accent = 0x3BA55Cu;
  p->guild_joined_at_ms = row->joined_at_ms;
  p->premium_since_ms = row->joined_at_ms + 10ll * 86400000ll;
}

ccord_status mock_request_profile_ex(ccord_transport *t, ccord_id user_id, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_PROFILE]++;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid user id");
  const ccord_user *u = nullptr;
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == user_id) {
      u = &m->users[i];
      break;
    }
  }
  if (!u) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown user");

  mock_item it = mk_make_item(CCORD_NET_USER_PROFILE, mk_now_of(m) + m->cfg.latency_ms);
  it.profile = ccord_alloc_array(1, sizeof *it.profile);
  if (!it.profile) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.profile = (ccord_user_profile){
    .user_id = u->id,
    .display_name = u->name,
    .handle = u->handle,
    .avatar_url = u->avatar_url,
    .banner_url = u->banner_url,
    .accent_color = u->accent_color,
    .bio = u->bio,
    .pronouns = u->pronouns,
    .guild_nick = "",
    .guild_avatar_url = "",
    .guild_banner_url = "",
    .guild_bio = "",
    .guild_pronouns = "",
    .avatar_decoration_url = "",
  };
  (void)snprintf(it.profile->clan_tag, sizeof it.profile->clan_tag, "%s", u->clan_tag);
  it.profile->clan_guild_id = u->clan_guild_id;
  (void)snprintf(it.profile->clan_badge, sizeof it.profile->clan_badge, "%s", u->clan_badge);
  if (u->id == m->users[1].id) mock_profile_extras(m, it.profile, guild_id);
  if (!u->is_bot) {
    size_t cc = 0;
    (void)snprintf(it.profile->connections[cc].type, sizeof it.profile->connections[cc].type, "github");
    (void)snprintf(it.profile->connections[cc].name, sizeof it.profile->connections[cc].name, "%s",
                   u->handle ? u->handle : u->name);
    it.profile->connections[cc].verified = true;
    cc++;
    if ((u->id % 2u) == 0u) {
      (void)snprintf(it.profile->connections[cc].type, sizeof it.profile->connections[cc].type, "website");
      (void)snprintf(it.profile->connections[cc].name, sizeof it.profile->connections[cc].name, "ccord.dev");
      it.profile->connections[cc].verified = true;
      cc++;
    }
    it.profile->connection_count = cc;
  }
  if (u->id != m->self_id) {
    for (size_t gi = 0; gi < m->nguilds && it.profile->mutual_guild_count < CCORD_PROFILE_MAX_MUTUAL_GUILDS; gi++) {
      ccord_id gid = m->guilds[gi].id;
      bool self_in = false, other_in = false;
      for (size_t mi = 0; mi < m->nmembers; mi++) {
        if (m->members[mi].guild_id != gid) continue;
        if (m->members[mi].user_id == m->self_id) self_in = true;
        if (m->members[mi].user_id == u->id) other_in = true;
      }
      if (self_in && other_in) {
        it.profile->mutual_guild_ids[it.profile->mutual_guild_count++] = gid;
      }
    }
    for (size_t ri = 0; ri < m->nrelationships && it.profile->mutual_friend_count < CCORD_PROFILE_MAX_MUTUAL_FRIENDS; ri++) {
      if (m->relationships[ri].kind != CCORD_RELATIONSHIP_FRIEND) continue;
      ccord_id fid = m->relationships[ri].user_id;
      if (fid == u->id || fid == m->self_id) continue;
      for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
        if (m->users[ui].id == fid) {
          ccord_mutual_friend *mf = &it.profile->mutual_friends[it.profile->mutual_friend_count++];
          mf->user_id = fid;
          (void)snprintf(mf->name, sizeof mf->name, "%s", m->users[ui].name ? m->users[ui].name : "");
          (void)snprintf(mf->handle, sizeof mf->handle, "%s", m->users[ui].handle ? m->users[ui].handle : "");
          (void)snprintf(mf->avatar_url, sizeof mf->avatar_url, "%s",
                         m->users[ui].avatar_url ? m->users[ui].avatar_url : "");
          break;
        }
      }
    }
    it.profile->mutual_friend_total = (uint32_t)it.profile->mutual_friend_count;
  }
  it.ev.user_id = u->id;
  it.ev.user_profile = it.profile;
  it.ev.users = u;
  it.ev.user_count = 1;
  return mk_push_item(m, &it, err);
}

ccord_status mock_request_profile(ccord_transport *t, ccord_id user_id, ccord_error *err)
{
  return mock_request_profile_ex(t, user_id, CCORD_ID_NONE, err);
}


size_t ccord_mock_presence_send_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->op3_count : 0;
}

ccord_presence ccord_mock_last_presence_send(const ccord_transport *t, const char **custom_text)
{
  const mock *m = (const mock *)t;
  if (custom_text) *custom_text = m ? m->op3_text : "";
  return m ? m->op3_presence : CCORD_PRESENCE_OFFLINE;
}

// Sets the mock account's own activity (a Playing line, or none) in the dataset the next SNAPSHOT is built from.
static void mock_set_self_activity_fields(mock *m, const char *name)
{
  ccord_user *u = &m->users[0];
  u->has_activity = name && name[0] != '\0';
  u->activity_type = CCORD_ACTIVITY_PLAYING;
  memset(u->activity_name, 0, sizeof u->activity_name);
  memset(u->activity_details, 0, sizeof u->activity_details);
  memset(u->activity_state, 0, sizeof u->activity_state);
  u->activity_start_ms = 0;
  u->activity_end_ms = 0;
  if (u->has_activity) (void)snprintf(u->activity_name, sizeof u->activity_name, "%s", name);
}

void ccord_mock_set_own_presence_echo(ccord_transport *t, bool echo)
{
  if (t) ((mock *)t)->own_presence_silent = !echo;
}

ccord_status ccord_mock_set_self_activity(ccord_transport *t, const char *name, bool echo, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock_set_self_activity_fields(m, name);
  if (!echo) return CCORD_OK;
  mock_item it = mk_make_item(CCORD_NET_PRESENCE_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.user_id = m->self_id;
  it.ev.presence = m->users[0].presence;
  it.ev.status_text = "";
  it.ev.has_presence_detail = true;
  (void)snprintf(it.ev.custom_status_emoji, sizeof it.ev.custom_status_emoji, "%s", m->users[0].custom_status_emoji);
  (void)snprintf(it.ev.custom_status_text, sizeof it.ev.custom_status_text, "%s", m->users[0].custom_status_text);
  it.ev.custom_status_expires_ms = m->users[0].custom_status_expires_ms;
  it.ev.has_activity = m->users[0].has_activity;
  it.ev.activity_type = (ccord_activity_type)m->users[0].activity_type;
  (void)snprintf(it.ev.activity_name, sizeof it.ev.activity_name, "%s", m->users[0].activity_name);
  return mk_push_item(m, &it, err);
}

ccord_status ccord_mock_inject_sessions_replace_activity(ccord_transport *t, ccord_presence overall,
                                                         const char *activity_name, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock_set_self_activity_fields(m, activity_name);
  mock_item it = mk_make_item(CCORD_NET_SESSIONS_REPLACE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.presence = overall;
  it.ev.has_presence_detail = true;
  it.ev.has_activity = m->users[0].has_activity;
  it.ev.activity_type = (ccord_activity_type)m->users[0].activity_type;
  (void)snprintf(it.ev.activity_name, sizeof it.ev.activity_name, "%s", m->users[0].activity_name);
  return mk_push_item(m, &it, err);
}

ccord_status ccord_mock_inject_sessions_replace(ccord_transport *t, ccord_presence overall, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: null transport");
  mock_item it = mk_make_item(CCORD_NET_SESSIONS_REPLACE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.presence = overall;
  return mk_push_item(m, &it, err);
}

void ccord_mock_set_status_proto_base(ccord_transport *t, const uint8_t *proto, size_t len)
{
  if (!t || (len > 0 && !proto)) return;
  mock *m = (mock *)t;
  uint8_t *copy = ccord_alloc_array(len ? len : 1u, 1);
  if (!copy) return;
  if (len > 0) memcpy(copy, proto, len);
  ccord_free(m->status1);
  m->status1 = copy;
  m->status1_len = len;
}

const uint8_t *ccord_mock_status_proto_base(const ccord_transport *t, size_t *len)
{
  const mock *m = (const mock *)t;
  if (len) *len = m ? m->status1_len : 0;
  return m ? m->status1 : nullptr;
}

size_t ccord_mock_status_proto_patch_count(const ccord_transport *t)
{
  return t ? ((const mock *)t)->nstatus1_patches : 0;
}

const uint8_t *ccord_mock_status_proto_patch_at(const ccord_transport *t, size_t i, size_t *len)
{
  const mock *m = (const mock *)t;
  if (len) *len = 0;
  if (!m || i >= m->nstatus1_patches) return nullptr;
  if (len) *len = m->status1_patches[i].len;
  return m->status1_patches[i].raw;
}

const char *ccord_mock_status_proto_patch_body(const ccord_transport *t, size_t i)
{
  const mock *m = (const mock *)t;
  return m && i < m->nstatus1_patches ? m->status1_patches[i].body : nullptr;
}

ccord_status ccord_mock_inject_status_proto_update(ccord_transport *t, const uint8_t *proto, size_t len, bool partial,
                                                   ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m || (len > 0 && !proto)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad status proto update");
  uint8_t *ev_bytes = ccord_alloc_array(len ? len : 1u, 1);
  if (!ev_bytes) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (len > 0) memcpy(ev_bytes, proto, len);
  if (!partial) ccord_mock_set_status_proto_base(t, proto, len);   // the server now holds what the other client wrote
  mock_item it = mk_make_item(CCORD_NET_USER_SETTINGS_PROTO, mk_now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = ev_bytes;   // owned by the item; the event borrows it
  it.ev.settings_proto1 = ev_bytes;
  it.ev.settings_proto1_len = len;
  it.ev.settings_proto1_partial = partial;
  return mk_push_item(m, &it, err);
}

ccord_status ccord_mock_inject_settings_proto_update(ccord_transport *t, const uint8_t *proto, size_t len, bool partial,
                                                     ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (!m || (len > 0 && !proto)) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad settings proto update");
  uint8_t *ev_bytes = ccord_alloc_array(len ? len : 1u, 1);
  if (!ev_bytes) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  if (len > 0) memcpy(ev_bytes, proto, len);
  if (!partial) {   // the server now holds what the other client wrote: later GETs and PATCH bases see it
    uint8_t *own = ccord_alloc_array(len ? len : 1u, 1);
    if (!own) {
      ccord_free(ev_bytes);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    if (len > 0) memcpy(own, proto, len);
    ccord_free(m->settings_proto);
    m->settings_proto = own;
    m->settings_proto_len = len;
    m->has_settings_proto = true;
  }
  mock_item it = mk_make_item(CCORD_NET_USER_SETTINGS_PROTO, mk_now_of(m) + m->cfg.latency_ms);
  it.raw_proto_own = ev_bytes;
  it.ev.raw_proto = ev_bytes;
  it.ev.raw_proto_len = len;
  it.ev.settings_proto1_partial = partial;
  return mk_push_item(m, &it, err);
}


