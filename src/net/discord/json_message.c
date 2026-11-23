// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "json_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void message_reactions(const ccord_json *j, uint32_t *count, bool *self)
{
  if (!j) {
    *count = 0;
    *self = false;
    return;
  }
  const ccord_json *arr = ccord_json_get(j, "reactions");
  size_t n = arr ? ccord_json_array_len(arr) : 0;
  int64_t total = 0;
  bool me = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *r = ccord_json_array_at(arr, i);
    int64_t c = dj_json_i64(r, "count", 0);
    if (c <= 0 && r) {
      const ccord_json *cd = ccord_json_get(r, "count_details");
      if (cd && ccord_json_type_of(cd) == CCORD_JSON_OBJECT) {
        int64_t nc = dj_json_i64(cd, "normal", 0);
        int64_t bc = dj_json_i64(cd, "burst", 0);
        if (nc < 0) nc = 0;
        if (bc < 0) bc = 0;
        c = nc + bc;
      }
    }
    if (c > 0) total += c;
    const ccord_json *mj = r ? ccord_json_get(r, "me") : nullptr;
    const ccord_json *mbj = r ? ccord_json_get(r, "me_burst") : nullptr;
    if ((mj && ccord_json_bool(mj, false)) || (mbj && ccord_json_bool(mbj, false))) me = true;
  }
  if (total < 0) total = 0;
  if (total > (int64_t)UINT32_MAX) total = (int64_t)UINT32_MAX;
  *count = (uint32_t)total;
  *self = me;
}

// Formats a Discord emoji JSON object into a canonical UTF-8 reaction identifier:
// standard Unicode emojis keep `name`, custom emojis with a snowflake `id` format as `name:id`.
const char *dj_format_reaction_emoji(ccord_arena *a, const ccord_json *emj, ccord_id *out_id,
                                         bool *out_animated)
{
  if (out_id) *out_id = CCORD_ID_NONE;
  if (out_animated) *out_animated = false;
  if (!emj) return nullptr;
  ccord_id eid = CCORD_ID_NONE;
  (void)dj_json_id_key(emj, "id", &eid);
  const ccord_json *aj = ccord_json_get(emj, "animated");
  bool anim = aj && ccord_json_bool(aj, false);
  const char *name = dj_jstr(emj, "name");
  if (eid != CCORD_ID_NONE) {
    if (!name || name[0] == '\0' || !dj_text_ok(name, CCORD_EMOJI_MAX_BYTES)) name = "emoji";
    char idbuf[21];
    int idlen = ccord_id_format(eid, idbuf);
    if (idlen <= 0) return nullptr;
    size_t max_nlen = CCORD_EMOJI_MAX_BYTES > (size_t)idlen + 1u
                        ? CCORD_EMOJI_MAX_BYTES - (size_t)idlen - 1u
                        : 0u;
    size_t nlen = dj_utf8_trunc_len(name, strlen(name), max_nlen);
    size_t total_len = nlen + 1u + (size_t)idlen;
    char *buf = ccord_arena_alloc(a, total_len + 1u, 1);
    if (!buf) return nullptr;
    memcpy(buf, name, nlen);
    buf[nlen] = ':';
    memcpy(buf + nlen + 1u, idbuf, (size_t)idlen);
    buf[total_len] = '\0';
    if (out_id) *out_id = eid;
    if (out_animated) *out_animated = anim;
    return buf;
  }
  if (!dj_text_ok(name, CCORD_EMOJI_MAX_BYTES)) return nullptr;
  return ccord_arena_strndup(a, name, strlen(name));
}

// Fills the per-emoji breakdown from a Discord message's `reactions` array.
static void map_reaction_list(ccord_arena *a, const ccord_json *j, ccord_message *out)
{
  if (!j) return;
  const ccord_json *arr = ccord_json_get(j, "reactions");
  size_t n = arr ? ccord_json_array_len(arr) : 0;
  if (n == 0) return;
  if (n > CCORD_MAX_REACTIONS_PER_MESSAGE) n = CCORD_MAX_REACTIONS_PER_MESSAGE;
  ccord_reaction *rs = ccord_arena_alloc(a, n * sizeof *rs, _Alignof(ccord_reaction));
  if (!rs) return;
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *r = ccord_json_array_at(arr, i);
    const ccord_json *emj = r ? ccord_json_get(r, "emoji") : nullptr;
    ccord_id eid = CCORD_ID_NONE;
    bool anim = false;
    const char *emoji_str = dj_format_reaction_emoji(a, emj, &eid, &anim);
    if (!emoji_str) continue;
    const ccord_json *cd = ccord_json_get(r, "count_details");
    int64_t bc = (cd && ccord_json_type_of(cd) == CCORD_JSON_OBJECT) ? dj_json_i64(cd, "burst", 0) : 0;
    if (bc < 0) bc = 0;
    if (bc > (int64_t)UINT32_MAX) bc = (int64_t)UINT32_MAX;
    int64_t c = dj_json_i64(r, "count", 0);
    if (c <= 0 && bc > 0) {
      int64_t nc = dj_json_i64(cd, "normal", 0);
      if (nc < 0) nc = 0;
      c = nc + bc;
    }
    if (c <= 0) continue;
    if (c > (int64_t)UINT32_MAX) c = (int64_t)UINT32_MAX;
    if (bc > c) bc = c;
    const ccord_json *mj = ccord_json_get(r, "me");
    const ccord_json *mbj = ccord_json_get(r, "me_burst");
    bool me_norm = mj && ccord_json_bool(mj, false);
    bool me_burst = mbj && ccord_json_bool(mbj, false);
    rs[kept++] = (ccord_reaction){
      .emoji = emoji_str,
      .emoji_id = eid,
      .count = (uint32_t)c,
      .burst_count = (uint32_t)bc,
      .self_reacted = me_norm,
      .me_burst = me_burst,
      .animated = anim,
    };
  }
  if (kept) {
    out->reactions = rs;
    out->reaction_kind_count = kept;
  }
}

static const char *clamp_message_text(ccord_arena *a, const char *s)
{
  if (!s || s[0] == '\0') return "";
  size_t len = strlen(s);
  size_t n = (len <= CCORD_MESSAGE_MAX_BYTES) ? len : dj_utf8_trunc_len(s, len, CCORD_MESSAGE_MAX_BYTES);
  if (n == 0) return "";
  const char *out = ccord_arena_strndup(a, s, n);
  return out ? out : "";
}

// ---------------------------------------------------------------- rich message data
// Attachments, embeds, stickers, mentions, the replied-to message, a forwarded snapshot, a poll and a started thread,
// copied into the event's arena as a ccord_message_rich (the store takes its own deep copy). Anything over a cap is
// dropped rather than failing the message; text over its cap is cut on a character boundary; an over-long URL is
// dropped (half a link is worse than none).

uint32_t dj_clamp_u32(int64_t v)
{
  if (v < 0) return 0u;
  return v > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

bool dj_is_json_object(const ccord_json *j)
{
  return j && ccord_json_type_of(j) == CCORD_JSON_OBJECT;
}

bool dj_is_json_array(const ccord_json *j)
{
  return j && ccord_json_type_of(j) == CCORD_JSON_ARRAY;
}

// An arena copy of obj[key], "" when it is missing, not text, or not valid UTF-8; cut to `max` bytes.
static const char *astr(ccord_arena *a, const ccord_json *obj, const char *key, size_t max)
{
  const char *s = dj_jstr(obj, key);
  if (!s || s[0] == '\0') return "";
  size_t n = dj_utf8_trunc_len(s, strlen(s), max);
  if (n == 0) return "";
  char *d = ccord_arena_strndup(a, s, n);
  if (!d || !ccord_utf8_valid_text(d, n)) return "";
  return d;
}

static const char *aurl(ccord_arena *a, const ccord_json *obj, const char *key)
{
  const char *s = dj_jstr(obj, key);
  if (!s || s[0] == '\0' || strlen(s) > CCORD_RICH_URL_MAX_BYTES) return "";
  return astr(a, obj, key, CCORD_RICH_URL_MAX_BYTES);
}

static void *rich_alloc(ccord_arena *a, size_t n, size_t size)
{
  if (n == 0) return nullptr;
  void *p = ccord_arena_alloc(a, n * size, _Alignof(max_align_t));
  if (p) memset(p, 0, n * size);
  return p;
}

static void rich_media(ccord_arena *a, const ccord_json *m, ccord_embed_media *out)
{
  *out = (ccord_embed_media){.url = "", .proxy_url = ""};
  if (!dj_is_json_object(m)) return;
  out->url = aurl(a, m, "url");
  out->proxy_url = aurl(a, m, "proxy_url");
  out->width = dj_clamp_u32(dj_json_i64(m, "width", 0));
  out->height = dj_clamp_u32(dj_json_i64(m, "height", 0));
}

static bool starts_with_spoiler(const char *name)
{
  return name && strncmp(name, "SPOILER_", 8) == 0;
}

// A voice-message waveform is base64 text. Anything longer than the cap, or with a byte outside the base64
// alphabet, is dropped whole (a truncated waveform would decode to the wrong bars) so the attachment still parses.
static const char *awaveform(ccord_arena *a, const ccord_json *obj)
{
  const char *s = dj_jstr(obj, "waveform");
  if (!s || s[0] == '\0') return "";
  size_t n = strlen(s);
  if (n > CCORD_RICH_WAVEFORM_MAX_BYTES) return "";
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    bool ok = (c >= (unsigned char)'A' && c <= (unsigned char)'Z') || (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
              (c >= (unsigned char)'0' && c <= (unsigned char)'9') || c == (unsigned char)'+' || c == (unsigned char)'/' ||
              c == (unsigned char)'=';
    if (!ok) return "";
  }
  char *d = ccord_arena_strndup(a, s, n);
  return d ? d : "";
}

static ccord_attachment *rich_attachments(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_ATTACHMENTS) n = CCORD_RICH_MAX_ATTACHMENTS;
  ccord_attachment *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *j = ccord_json_array_at(arr, i);
    if (!dj_is_json_object(j)) continue;
    ccord_attachment at = {0};
    (void)dj_json_id_key(j, "id", &at.id);
    at.filename = astr(a, j, "filename", CCORD_RICH_SHORT_MAX_BYTES);
    at.description = astr(a, j, "description", CCORD_RICH_TEXT_MAX_BYTES);
    at.content_type = astr(a, j, "content_type", CCORD_RICH_SHORT_MAX_BYTES);
    at.url = aurl(a, j, "url");
    at.proxy_url = aurl(a, j, "proxy_url");
    if (at.url[0] == '\0' && at.proxy_url[0] == '\0' && at.filename[0] == '\0') continue;   // nothing to show or fetch
    int64_t size = dj_json_i64(j, "size", 0);
    at.size = size > 0 ? (uint64_t)size : 0u;
    at.width = dj_clamp_u32(dj_json_i64(j, "width", 0));
    at.height = dj_clamp_u32(dj_json_i64(j, "height", 0));
    const ccord_json *dj = ccord_json_get(j, "duration_secs");
    double secs = dj ? ccord_json_number(dj, 0.0) : 0.0;
    at.duration_ms = (secs > 0.0 && secs < 4000000.0) ? (uint32_t)(secs * 1000.0) : 0u;
    at.waveform_b64 = awaveform(a, j);
    at.flags = dj_clamp_u32(dj_json_i64(j, "flags", 0));
    at.spoiler = starts_with_spoiler(at.filename);
    out[(*count)++] = at;
  }
  return *count ? out : nullptr;
}

static ccord_embed_kind embed_kind_of(const char *t)
{
  if (!t || t[0] == '\0' || strcmp(t, "rich") == 0) return CCORD_EMBED_RICH;
  if (strcmp(t, "image") == 0) return CCORD_EMBED_IMAGE;
  if (strcmp(t, "video") == 0) return CCORD_EMBED_VIDEO;
  if (strcmp(t, "gifv") == 0) return CCORD_EMBED_GIFV;
  if (strcmp(t, "article") == 0) return CCORD_EMBED_ARTICLE;
  if (strcmp(t, "link") == 0) return CCORD_EMBED_LINK;
  if (strcmp(t, "poll_result") == 0) return CCORD_EMBED_POLL_RESULT;
  return CCORD_EMBED_OTHER;
}

static ccord_embed *rich_embeds(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_EMBEDS) n = CCORD_RICH_MAX_EMBEDS;
  ccord_embed *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *j = ccord_json_array_at(arr, i);
    if (!dj_is_json_object(j)) continue;
    ccord_embed e = {0};
    e.kind = embed_kind_of(dj_jstr(j, "type"));
    e.title = astr(a, j, "title", CCORD_RICH_SHORT_MAX_BYTES * 2u);
    e.description = astr(a, j, "description", CCORD_RICH_TEXT_MAX_BYTES);
    e.url = aurl(a, j, "url");
    const char *ts = dj_jstr(j, "timestamp");
    int64_t t = ts ? dj_iso_ms(ts, strlen(ts)) : -1;
    e.timestamp_ms = t > 0 ? t : 0;
    const ccord_json *cj = ccord_json_get(j, "color");
    int64_t color = -1;
    if (cj && ccord_json_int64(cj, &color) && color >= 0 && color <= 0xFFFFFF) {
      e.color = (uint32_t)color;
      e.has_color = true;
    }
    const ccord_json *prov = ccord_json_get(j, "provider");
    e.provider = dj_is_json_object(prov) ? astr(a, prov, "name", CCORD_RICH_SHORT_MAX_BYTES) : "";
    const ccord_json *au = ccord_json_get(j, "author");
    e.author_name = e.author_url = e.author_icon = "";
    if (dj_is_json_object(au)) {
      e.author_name = astr(a, au, "name", CCORD_RICH_SHORT_MAX_BYTES);
      e.author_url = aurl(a, au, "url");
      // Icons are drawn, never opened: prefer Discord's proxied copy (the origin host is not fetched directly).
      e.author_icon = aurl(a, au, "proxy_icon_url");
      if (e.author_icon[0] == '\0') e.author_icon = aurl(a, au, "icon_url");
    }
    const ccord_json *ft = ccord_json_get(j, "footer");
    e.footer_text = e.footer_icon = "";
    if (dj_is_json_object(ft)) {
      e.footer_text = astr(a, ft, "text", CCORD_RICH_TEXT_MAX_BYTES / 2u);
      e.footer_icon = aurl(a, ft, "proxy_icon_url");
      if (e.footer_icon[0] == '\0') e.footer_icon = aurl(a, ft, "icon_url");
    }
    rich_media(a, ccord_json_get(j, "image"), &e.image);
    rich_media(a, ccord_json_get(j, "thumbnail"), &e.thumbnail);
    rich_media(a, ccord_json_get(j, "video"), &e.video);
    const ccord_json *fa = ccord_json_get(j, "fields");
    size_t nf = dj_is_json_array(fa) ? ccord_json_array_len(fa) : 0;
    if (nf > CCORD_RICH_MAX_EMBED_FIELDS) nf = CCORD_RICH_MAX_EMBED_FIELDS;
    ccord_embed_field *fields = rich_alloc(a, nf, sizeof *fields);
    size_t kept = 0;
    for (size_t k = 0; fields && k < nf; k++) {
      const ccord_json *fj = ccord_json_array_at(fa, k);
      if (!dj_is_json_object(fj)) continue;
      const char *name = astr(a, fj, "name", CCORD_RICH_SHORT_MAX_BYTES);
      const char *value = astr(a, fj, "value", CCORD_RICH_FIELD_VALUE_MAX_BYTES);
      if (name[0] == '\0' && value[0] == '\0') continue;
      const ccord_json *ij = ccord_json_get(fj, "inline");
      fields[kept++] = (ccord_embed_field){.name = name, .value = value, .inline_ = ij && ccord_json_bool(ij, false)};
    }
    e.fields = kept ? fields : nullptr;
    e.field_count = kept;
    out[(*count)++] = e;
  }
  return *count ? out : nullptr;
}

static ccord_msg_sticker *rich_stickers(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_STICKERS) n = CCORD_RICH_MAX_STICKERS;
  ccord_msg_sticker *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *j = ccord_json_array_at(arr, i);
    if (!dj_is_json_object(j)) continue;
    ccord_msg_sticker st = {0};
    if (!dj_json_id_key(j, "id", &st.id)) continue;
    st.name = astr(a, j, "name", CCORD_RICH_SHORT_MAX_BYTES);
    int64_t fmt = dj_json_i64(j, "format_type", 0);
    st.format = (fmt >= 1 && fmt <= 4) ? (uint8_t)fmt : 0u;
    out[(*count)++] = st;
  }
  return *count ? out : nullptr;
}

static void rich_poll(ccord_arena *a, const ccord_json *p, ccord_poll *out)
{
  *out = (ccord_poll){.question = ""};
  if (!dj_is_json_object(p)) return;
  const ccord_json *q = ccord_json_get(p, "question");
  const char *qtext = dj_is_json_object(q) ? astr(a, q, "text", CCORD_RICH_SHORT_MAX_BYTES * 2u) : "";
  out->present = true;
  out->question = qtext[0] != '\0' ? qtext : "Poll";
  const ccord_json *ex = ccord_json_get(p, "expiry");
  const char *exs = (ex && ccord_json_type_of(ex) == CCORD_JSON_STRING) ? dj_jstr(p, "expiry") : nullptr;
  int64_t exm = exs ? dj_iso_ms(exs, strlen(exs)) : -1;
  out->expiry_ms = exm > 0 ? exm : 0;
  const ccord_json *ms = ccord_json_get(p, "allow_multiselect");
  out->multiselect = ms && ccord_json_bool(ms, false);
  const ccord_json *answers = ccord_json_get(p, "answers");
  size_t n = dj_is_json_array(answers) ? ccord_json_array_len(answers) : 0;
  if (n > CCORD_RICH_MAX_POLL_ANSWERS) n = CCORD_RICH_MAX_POLL_ANSWERS;
  out->answers = n > 0 ? rich_alloc(a, n, sizeof *out->answers) : nullptr;
  if (n > 0 && !out->answers) n = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *aj = ccord_json_array_at(answers, i);
    if (!dj_is_json_object(aj)) continue;
    ccord_poll_answer *pa = &out->answers[out->answer_count];
    *pa = (ccord_poll_answer){.text = "", .emoji = ""};
    pa->id = dj_clamp_u32(dj_json_i64(aj, "answer_id", (int64_t)i + 1));
    const ccord_json *media = ccord_json_get(aj, "poll_media");
    if (dj_is_json_object(media)) {
      pa->text = astr(a, media, "text", CCORD_RICH_SHORT_MAX_BYTES);
      const ccord_json *em = ccord_json_get(media, "emoji");
      if (dj_is_json_object(em)) {
        pa->emoji = astr(a, em, "name", CCORD_EMOJI_MAX_BYTES);
        (void)dj_json_id_key(em, "id", &pa->emoji_id);
      }
    }
    out->answer_count++;
  }
  const ccord_json *res = ccord_json_get(p, "results");
  if (dj_is_json_object(res)) {
    const ccord_json *fin = ccord_json_get(res, "is_finalized");
    out->finalized = fin && ccord_json_bool(fin, false);
    const ccord_json *counts = ccord_json_get(res, "answer_counts");
    size_t nc = dj_is_json_array(counts) ? ccord_json_array_len(counts) : 0;
    for (size_t i = 0; i < nc; i++) {
      const ccord_json *cj = ccord_json_array_at(counts, i);
      if (!dj_is_json_object(cj)) continue;
      uint32_t aid = dj_clamp_u32(dj_json_i64(cj, "id", 0));
      for (size_t k = 0; k < out->answer_count; k++) {
        if (out->answers[k].id != aid) continue;
        out->answers[k].votes = dj_clamp_u32(dj_json_i64(cj, "count", 0));
        const ccord_json *me = ccord_json_get(cj, "me_voted");
        out->answers[k].me_voted = me && ccord_json_bool(me, false);
      }
    }
  }
}

static const char *display_name_of(ccord_arena *a, const ccord_json *user)
{
  const char *g = dj_jstr(user, "global_name");
  if (g && g[0] != '\0') return astr(a, user, "global_name", CCORD_RICH_SHORT_MAX_BYTES);
  return astr(a, user, "username", CCORD_RICH_SHORT_MAX_BYTES);
}

static void rich_ref_preview(ccord_arena *a, const ccord_json *j, ccord_ref_preview *out)
{
  *out = (ccord_ref_preview){.author_name = "", .content = ""};
  const ccord_json *rm = ccord_json_get(j, "referenced_message");
  if (!rm) return;                                   // Discord did not say: unknown, not deleted
  if (!dj_is_json_object(rm)) {                         // null: the message is gone
    out->deleted = true;
    return;
  }
  out->present = true;
  const ccord_json *au = ccord_json_get(rm, "author");
  if (dj_is_json_object(au)) {
    (void)dj_json_id_key(au, "id", &out->author_id);
    out->author_name = display_name_of(a, au);
  }
  out->content = astr(a, rm, "content", CCORD_RICH_SNIPPET_MAX_BYTES);
  out->has_attachments = dj_jarray_len(rm, "attachments") > 0;
}

static void rich_forward(ccord_arena *a, const ccord_json *j, ccord_forward *out)
{
  *out = (ccord_forward){.content = ""};
  const ccord_json *snaps = ccord_json_get(j, "message_snapshots");
  if (!dj_is_json_array(snaps) || ccord_json_array_len(snaps) == 0) return;
  const ccord_json *first = ccord_json_array_at(snaps, 0);
  const ccord_json *msg = dj_is_json_object(first) ? ccord_json_get(first, "message") : nullptr;
  if (!dj_is_json_object(msg)) return;
  out->present = true;
  out->content = astr(a, msg, "content", CCORD_RICH_TEXT_MAX_BYTES);
  const char *ts = dj_jstr(msg, "timestamp");
  int64_t t = ts ? dj_iso_ms(ts, strlen(ts)) : -1;
  out->timestamp_ms = t > 0 ? t : 0;
  out->attachments = rich_attachments(a, ccord_json_get(msg, "attachments"), &out->attachment_count);
  out->embeds = rich_embeds(a, ccord_json_get(msg, "embeds"), &out->embed_count);
  const ccord_json *st = ccord_json_get(msg, "sticker_items");
  if (!st) st = ccord_json_get(msg, "stickers");
  out->stickers = rich_stickers(a, st, &out->sticker_count);
}

static ccord_id *rich_user_ids(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_MENTIONS) n = CCORD_RICH_MAX_MENTIONS;
  ccord_id *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    ccord_id id = CCORD_ID_NONE;
    if (dj_json_id_key(ccord_json_array_at(arr, i), "id", &id)) out[(*count)++] = id;
  }
  return *count ? out : nullptr;
}

static ccord_id *rich_role_ids(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_MENTIONS) n = CCORD_RICH_MAX_MENTIONS;
  ccord_id *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    ccord_id id = CCORD_ID_NONE;
    if (dj_json_id(ccord_json_array_at(arr, i), &id)) out[(*count)++] = id;
  }
  return *count ? out : nullptr;
}

static ccord_select_option *rich_select_options(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (n > CCORD_RICH_MAX_SELECT_OPTIONS) n = CCORD_RICH_MAX_SELECT_OPTIONS;
  ccord_select_option *out = rich_alloc(a, n, sizeof *out);
  for (size_t i = 0; out && i < n; i++) {
    const ccord_json *oj = ccord_json_array_at(arr, i);
    if (!dj_is_json_object(oj)) continue;
    const char *val = astr(a, oj, "value", CCORD_RICH_SHORT_MAX_BYTES);
    if (val[0] == '\0') continue;
    const char *lbl = astr(a, oj, "label", CCORD_RICH_SHORT_MAX_BYTES);
    const ccord_json *defj = ccord_json_get(oj, "default");
    ccord_select_option *o = &out[(*count)++];
    *o = (ccord_select_option){
      .label = lbl[0] != '\0' ? lbl : val,
      .value = val,
      .description = astr(a, oj, "description", CCORD_RICH_SHORT_MAX_BYTES),
      .emoji = "",
      .default_ = defj && ccord_json_bool(defj, false),
    };
    const ccord_json *ej = ccord_json_get(oj, "emoji");
    if (dj_is_json_object(ej)) {
      o->emoji = astr(a, ej, "name", CCORD_EMOJI_MAX_BYTES);
      (void)dj_json_id_key(ej, "id", &o->emoji_id);
    }
  }
  return *count ? out : nullptr;
}

// A button or select menu: the mapping v1 rows and the v2 tree share. False when the component cannot be kept
// (a link button without a url, an interactive component without a custom_id).
static bool comp_interactive(ccord_arena *a, const ccord_json *cj, int64_t ct, ccord_component *out)
{
  const ccord_json *disj = ccord_json_get(cj, "disabled");
  ccord_component c = {
    .kind = (ccord_component_kind)ct,
    .label = astr(a, cj, "label", CCORD_RICH_SHORT_MAX_BYTES),
    .custom_id = astr(a, cj, "custom_id", CCORD_RICH_CUSTOM_ID_MAX_BYTES),
    .url = astr(a, cj, "url", CCORD_RICH_URL_MAX_BYTES),
    .placeholder = astr(a, cj, "placeholder", CCORD_RICH_SHORT_MAX_BYTES),
    .emoji = "",
    .disabled = disj && ccord_json_bool(disj, false),
  };
  int64_t style = dj_json_i64(cj, "style", ct == CCORD_COMP_BUTTON ? CCORD_BTN_SECONDARY : 0);
  c.style = (style >= 0 && style <= UINT8_MAX) ? (uint8_t)style : 0u;
  const ccord_json *ej = ccord_json_get(cj, "emoji");
  if (dj_is_json_object(ej)) {
    c.emoji = astr(a, ej, "name", CCORD_EMOJI_MAX_BYTES);
    (void)dj_json_id_key(ej, "id", &c.emoji_id);
  }
  if (ct == CCORD_COMP_BUTTON) {
    if (c.style == CCORD_BTN_LINK && c.url[0] == '\0') return false;
    if (c.style != CCORD_BTN_LINK && c.style != CCORD_BTN_PREMIUM && c.custom_id[0] == '\0') return false;
  } else {
    if (c.custom_id[0] == '\0') return false;
    int64_t minv = dj_json_i64(cj, "min_values", 1);
    int64_t maxv = dj_json_i64(cj, "max_values", 1);
    c.min_values = minv < 0 ? 0u
                            : (minv > (int64_t)CCORD_RICH_MAX_SELECT_OPTIONS ? CCORD_RICH_MAX_SELECT_OPTIONS
                                                                             : (uint8_t)minv);
    c.max_values = maxv < 1 ? 1u
                            : (maxv > (int64_t)CCORD_RICH_MAX_SELECT_OPTIONS ? CCORD_RICH_MAX_SELECT_OPTIONS
                                                                             : (uint8_t)maxv);
    if (ct == CCORD_COMP_STRING_SELECT) {
      c.options = rich_select_options(a, ccord_json_get(cj, "options"), &c.option_count);
    }
  }
  *out = c;
  return true;
}

static ccord_component_row *rich_components(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t nr = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  if (nr > CCORD_RICH_MAX_COMPONENT_ROWS) nr = CCORD_RICH_MAX_COMPONENT_ROWS;
  ccord_component_row *rows = rich_alloc(a, nr, sizeof *rows);
  for (size_t ri = 0; rows && ri < nr; ri++) {
    const ccord_json *rj = ccord_json_array_at(arr, ri);
    if (!dj_is_json_object(rj) || dj_json_i64(rj, "type", 0) != CCORD_COMP_ACTION_ROW) continue;
    const ccord_json *carr = ccord_json_get(rj, "components");
    size_t nc = dj_is_json_array(carr) ? ccord_json_array_len(carr) : 0;
    if (nc > CCORD_RICH_MAX_ROW_COMPONENTS) nc = CCORD_RICH_MAX_ROW_COMPONENTS;
    ccord_component *cs = rich_alloc(a, nc, sizeof *cs);
    size_t kept = 0;
    for (size_t ci = 0; cs && ci < nc; ci++) {
      const ccord_json *cj = ccord_json_array_at(carr, ci);
      if (!dj_is_json_object(cj)) continue;
      int64_t ct = dj_json_i64(cj, "type", 0);
      if (ct < CCORD_COMP_BUTTON || ct > CCORD_COMP_CHANNEL_SELECT || ct == CCORD_COMP_TEXT_INPUT) continue;
      ccord_component c;
      if (!comp_interactive(a, cj, ct, &c)) continue;
      cs[kept++] = c;
    }
    if (kept > 0) {
      rows[(*count)++] = (ccord_component_row){.components = cs, .component_count = kept};
    }
  }
  return *count ? rows : nullptr;
}

// ---------------------------------------------------------------- components v2
// The IS_COMPONENTS_V2 tree: layout and content nodes instead of action rows. Parsing truncates (node budget, depth,
// per-kind caps) and skips what it cannot place or does not know, so a newer or oversized payload still renders the
// rest; the result always satisfies ccord_message_rich_validate.

// Where a v2 node sits, which decides the kinds it may be (the model's placement rules).
typedef enum { COMP2_TOP, COMP2_CONTAINER, COMP2_SECTION_TEXT, COMP2_ACCESSORY, COMP2_ROW } comp2_where;

static bool comp2_allowed(comp2_where where, int64_t t)
{
  switch (where) {
    case COMP2_TOP:
      return t == CCORD_COMP_CONTAINER || t == CCORD_COMP_ACTION_ROW || t == CCORD_COMP_SECTION ||
             t == CCORD_COMP_TEXT_DISPLAY || t == CCORD_COMP_MEDIA_GALLERY || t == CCORD_COMP_FILE ||
             t == CCORD_COMP_SEPARATOR;
    case COMP2_CONTAINER:
      return t == CCORD_COMP_ACTION_ROW || t == CCORD_COMP_SECTION || t == CCORD_COMP_TEXT_DISPLAY ||
             t == CCORD_COMP_MEDIA_GALLERY || t == CCORD_COMP_FILE || t == CCORD_COMP_SEPARATOR;
    case COMP2_SECTION_TEXT: return t == CCORD_COMP_TEXT_DISPLAY;
    case COMP2_ACCESSORY: return t == CCORD_COMP_THUMBNAIL || t == CCORD_COMP_BUTTON;
    case COMP2_ROW:
      return t == CCORD_COMP_BUTTON ||
             (t >= CCORD_COMP_STRING_SELECT && t <= CCORD_COMP_CHANNEL_SELECT && t != CCORD_COMP_TEXT_INPUT);
  }
  return false;
}

// One message's whole tree shares the node budget; only kept nodes count against it.
typedef struct { ccord_arena *a; size_t nodes; } comp2_ctx;

static bool comp2_parse(comp2_ctx *cx, const ccord_json *j, unsigned depth, comp2_where where, ccord_component *out);

// Up to `cap` children of one placement into a fresh arena array; returns how many were kept.
static size_t comp2_children(comp2_ctx *cx, const ccord_json *arr, unsigned depth, comp2_where where, size_t cap,
                             const ccord_component **out)
{
  *out = nullptr;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  ccord_component *kids = rich_alloc(cx->a, n < cap ? n : cap, sizeof *kids);   // skipped entries do not use up the cap
  size_t kept = 0;
  for (size_t i = 0; kids && i < n && kept < cap; i++) {
    ccord_component c;
    if (comp2_parse(cx, ccord_json_array_at(arr, i), depth, where, &c)) kids[kept++] = c;
  }
  if (kept == 0) return 0;
  *out = kids;
  return kept;
}

// An unfurled media item (Thumbnail `media`, gallery item `media`, File `file`). Description and spoiler live on the
// component or gallery item around it, not here.
static void comp2_media(ccord_arena *a, const ccord_json *m, ccord_media_ref *out)
{
  *out = (ccord_media_ref){.url = "", .proxy_url = "", .content_type = "", .description = ""};
  if (!dj_is_json_object(m)) return;
  out->url = aurl(a, m, "url");
  out->proxy_url = aurl(a, m, "proxy_url");
  out->width = dj_clamp_u32(dj_json_i64(m, "width", 0));
  out->height = dj_clamp_u32(dj_json_i64(m, "height", 0));
  out->content_type = astr(a, m, "content_type", CCORD_RICH_SHORT_MAX_BYTES);
}

static bool comp2_has_media(const ccord_media_ref *m)
{
  return m->url[0] != '\0' || m->proxy_url[0] != '\0';
}

// The per-kind fields of one v2 node, false when it cannot be kept (a required field is missing). Its budget slot is
// already reserved by comp2_parse; children reserve their own.
static bool comp2_fields(comp2_ctx *cx, const ccord_json *j, int64_t t, unsigned depth, ccord_component *c)
{
  ccord_arena *a = cx->a;
  switch (t) {
    case CCORD_COMP_BUTTON:
    case CCORD_COMP_STRING_SELECT:
    case CCORD_COMP_USER_SELECT:
    case CCORD_COMP_ROLE_SELECT:
    case CCORD_COMP_MENTIONABLE_SELECT:
    case CCORD_COMP_CHANNEL_SELECT:
      return comp_interactive(a, j, t, c);
    case CCORD_COMP_ACTION_ROW:
      c->child_count = comp2_children(cx, ccord_json_get(j, "components"), depth + 1u, COMP2_ROW,
                                      CCORD_RICH_MAX_ROW_COMPONENTS, &c->children);
      return c->child_count > 0;
    case CCORD_COMP_SECTION: {
      ccord_component acc;
      if (!comp2_parse(cx, ccord_json_get(j, "accessory"), depth + 1u, COMP2_ACCESSORY, &acc)) return false;
      c->child_count = comp2_children(cx, ccord_json_get(j, "components"), depth + 1u, COMP2_SECTION_TEXT,
                                      CCORD_RICH_MAX_V2_SECTION_TEXTS, &c->children);
      if (c->child_count == 0) return false;
      ccord_component *ap = rich_alloc(a, 1, sizeof *ap);
      if (!ap) return false;
      *ap = acc;
      c->accessory = ap;
      return true;
    }
    case CCORD_COMP_TEXT_DISPLAY:
      c->content = astr(a, j, "content", CCORD_RICH_TEXT_MAX_BYTES);
      return true;
    case CCORD_COMP_THUMBNAIL:
      comp2_media(a, ccord_json_get(j, "media"), &c->media);
      if (!comp2_has_media(&c->media)) return false;
      c->media.description = astr(a, j, "description", CCORD_RICH_FIELD_VALUE_MAX_BYTES);
      c->media.spoiler = dj_json_bool_key(j, "spoiler", false);
      return true;
    case CCORD_COMP_MEDIA_GALLERY: {
      const ccord_json *items = ccord_json_get(j, "items");
      size_t n = dj_is_json_array(items) ? ccord_json_array_len(items) : 0;
      if (n > CCORD_RICH_MAX_V2_GALLERY_ITEMS) n = CCORD_RICH_MAX_V2_GALLERY_ITEMS;
      ccord_media_ref *g = rich_alloc(a, n, sizeof *g);
      size_t kept = 0;
      for (size_t i = 0; g && i < n; i++) {
        const ccord_json *ij = ccord_json_array_at(items, i);
        if (!dj_is_json_object(ij)) continue;
        ccord_media_ref mr;
        comp2_media(a, ccord_json_get(ij, "media"), &mr);
        if (!comp2_has_media(&mr)) continue;
        mr.description = astr(a, ij, "description", CCORD_RICH_FIELD_VALUE_MAX_BYTES);
        mr.spoiler = dj_json_bool_key(ij, "spoiler", false);
        g[kept++] = mr;
      }
      if (kept == 0) return false;
      c->gallery = g;
      c->gallery_count = kept;
      return true;
    }
    case CCORD_COMP_FILE: {
      comp2_media(a, ccord_json_get(j, "file"), &c->media);
      if (!comp2_has_media(&c->media)) return false;
      c->spoiler = dj_json_bool_key(j, "spoiler", false);
      c->file_name = astr(a, j, "name", CCORD_RICH_SHORT_MAX_BYTES);
      int64_t sz = dj_json_i64(j, "size", 0);
      c->file_size = sz > 0 ? (uint64_t)sz : 0u;
      return true;
    }
    case CCORD_COMP_SEPARATOR: {
      c->divider = dj_json_bool_key(j, "divider", true);   // Discord's default is a visible divider
      int64_t sp = dj_json_i64(j, "spacing", 1);
      c->spacing = sp == 2 ? 2u : 1u;
      return true;
    }
    case CCORD_COMP_CONTAINER: {
      int64_t ac = dj_json_i64(j, "accent_color", -1);
      if (ac >= 0 && ac <= 0xFFFFFF) {
        c->accent_color = (uint32_t)ac;
        c->has_accent_color = true;
      }
      c->spoiler = dj_json_bool_key(j, "spoiler", false);
      c->child_count = comp2_children(cx, ccord_json_get(j, "components"), depth + 1u, COMP2_CONTAINER,
                                      CCORD_RICH_MAX_V2_NODES, &c->children);
      return true;
    }
    default: return false;
  }
}

// One node of the tree, false when it must be skipped (unknown kind, wrong placement, missing required field, or the
// budget/depth is spent). The slot is reserved before the children parse, so an oversized tree truncates its deepest
// nodes instead of dropping whole branches, and the kept tree always fits the budget.
static bool comp2_parse(comp2_ctx *cx, const ccord_json *j, unsigned depth, comp2_where where, ccord_component *out)
{
  if (!dj_is_json_object(j) || depth > CCORD_RICH_MAX_V2_DEPTH || cx->nodes >= CCORD_RICH_MAX_V2_NODES) return false;
  int64_t t = dj_json_i64(j, "type", 0);
  if (!comp2_allowed(where, t)) return false;
  size_t before = cx->nodes;
  cx->nodes++;
  ccord_component c = {.kind = (ccord_component_kind)t};
  if (!comp2_fields(cx, j, t, depth, &c)) {
    cx->nodes = before;   // also gives back the slots of children parsed before this node was dropped
    return false;
  }
  *out = c;
  return true;
}

static ccord_component *rich_components_v2(ccord_arena *a, const ccord_json *arr, size_t *count)
{
  *count = 0;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  ccord_component *nodes = rich_alloc(a, n < CCORD_RICH_MAX_V2_NODES ? n : CCORD_RICH_MAX_V2_NODES, sizeof *nodes);
  comp2_ctx cx = {.a = a, .nodes = 0};
  for (size_t i = 0; nodes && i < n && *count < CCORD_RICH_MAX_V2_NODES; i++) {
    ccord_component c;
    if (comp2_parse(&cx, ccord_json_array_at(arr, i), 1u, COMP2_TOP, &c)) nodes[(*count)++] = c;
  }
  return *count ? nodes : nullptr;
}

// A v2 payload: the IS_COMPONENTS_V2 flag says so, or a top-level node is a v2-only kind (covers an update that
// carries components without the flag).
static bool components_are_v2(const ccord_json *arr, uint32_t flags)
{
  if ((flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0u) return true;
  size_t n = dj_is_json_array(arr) ? ccord_json_array_len(arr) : 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *cj = ccord_json_array_at(arr, i);
    if (!dj_is_json_object(cj)) continue;
    int64_t t = dj_json_i64(cj, "type", 0);
    if ((t >= CCORD_COMP_SECTION && t <= CCORD_COMP_SEPARATOR) || t == CCORD_COMP_CONTAINER) return true;
  }
  return false;
}

// `partial` is a MESSAGE_UPDATE: Discord then sends only what changed, so a part counts as carried only when its key is
// there (an empty list under the key means "now empty"). Whole messages carry every part. Returns nullptr when a whole
// message has nothing beyond its text.
static const ccord_message_rich *map_rich(ccord_arena *a, const ccord_json *j, bool partial)
{
  if (!dj_is_json_object(j)) return nullptr;
  ccord_message_rich r = {0};
  r.ref_preview.author_name = r.ref_preview.content = "";
  r.forward.content = "";
  r.poll.question = "";
  r.thread.name = "";
  uint32_t present = 0;
  bool any = false;

  const ccord_json *tj = ccord_json_get(j, "type");
  const ccord_json *fj = ccord_json_get(j, "flags");
  if (tj || fj) present |= CCORD_RICH_HAS_TYPE;
  int64_t type = dj_json_i64(j, "type", 0);
  r.type = (type >= 0 && type <= UINT16_MAX) ? (uint16_t)type : 0u;
  r.flags = dj_clamp_u32(dj_json_i64(j, "flags", 0));
  any |= r.type != 0 || r.flags != 0;

  const ccord_json *mu = ccord_json_get(j, "mentions");
  const ccord_json *mr = ccord_json_get(j, "mention_roles");
  const ccord_json *me = ccord_json_get(j, "mention_everyone");
  if (mu || mr || me) present |= CCORD_RICH_HAS_MENTIONS;
  r.mention_users = rich_user_ids(a, mu, &r.mention_user_count);
  r.mention_roles = rich_role_ids(a, mr, &r.mention_role_count);
  r.mention_everyone = me && ccord_json_bool(me, false);
  any |= r.mention_user_count || r.mention_role_count || r.mention_everyone;

  const ccord_json *at = ccord_json_get(j, "attachments");
  if (at) present |= CCORD_RICH_HAS_ATTACHMENTS;
  r.attachments = rich_attachments(a, at, &r.attachment_count);
  any |= r.attachment_count > 0;

  const ccord_json *em = ccord_json_get(j, "embeds");
  if (em) present |= CCORD_RICH_HAS_EMBEDS;
  r.embeds = rich_embeds(a, em, &r.embed_count);
  any |= r.embed_count > 0;

  const ccord_json *st = ccord_json_get(j, "sticker_items");
  if (!st) st = ccord_json_get(j, "stickers");
  if (st) present |= CCORD_RICH_HAS_STICKERS;
  r.stickers = rich_stickers(a, st, &r.sticker_count);
  any |= r.sticker_count > 0;

  const ccord_json *ref = ccord_json_get(j, "message_reference");
  if (ref || ccord_json_get(j, "referenced_message")) present |= CCORD_RICH_HAS_REFERENCE;
  if (dj_is_json_object(ref)) {
    int64_t rt = dj_json_i64(ref, "type", 0);
    ccord_id mid = CCORD_ID_NONE;
    (void)dj_json_id_key(ref, "message_id", &mid);
    (void)dj_json_id_key(ref, "channel_id", &r.reference.channel_id);
    (void)dj_json_id_key(ref, "guild_id", &r.reference.guild_id);
    r.reference.message_id = mid;
    r.reference.kind = rt == 1 ? CCORD_REF_FORWARD : (mid != CCORD_ID_NONE ? CCORD_REF_REPLY : CCORD_REF_NONE);
  }
  rich_ref_preview(a, j, &r.ref_preview);
  if (r.reference.kind == CCORD_REF_NONE && (r.ref_preview.present || r.ref_preview.deleted)) r.reference.kind = CCORD_REF_REPLY;
  any |= r.reference.kind != CCORD_REF_NONE || r.ref_preview.present || r.ref_preview.deleted;

  if (ccord_json_get(j, "message_snapshots")) present |= CCORD_RICH_HAS_FORWARD;
  rich_forward(a, j, &r.forward);
  // A type-1 reference alone marks a forward: Discord can deliver it before (or without) the snapshot,
  // and the snapshot content then fills in on a later update.
  if (r.reference.kind == CCORD_REF_FORWARD) {
    present |= CCORD_RICH_HAS_FORWARD;
    r.forward.present = true;
  }
  any |= r.forward.present;

  const ccord_json *pj = ccord_json_get(j, "poll");
  if (pj) present |= CCORD_RICH_HAS_POLL;
  rich_poll(a, pj, &r.poll);
  any |= r.poll.present;

  const ccord_json *th = ccord_json_get(j, "thread");
  if (th) present |= CCORD_RICH_HAS_THREAD;
  if (dj_is_json_object(th) && dj_json_id_key(th, "id", &r.thread.id)) {
    r.thread.name = astr(a, th, "name", CCORD_RICH_SHORT_MAX_BYTES);
    r.thread.message_count = dj_clamp_u32(dj_json_i64(th, "message_count", 0));
    any = true;
  }

  (void)dj_json_id_key(j, "application_id", &r.application_id);
  if (r.application_id == CCORD_ID_NONE) {
    const ccord_json *au = ccord_json_get(j, "author");
    const ccord_json *botj = dj_is_json_object(au) ? ccord_json_get(au, "bot") : nullptr;
    if (botj && ccord_json_bool(botj, false)) {
      (void)dj_json_id_key(au, "id", &r.application_id);
    }
  }
  const ccord_json *cmps = ccord_json_get(j, "components");
  if (cmps) present |= CCORD_RICH_HAS_COMPONENTS;
  if (components_are_v2(cmps, r.flags)) {
    r.v2_components = rich_components_v2(a, cmps, &r.v2_component_count);
    any |= r.v2_component_count > 0;
  } else {
    r.components = rich_components(a, cmps, &r.component_row_count);
    any |= r.component_row_count > 0;
  }

  if (!partial) present = CCORD_RICH_ALL;
  const ccord_json *cj = ccord_json_get(j, "content");
  size_t clen = 0;
  r.text_absent = partial && !(cj && ccord_json_string(cj, &clen) != nullptr);
  if (partial ? present == 0 : !any) return nullptr;
  r.present = present;
  ccord_message_rich *out = ccord_arena_alloc(a, sizeof *out, _Alignof(ccord_message_rich));
  if (!out) return nullptr;
  *out = r;
  return out;
}

bool dj_map_message_ex(ccord_arena *a, const ccord_json *j, ccord_id fallback_channel,
                           bool allow_missing_author, ccord_message *out)
{
  if (!j) return false;
  ccord_id id = CCORD_ID_NONE;
  if (!dj_json_id_key(j, "id", &id)) return false;
  ccord_id channel = fallback_channel;
  if (!dj_json_id_key(j, "channel_id", &channel) && channel == CCORD_ID_NONE) return false;
  const ccord_json *author = ccord_json_get(j, "author");
  ccord_id author_id = CCORD_ID_NONE;
  if (!author || !dj_json_id_key(author, "id", &author_id)) {
    if (!dj_json_id_key(j, "author_id", &author_id) && !dj_json_id_key(j, "webhook_id", &author_id)) {
      if (!allow_missing_author) author_id = id;
    }
  }
  const char *content = dj_jstr(j, "content");
  if (!content) content = "";
  const char *ts = dj_jstr(j, "timestamp");
  size_t ts_len = 0;
  if (ts) ts_len = strlen(ts);
  int64_t stamp = dj_iso_ms(ts, ts_len);
  if (stamp < 0) stamp = 0;
  const char *edited = dj_jstr(j, "edited_timestamp");
  int64_t edited_ms = 0;
  if (edited && edited[0] != '\0') {
    edited_ms = dj_iso_ms(edited, strlen(edited));
    if (edited_ms < 0) edited_ms = 0;
  }
  uint32_t reactions = 0;
  bool self = false;
  message_reactions(j, &reactions, &self);
  ccord_id reply_to = CCORD_ID_NONE;
  const ccord_json *ref = ccord_json_get(j, "message_reference");
  if (ref && dj_json_i64(ref, "type", 0) != 1) (void)dj_json_id_key(ref, "message_id", &reply_to);   // type 1 is a forward
  const ccord_json *contentj = ccord_json_get(j, "content");
  const ccord_json *pinj = ccord_json_get(j, "pinned");
  const ccord_json *ttsj = ccord_json_get(j, "tts");
  memset(out, 0, sizeof *out);
  out->id = id;
  out->channel_id = channel;
  out->author_id = author_id;
  out->timestamp_ms = stamp;
  out->content = clamp_message_text(a, content);
  out->edited_timestamp_ms = edited_ms;
  out->reaction_count = reactions;
  out->self_reacted = self;
  out->reactions = nullptr;
  out->reaction_kind_count = 0;
  map_reaction_list(a, j, out);
  out->edited = edited_ms > 0;
  out->reply_to_id = reply_to;
  out->pinned = pinj && ccord_json_bool(pinj, false);
  out->tts = dj_json_bool_key(j, "tts", false);
  out->has_pinned = pinj != nullptr && ccord_json_type_of(pinj) == CCORD_JSON_BOOL;
  out->omit_content = allow_missing_author && (!contentj || ccord_json_type_of(contentj) != CCORD_JSON_STRING);
  out->omit_tts = allow_missing_author && (!ttsj || ccord_json_type_of(ttsj) != CCORD_JSON_BOOL);
  // M0-3: the server echoes the client nonce back on the sender's message; it confirms the pending local echo.
  const char *nonce = dj_jstr(j, "nonce");
  out->nonce[0] = '\0';
  if (nonce) {
    size_t nlen = strlen(nonce);
    if (nlen >= sizeof out->nonce) nlen = sizeof out->nonce - 1u;
    memcpy(out->nonce, nonce, nlen);
    out->nonce[nlen] = '\0';
  }
  out->send_state = CCORD_MSG_SEND_CONFIRMED;
  out->rich = map_rich(a, j, allow_missing_author);
  return true;
}

bool dj_map_message(ccord_arena *a, const ccord_json *j, ccord_id fallback_channel, ccord_message *out)
{
  return dj_map_message_ex(a, j, fallback_channel, false, out);
}


void dj_map_message_author(ccord_arena *a, const ccord_json *j, ccord_id author_id, ccord_user *out)
{
  const ccord_json *au = j ? ccord_json_get(j, "author") : nullptr;
  const ccord_json *mem = j ? ccord_json_get(j, "member") : nullptr;
  const char *name = mem ? dj_jstr(mem, "nick") : nullptr;
  if (!name || name[0] == '\0') name = au ? dj_jstr(au, "global_name") : nullptr;
  if (!name || name[0] == '\0') name = au ? dj_jstr(au, "username") : nullptr;
  if (!name || name[0] == '\0') name = j ? dj_jstr(j, "username") : nullptr;
  if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) name = "System";
  else {
    name = dj_arena_dup(a, name);
    if (name[0] == '\0') name = "System";
  }
  const char *handle = au ? dj_jstr(au, "username") : (j ? dj_jstr(j, "username") : nullptr);
  const ccord_json *botj = au ? ccord_json_get(au, "bot") : nullptr;
  *out = (ccord_user){
    .id = author_id,
    .name = name,
    .handle = dj_opt_text_dup(a, handle, CCORD_NAME_MAX_BYTES),
    .status_text = "",
    .avatar_url = au ? dj_build_cdn_url(a, "avatars", author_id, dj_jstr(au, "avatar")) : "",
    .avatar_decoration_url = au ? dj_extract_avatar_deco_url(a, au) : "",
    .bio = "",
    .pronouns = "",
    .presence = CCORD_PRESENCE_OFFLINE,
    .is_bot = (botj && ccord_json_bool(botj, false)) || (j && ccord_json_get(j, "webhook_id") != nullptr),
    .avatar_palette = (uint8_t)(author_id % 6u),
  };
  dj_extract_clan_info(au, out->clan_tag, &out->clan_guild_id, out->clan_badge);
  dj_extract_nameplate_info(au, out->nameplate_asset, out->nameplate_palette);
}

void dj_collect_message_mentions(ccord_arena *a, const ccord_json *j,
                                     ccord_user *users, size_t *ukept, size_t max_users)
{
  if (!j || !users || !ukept) return;
  const ccord_json *ments = ccord_json_get(j, "mentions");
  size_t nm = ments ? ccord_json_array_len(ments) : 0;
  for (size_t i = 0; i < nm && *ukept < max_users; i++) {
    const ccord_json *mu = ccord_json_array_at(ments, i);
    ccord_id uid = CCORD_ID_NONE;
    if (!mu || !dj_json_id_key(mu, "id", &uid)) continue;
    bool seen = false;
    for (size_t u = 0; u < *ukept; u++) {
      if (users[u].id == uid) { seen = true; break; }
    }
    if (seen) continue;
    const ccord_json *mem = ccord_json_get(mu, "member");
    const char *name = mem ? dj_jstr(mem, "nick") : nullptr;
    if (!name || name[0] == '\0') name = dj_jstr(mu, "global_name");
    if (!name || name[0] == '\0') name = dj_jstr(mu, "username");
    if (!dj_text_ok(name, CCORD_NAME_MAX_BYTES)) continue;
    const char *name_dup = dj_arena_dup(a, name);
    if (name_dup[0] == '\0') continue;
    const char *handle = dj_jstr(mu, "username");
    const ccord_json *botj = ccord_json_get(mu, "bot");
    ccord_user *dst = &users[(*ukept)++];
    *dst = (ccord_user){
      .id = uid,
      .name = name_dup,
      .handle = dj_opt_text_dup(a, handle, CCORD_NAME_MAX_BYTES),
      .status_text = "",
      .avatar_url = dj_build_cdn_url(a, "avatars", uid, dj_jstr(mu, "avatar")),
      .avatar_decoration_url = dj_extract_avatar_deco_url(a, mu),
      .bio = "",
      .pronouns = "",
      .presence = CCORD_PRESENCE_OFFLINE,
      .is_bot = botj && ccord_json_bool(botj, false),
      .avatar_palette = (uint8_t)(uid % 6u),
    };
    dj_extract_clan_info(mu, dst->clan_tag, &dst->clan_guild_id, dst->clan_badge);
    dj_extract_nameplate_info(mu, dst->nameplate_asset, dst->nameplate_palette);
  }
}


ccord_status ccord_live_json_message(const char *json, size_t len, ccord_net_event_type type,
                                     ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null message");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  ccord_message *m = ccord_arena_alloc(&out->arena, sizeof *m, _Alignof(ccord_message));
  if (!m) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  if (!dj_map_message(&out->arena, root, CCORD_ID_NONE, m)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad message object");
    goto done;
  }
  if (m->author_id != CCORD_ID_NONE) {
    ccord_user *u = ccord_arena_alloc(&out->arena, sizeof *u, _Alignof(ccord_user));
    if (u) {
      dj_map_message_author(&out->arena, root, m->author_id, u);
      out->users = u;
      out->user_count = 1;
      out->ev.users = u;
      out->ev.user_count = 1;
    }
  }
  out->messages = m;
  out->message_count = 1;
  out->ev.type = type;
  out->ev.channel_id = m->channel_id;
  out->ev.messages = m;
  out->ev.message_count = 1;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_history(const char *json, size_t len, ccord_id channel_id,
                                     ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null history");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n == 0) {
    out->ev.type = CCORD_NET_HISTORY;
    out->ev.channel_id = channel_id;
    goto done;
  }
  size_t ucap = n;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *mj = ccord_json_array_at(root, i);
    ucap += mj ? dj_jarray_len(mj, "mentions") : 0;
  }
  ccord_message *msgs = ccord_arena_alloc(&out->arena, n * sizeof *msgs, _Alignof(ccord_message));
  ccord_user *users = ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user));
  if (!msgs || !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: history too large");
    goto done;
  }
  size_t kept = 0, ukept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *mj = ccord_json_array_at(root, i);
    if (!dj_map_message(&out->arena, mj, channel_id, &msgs[kept])) continue;
    ccord_id aid = msgs[kept].author_id;
    kept++;
    if (aid != CCORD_ID_NONE) {
      bool seen = false;
      for (size_t u = 0; u < ukept; u++) {
        if (users[u].id == aid) { seen = true; break; }
      }
      if (!seen && ukept < ucap) {
        dj_map_message_author(&out->arena, mj, aid, &users[ukept++]);
      }
    }
    dj_collect_message_mentions(&out->arena, mj, users, &ukept, ucap);
  }
  out->messages = msgs;
  out->message_count = kept;
  out->users = users;
  out->user_count = ukept;
  out->ev.type = CCORD_NET_HISTORY;
  out->ev.channel_id = channel_id;
  out->ev.messages = msgs;
  out->ev.message_count = kept;
  out->ev.users = users;
  out->ev.user_count = ukept;
done:
  if (st != CCORD_OK) ccord_live_event_free(out);
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_search(const char *json, size_t len, ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "rest: null search");
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }

  const ccord_json *arr = root;
  bool nested = false;
  int64_t total_raw = -1;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    arr = ccord_json_get(root, "messages");
    nested = true;
    total_raw = dj_json_i64(root, "total_results", -1);
  }
  size_t n = arr ? ccord_json_array_len(arr) : 0;
  if (n == 0) {
    out->ev.type = CCORD_NET_SEARCH_RESULTS;
    out->ev.search_total_results = total_raw >= 0 ? dj_clamp_u32(total_raw) : 0u;
    goto done;
  }
  size_t ucap = n * 4u;
  ccord_message *msgs = ccord_arena_alloc(&out->arena, n * sizeof *msgs, _Alignof(ccord_message));
  ccord_user *users = ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user));
  if (!msgs || !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: search results too large");
    goto done;
  }
  size_t kept = 0, ukept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *item = ccord_json_array_at(arr, i);
    const ccord_json *mj = item;
    if (nested && item && ccord_json_type_of(item) == CCORD_JSON_ARRAY) {
      size_t group_len = ccord_json_array_len(item);
      mj = group_len > 0 ? ccord_json_array_at(item, 0) : nullptr;
      for (size_t k = 0; k < group_len; k++) {
        const ccord_json *cand = ccord_json_array_at(item, k);
        const ccord_json *hit = cand ? ccord_json_get(cand, "hit") : nullptr;
        if (hit && ccord_json_bool(hit, false)) {
          mj = cand;
          break;
        }
      }
    }
    if (mj && dj_map_message(&out->arena, mj, CCORD_ID_NONE, &msgs[kept])) {
      ccord_id aid = msgs[kept].author_id;
      kept++;
      if (aid != CCORD_ID_NONE) {
        bool seen = false;
        for (size_t u = 0; u < ukept; u++) {
          if (users[u].id == aid) { seen = true; break; }
        }
        if (!seen && ukept < ucap) {
          dj_map_message_author(&out->arena, mj, aid, &users[ukept++]);
        }
      }
      dj_collect_message_mentions(&out->arena, mj, users, &ukept, ucap);
    }
  }
  out->messages = msgs;
  out->message_count = kept;
  out->users = users;
  out->user_count = ukept;
  out->ev.type = CCORD_NET_SEARCH_RESULTS;
  out->ev.messages = msgs;
  out->ev.message_count = kept;
  out->ev.users = users;
  out->ev.user_count = ukept;
  out->ev.search_total_results = total_raw >= 0 ? dj_clamp_u32(total_raw) : (uint32_t)kept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_pins(const char *json, size_t len, ccord_id channel_id, bool append,
                                  ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || channel_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad pins args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root) {
    st = CCORD_ERR_PARSE;
    goto done;
  }

  const ccord_json *arr = nullptr;
  bool wrapped_items = false;
  bool has_more = false;
  if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
    arr = ccord_json_get(root, "items");
    if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
      st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad pins items");
      goto done;
    }
    wrapped_items = true;
    const ccord_json *hm = ccord_json_get(root, "has_more");
    has_more = hm && ccord_json_bool(hm, false);
  } else if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) {
    arr = root;
  } else {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad pins payload");
    goto done;
  }

  size_t n = ccord_json_array_len(arr);
  out->ev.type = CCORD_NET_PIN_LIST;
  out->ev.channel_id = channel_id;
  out->ev.pins_has_more = has_more;
  out->ev.pins_append = append;
  if (n == 0) {
    out->ev.pins_has_more = false;
    goto done;
  }

  size_t ucap = n * 4u;
  ccord_message *msgs = ccord_arena_alloc(&out->arena, n * sizeof *msgs, _Alignof(ccord_message));
  ccord_user *users = ccord_arena_alloc(&out->arena, ucap * sizeof *users, _Alignof(ccord_user));
  if (!msgs || !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: pins too large");
    goto done;
  }
  size_t kept = 0, ukept = 0;
  const char *last_pinned_at = nullptr;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *entry = ccord_json_array_at(arr, i);
    const ccord_json *mj = wrapped_items && entry ? ccord_json_get(entry, "message") : entry;
    if (!mj || !dj_map_message(&out->arena, mj, channel_id, &msgs[kept])) continue;
    msgs[kept].pinned = true;
    if (wrapped_items && entry) {
      const char *pa = dj_jstr(entry, "pinned_at");
      if (pa && pa[0] != '\0') last_pinned_at = pa;
    }
    ccord_id aid = msgs[kept].author_id;
    kept++;
    if (aid != CCORD_ID_NONE) {
      bool seen = false;
      for (size_t u = 0; u < ukept; u++) {
        if (users[u].id == aid) { seen = true; break; }
      }
      if (!seen && ukept < ucap) {
        dj_map_message_author(&out->arena, mj, aid, &users[ukept++]);
      }
    }
    dj_collect_message_mentions(&out->arena, mj, users, &ukept, ucap);
  }
  if (kept > 0) {
    if (last_pinned_at && last_pinned_at[0] != '\0') {
      out->ev.pins_next_cursor = ccord_arena_strndup(&out->arena, last_pinned_at, strlen(last_pinned_at));
    } else {
      char idbuf[24];
      int idlen = ccord_id_format(msgs[kept - 1].id, idbuf);
      if (idlen > 0) {
        out->ev.pins_next_cursor = ccord_arena_strndup(&out->arena, idbuf, (size_t)idlen);
      }
    }
  } else {
    out->ev.pins_has_more = false;
  }
  out->messages = msgs;
  out->message_count = kept;
  out->users = users;
  out->user_count = ukept;
  out->ev.messages = msgs;
  out->ev.message_count = kept;
  out->ev.users = users;
  out->ev.user_count = ukept;
done:
  ccord_arena_free(&scratch);
  return st;
}

ccord_status ccord_live_json_reaction_users(const char *json, size_t len, ccord_id channel_id,
                                            ccord_id message_id, const char *emoji,
                                            ccord_live_event *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (!json || !out || channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "rest: bad reaction users args");
  }
  ccord_live_event_init(out);
  ccord_arena scratch;
  ccord_arena_init(&scratch, dj_scratch_chunk_size(len));
  ccord_status st = CCORD_OK;
  const ccord_json *root = dj_parse_json(&scratch, json, len, err);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_ARRAY) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "rest: bad reaction users array");
    goto done;
  }
  size_t n = ccord_json_array_len(root);
  if (n > 100u) n = 100u;
  ccord_user *users = n > 0 ? ccord_arena_alloc(&out->arena, n * sizeof *users, _Alignof(ccord_user)) : nullptr;
  if (n > 0 && !users) {
    st = ccord_error_set(err, CCORD_ERR_NOMEM, "rest: out of memory");
    goto done;
  }
  size_t kept = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *uj = ccord_json_array_at(root, i);
    if (uj && dj_map_user_obj(&out->arena, uj, CCORD_PRESENCE_OFFLINE, &users[kept])) {
      kept++;
    }
  }
  const char *em = emoji ? emoji : "";
  out->users = users;
  out->user_count = kept;
  out->ev.type = CCORD_NET_REACTION_USERS;
  out->ev.channel_id = channel_id;
  out->ev.message_id = message_id;
  out->ev.reaction_emoji = ccord_arena_strndup(&out->arena, em, strlen(em));
  out->ev.users = users;
  out->ev.user_count = kept;
done:
  ccord_arena_free(&scratch);
  return st;
}


