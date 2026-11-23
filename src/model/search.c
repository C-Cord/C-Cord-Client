// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/search.h>
#include <ccord/model/rich.h>

#include <stdio.h>
#include <string.h>

static char ascii_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool ccord_search_contains_ci(const char *hay, const char *needle)
{
  if (!needle || !needle[0]) return true;
  if (!hay) return false;
  size_t n = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < n && p[i] && ascii_lower(p[i]) == ascii_lower(needle[i])) i++;
    if (i == n) return true;
  }
  return false;
}

static bool str_eq_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    if (ascii_lower(*a) != ascii_lower(*b)) return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool token_prefix_ci(const char *s, size_t slen, const char *prefix, const char **out_val, size_t *out_vlen)
{
  size_t plen = strlen(prefix);
  if (slen <= plen) return false;
  for (size_t i = 0; i < plen; i++) {
    if (ascii_lower(s[i]) != ascii_lower(prefix[i])) return false;
  }
  *out_val = s + plen;
  *out_vlen = slen - plen;
  return true;
}

static bool parse_uint_fixed(const char *s, size_t n, uint32_t *out)
{
  uint32_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10u + (uint32_t)(s[i] - '0');
  }
  *out = v;
  return true;
}

static bool is_leap_year(uint32_t y)
{
  return (y % 4u == 0u && y % 100u != 0u) || (y % 400u == 0u);
}

// Howard Hinnant's days-from-civil (1970-01-01 == 0), pure UTC arithmetic.
static int64_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
  y -= (m <= 2u) ? 1 : 0;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = (uint32_t)(y - era * 400);
  uint32_t doy = (153u * (m + (m > 2u ? (uint32_t)-3 : 9u)) + 2u) / 5u + d - 1u;
  uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

bool ccord_search_date_to_unix_ms(const char *s, size_t len, int64_t *out_day_start_ms)
{
  if (!s || len != 10u || !out_day_start_ms) return false;
  if (s[4] != '-' || s[7] != '-') return false;
  uint32_t y = 0, m = 0, d = 0;
  if (!parse_uint_fixed(s, 4u, &y) || !parse_uint_fixed(s + 5, 2u, &m) || !parse_uint_fixed(s + 8, 2u, &d)) {
    return false;
  }
  if (y < 2015u || y > 2100u || m < 1u || m > 12u || d < 1u) return false;
  static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint32_t max_d = mdays[m - 1u];
  if (m == 2u && is_leap_year(y)) max_d = 29u;
  if (d > max_d) return false;
  int64_t days = days_from_civil((int32_t)y, m, d);
  *out_day_start_ms = days * 86400000LL;
  return *out_day_start_ms >= CCORD_DISCORD_EPOCH_MS;
}

bool ccord_search_unix_ms_to_snowflake(int64_t unix_ms, ccord_id *out_id)
{
  if (!out_id || unix_ms < CCORD_DISCORD_EPOCH_MS) return false;
  uint64_t delta = (uint64_t)(unix_ms - CCORD_DISCORD_EPOCH_MS);
  if (delta > (UINT64_MAX >> 22)) return false;
  ccord_id sf = delta << 22;
  *out_id = sf != CCORD_ID_NONE ? sf : 1u;
  return true;
}

static void copy_clean_target(char *dst, size_t cap, const char *val, size_t vlen, char strip_prefix,
                              const char *angle_prefix, ccord_id *out_id)
{
  if (out_id) *out_id = CCORD_ID_NONE;
  if (angle_prefix) {
    size_t aplen = strlen(angle_prefix);
    if (vlen > aplen + 1u && memcmp(val, angle_prefix, aplen) == 0 && val[vlen - 1u] == '>') {
      const char *inner = val + aplen;
      size_t ilen = vlen - aplen - 1u;
      if (strip_prefix == '@' && ilen > 0 && inner[0] == '!') {
        inner++;
        ilen--;
      }
      ccord_id parsed = CCORD_ID_NONE;
      if (ccord_id_parse(inner, ilen, &parsed)) {
        if (out_id) *out_id = parsed;
        size_t cl = ilen < cap - 1u ? ilen : cap - 1u;
        memcpy(dst, inner, cl);
        dst[cl] = '\0';
        return;
      }
    }
  }
  if (vlen > 0 && val[0] == strip_prefix) {
    val++;
    vlen--;
  }
  size_t cl = vlen < cap - 1u ? vlen : cap - 1u;
  memcpy(dst, val, cl);
  dst[cl] = '\0';
  if (out_id) {
    ccord_id parsed = CCORD_ID_NONE;
    if (ccord_id_parse(dst, strlen(dst), &parsed)) *out_id = parsed;
  }
}

static bool parse_has_flag(const char *val, size_t vlen, uint32_t *out_flag)
{
  char buf[32];
  if (vlen == 0 || vlen >= sizeof buf) return false;
  memcpy(buf, val, vlen);
  buf[vlen] = '\0';
  if (str_eq_ci(buf, "link") || str_eq_ci(buf, "url")) {
    *out_flag = CCORD_SEARCH_HAS_LINK;
    return true;
  }
  if (str_eq_ci(buf, "embed")) {
    *out_flag = CCORD_SEARCH_HAS_EMBED;
    return true;
  }
  if (str_eq_ci(buf, "file") || str_eq_ci(buf, "attachment")) {
    *out_flag = CCORD_SEARCH_HAS_FILE;
    return true;
  }
  if (str_eq_ci(buf, "video")) {
    *out_flag = CCORD_SEARCH_HAS_VIDEO;
    return true;
  }
  if (str_eq_ci(buf, "image")) {
    *out_flag = CCORD_SEARCH_HAS_IMAGE;
    return true;
  }
  if (str_eq_ci(buf, "sound") || str_eq_ci(buf, "audio")) {
    *out_flag = CCORD_SEARCH_HAS_SOUND;
    return true;
  }
  if (str_eq_ci(buf, "sticker")) {
    *out_flag = CCORD_SEARCH_HAS_STICKER;
    return true;
  }
  if (str_eq_ci(buf, "poll")) {
    *out_flag = CCORD_SEARCH_HAS_POLL;
    return true;
  }
  if (str_eq_ci(buf, "forward") || str_eq_ci(buf, "snapshot")) {
    *out_flag = CCORD_SEARCH_HAS_FORWARD;
    return true;
  }
  return false;
}

static void append_content_token(ccord_search_query *out, const char *tok, size_t tlen)
{
  if (tlen == 0) return;
  size_t cur = strlen(out->content);
  size_t sep = cur > 0 ? 1u : 0u;
  if (cur + sep < sizeof out->content - 1u) {
    if (sep) out->content[cur++] = ' ';
    size_t avail = sizeof out->content - 1u - cur;
    size_t cl = tlen < avail ? tlen : avail;
    memcpy(out->content + cur, tok, cl);
    out->content[cur + cl] = '\0';
  }
  if (out->term_count < CCORD_SEARCH_MAX_TERMS) {
    size_t cl = tlen < CCORD_SEARCH_TERM_MAX_BYTES - 1u ? tlen : CCORD_SEARCH_TERM_MAX_BYTES - 1u;
    memcpy(out->terms[out->term_count], tok, cl);
    out->terms[out->term_count][cl] = '\0';
    out->term_count++;
  }
}

void ccord_search_parse(const char *raw, size_t len, ccord_search_query *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  out->pinned = -1;
  if (!raw || len == 0) return;

  size_t pos = 0;
  while (pos < len && raw[pos] != '\0') {
    while (pos < len && (raw[pos] == ' ' || raw[pos] == '\t')) pos++;
    if (pos >= len || raw[pos] == '\0') break;
    size_t start = pos;
    while (pos < len && raw[pos] != '\0' && raw[pos] != ' ' && raw[pos] != '\t') pos++;
    size_t tlen = pos - start;
    const char *tok = raw + start;

    const char *val = nullptr;
    size_t vlen = 0;
    if (token_prefix_ci(tok, tlen, "from:", &val, &vlen)) {
      copy_clean_target(out->from, sizeof out->from, val, vlen, '@', "<@", &out->author_id);
      out->has_operators = true;
    } else if (token_prefix_ci(tok, tlen, "mentions:", &val, &vlen)) {
      copy_clean_target(out->mentions, sizeof out->mentions, val, vlen, '@', "<@", &out->mentions_id);
      out->has_operators = true;
    } else if (token_prefix_ci(tok, tlen, "in:", &val, &vlen)) {
      copy_clean_target(out->in_channel, sizeof out->in_channel, val, vlen, '#', "<#", &out->channel_id);
      out->has_operators = true;
    } else if (token_prefix_ci(tok, tlen, "has:", &val, &vlen)) {
      uint32_t flag = 0;
      if (parse_has_flag(val, vlen, &flag)) {
        out->has_mask |= flag;
        out->has_operators = true;
      } else {
        append_content_token(out, tok, tlen);
      }
    } else if (token_prefix_ci(tok, tlen, "pinned:", &val, &vlen)) {
      char bbuf[16];
      if (vlen < sizeof bbuf) {
        memcpy(bbuf, val, vlen);
        bbuf[vlen] = '\0';
        if (str_eq_ci(bbuf, "true")) {
          out->pinned = 1;
          out->has_operators = true;
        } else if (str_eq_ci(bbuf, "false")) {
          out->pinned = 0;
          out->has_operators = true;
        } else {
          append_content_token(out, tok, tlen);
        }
      } else {
        append_content_token(out, tok, tlen);
      }
    } else if (token_prefix_ci(tok, tlen, "author_type:", &val, &vlen)) {
      char abuf[16];
      if (vlen < sizeof abuf) {
        memcpy(abuf, val, vlen);
        abuf[vlen] = '\0';
        if (str_eq_ci(abuf, "user")) {
          out->author_type = CCORD_SEARCH_AUTHOR_USER;
          out->has_operators = true;
        } else if (str_eq_ci(abuf, "bot")) {
          out->author_type = CCORD_SEARCH_AUTHOR_BOT;
          out->has_operators = true;
        } else if (str_eq_ci(abuf, "webhook")) {
          out->author_type = CCORD_SEARCH_AUTHOR_WEBHOOK;
          out->has_operators = true;
        } else {
          append_content_token(out, tok, tlen);
        }
      } else {
        append_content_token(out, tok, tlen);
      }
    } else if (token_prefix_ci(tok, tlen, "sort:", &val, &vlen)) {
      char sbuf[16];
      if (vlen < sizeof sbuf) {
        memcpy(sbuf, val, vlen);
        sbuf[vlen] = '\0';
        if (str_eq_ci(sbuf, "new") || str_eq_ci(sbuf, "newest")) {
          out->sort = CCORD_SEARCH_SORT_NEW;
          out->has_operators = true;
        } else if (str_eq_ci(sbuf, "old") || str_eq_ci(sbuf, "oldest")) {
          out->sort = CCORD_SEARCH_SORT_OLD;
          out->has_operators = true;
        } else if (str_eq_ci(sbuf, "relevant") || str_eq_ci(sbuf, "relevance")) {
          out->sort = CCORD_SEARCH_SORT_RELEVANT;
          out->has_operators = true;
        } else {
          append_content_token(out, tok, tlen);
        }
      } else {
        append_content_token(out, tok, tlen);
      }
    } else if (token_prefix_ci(tok, tlen, "before:", &val, &vlen)) {
      int64_t day_ms = 0;
      ccord_id sf = CCORD_ID_NONE;
      if (ccord_search_date_to_unix_ms(val, vlen, &day_ms) && ccord_search_unix_ms_to_snowflake(day_ms, &sf)) {
        out->max_id = sf;
        out->has_operators = true;
      } else if (ccord_id_parse(val, vlen, &sf)) {
        out->max_id = sf;
        out->has_operators = true;
      } else {
        append_content_token(out, tok, tlen);
      }
    } else if (token_prefix_ci(tok, tlen, "after:", &val, &vlen)) {
      int64_t day_ms = 0;
      ccord_id sf = CCORD_ID_NONE;
      if (ccord_search_date_to_unix_ms(val, vlen, &day_ms) &&
          ccord_search_unix_ms_to_snowflake(day_ms + 86400000LL, &sf)) {
        out->min_id = sf;
        out->min_exclusive = false;
        out->has_operators = true;
      } else if (ccord_id_parse(val, vlen, &sf)) {
        out->min_id = sf;
        out->min_exclusive = true;
        out->has_operators = true;
      } else {
        append_content_token(out, tok, tlen);
      }
    } else if (token_prefix_ci(tok, tlen, "during:", &val, &vlen)) {
      int64_t day_ms = 0;
      ccord_id min_sf = CCORD_ID_NONE, max_sf = CCORD_ID_NONE;
      if (ccord_search_date_to_unix_ms(val, vlen, &day_ms) &&
          ccord_search_unix_ms_to_snowflake(day_ms, &min_sf) &&
          ccord_search_unix_ms_to_snowflake(day_ms + 86400000LL, &max_sf)) {
        out->min_id = min_sf;
        out->max_id = max_sf;
        out->min_exclusive = false;
        out->has_operators = true;
      } else {
        append_content_token(out, tok, tlen);
      }
    } else {
      append_content_token(out, tok, tlen);
    }
  }

  // When no operators were used, preserve exact phrase substring matching (including internal spaces).
  if (!out->has_operators) {
    size_t cl = len < sizeof out->content - 1u ? len : sizeof out->content - 1u;
    memcpy(out->content, raw, cl);
    out->content[cl] = '\0';
    if (cl > 0) {
      size_t tl = cl < CCORD_SEARCH_TERM_MAX_BYTES - 1u ? cl : CCORD_SEARCH_TERM_MAX_BYTES - 1u;
      memcpy(out->terms[0], raw, tl);
      out->terms[0][tl] = '\0';
      out->term_count = 1u;
    } else {
      out->term_count = 0u;
    }
  }
}

static bool content_mentions_id(const char *content, ccord_id uid)
{
  if (!content || uid == CCORD_ID_NONE) return false;
  char id_str[24];
  ccord_id_format(uid, id_str);
  char pat1[32], pat2[32];
  snprintf(pat1, sizeof pat1, "<@%s>", id_str);
  snprintf(pat2, sizeof pat2, "<@!%s>", id_str);
  return strstr(content, pat1) != nullptr || strstr(content, pat2) != nullptr;
}

static bool message_mentions_id(const ccord_message *m, ccord_id uid)
{
  if (!m || uid == CCORD_ID_NONE) return false;
  if (m->rich) {
    for (size_t i = 0; i < m->rich->mention_user_count; i++) {
      if (m->rich->mention_users[i] == uid) return true;
    }
  }
  return content_mentions_id(m->content, uid);
}

bool ccord_search_message_matches(const ccord_search_query *q, const ccord_message *m,
                                  const ccord_search_msg_ctx *ctx)
{
  if (!q || !m || !m->content) return false;

  if (q->channel_id != CCORD_ID_NONE) {
    if (m->channel_id != q->channel_id) return false;
  } else if (q->in_channel[0] != '\0') {
    if (!ctx || !ctx->channel_name || !ccord_search_contains_ci(ctx->channel_name, q->in_channel)) return false;
  }

  if (q->min_id != CCORD_ID_NONE) {
    if (q->min_exclusive ? (m->id <= q->min_id) : (m->id < q->min_id)) return false;
  }
  if (q->max_id != CCORD_ID_NONE && m->id >= q->max_id) return false;

  if (q->pinned == 1 && !m->pinned) return false;
  if (q->pinned == 0 && m->pinned) return false;

  const ccord_message_rich *rich = m->rich;
  if ((q->has_mask & CCORD_SEARCH_HAS_LINK) != 0) {
    bool in_text = ccord_search_contains_ci(m->content, "http://") || ccord_search_contains_ci(m->content, "https://");
    bool in_embed = false;
    for (size_t e = 0; rich && e < rich->embed_count; e++) {
      if (rich->embeds[e].url && rich->embeds[e].url[0]) in_embed = true;
    }
    if (!in_text && !in_embed) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_EMBED) != 0) {
    if (!rich || rich->embed_count == 0) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_FILE) != 0) {
    if (!rich || rich->attachment_count == 0) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_IMAGE) != 0) {
    bool any = false;
    for (size_t a = 0; rich && a < rich->attachment_count; a++) any |= ccord_attachment_is_image(&rich->attachments[a]);
    for (size_t e = 0; rich && e < rich->embed_count; e++) {
      any |= (rich->embeds[e].image.url && rich->embeds[e].image.url[0]) ||
             (rich->embeds[e].thumbnail.url && rich->embeds[e].thumbnail.url[0]);
    }
    if (!any) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_VIDEO) != 0) {
    bool any = false;
    for (size_t a = 0; rich && a < rich->attachment_count; a++) any |= ccord_attachment_is_video(&rich->attachments[a]);
    for (size_t e = 0; rich && e < rich->embed_count; e++) {
      any |= rich->embeds[e].video.url && rich->embeds[e].video.url[0];
    }
    if (!any) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_SOUND) != 0) {
    bool any = false;
    for (size_t a = 0; rich && a < rich->attachment_count; a++) any |= ccord_attachment_is_audio(&rich->attachments[a]);
    if (!any) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_STICKER) != 0) {
    if (!rich || rich->sticker_count == 0) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_POLL) != 0) {
    if (!rich || !rich->poll.present) return false;
  }
  if ((q->has_mask & CCORD_SEARCH_HAS_FORWARD) != 0) {
    if (!rich || !rich->forward.present) return false;
  }
  if (q->author_type != CCORD_SEARCH_AUTHOR_ANY && ctx) {
    if (q->author_type == CCORD_SEARCH_AUTHOR_BOT && !ctx->author_is_bot) return false;
    if (q->author_type == CCORD_SEARCH_AUTHOR_WEBHOOK && !ctx->author_is_webhook) return false;
    if (q->author_type == CCORD_SEARCH_AUTHOR_USER && (ctx->author_is_bot || ctx->author_is_webhook)) return false;
  }

  if (q->author_id != CCORD_ID_NONE) {
    if (m->author_id != q->author_id) return false;
  } else if (q->from[0] != '\0') {
    bool matched = false;
    if (str_eq_ci(q->from, "me") && ctx && ctx->self_id != CCORD_ID_NONE) {
      matched = (m->author_id == ctx->self_id);
    } else if (ctx) {
      if (ctx->author_name && ccord_search_contains_ci(ctx->author_name, q->from)) matched = true;
      if (!matched && ctx->author_handle && ccord_search_contains_ci(ctx->author_handle, q->from)) matched = true;
      if (!matched && ctx->author_nick && ctx->author_nick[0] && ccord_search_contains_ci(ctx->author_nick, q->from)) {
        matched = true;
      }
      if (!matched && ctx->user_matches && ctx->user_matches(ctx->ud, m->author_id, q->from)) matched = true;
    }
    if (!matched) return false;
  }

  if (q->mentions_id != CCORD_ID_NONE) {
    if (!message_mentions_id(m, q->mentions_id)) return false;
  } else if (q->mentions[0] != '\0') {
    bool matched = false;
    if (str_eq_ci(q->mentions, "me") && ctx && ctx->self_id != CCORD_ID_NONE) {
      matched = message_mentions_id(m, ctx->self_id);
    } else {
      if (rich && ctx && ctx->user_matches) {
        for (size_t i = 0; i < rich->mention_user_count; i++) {
          if (ctx->user_matches(ctx->ud, rich->mention_users[i], q->mentions)) {
            matched = true;
            break;
          }
        }
      }
      if (!matched) {
        char at_pat[CCORD_NAME_MAX_BYTES + 4];
        snprintf(at_pat, sizeof at_pat, "@%s", q->mentions);
        if (ccord_search_contains_ci(m->content, at_pat)) matched = true;
      }
    }
    if (!matched) return false;
  }

  for (size_t i = 0; i < q->term_count; i++) {
    if (!ccord_search_contains_ci(m->content, q->terms[i])) return false;
  }
  return true;
}
