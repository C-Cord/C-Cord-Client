// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

ccord_id dc_spoiler_att_key(ccord_id msg_id, const ccord_attachment *a, size_t idx)
{
  if (a && a->id != CCORD_ID_NONE) return a->id ^ (1ULL << 63);
  return (msg_id ^ 0x50011E0000000000ULL) + (ccord_id)(idx + 1u);
}

static bool slice_eq_ci(const char *s, const char *slice, size_t slen)
{
  if (!s || !slice || slen == 0) return false;
  for (size_t i = 0; i < slen; i++) {
    char a = s[i], b = slice[i];
    if (a == '\0') return false;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return s[slen] == '\0';
}

static bool user_matches_mention(const ccord_user *u, const char *name, size_t nlen)
{
  if (!u || !name || nlen == 0) return false;
  if (u->name && slice_eq_ci(u->name, name, nlen)) return true;
  if (u->handle && slice_eq_ci(u->handle, name, nlen)) return true;
  return false;
}

static ccord_id resolve_mention_user_id(const ccord_ui *ui, const ui_md_token *tok)
{
  if (!tok) return CCORD_ID_NONE;
  if (tok->style == UI_MD_USER_MENTION) return tok->id;
  if (tok->style != UI_MD_MENTION || !tok->text || tok->len <= 1 || tok->text[0] != '@') {
    return CCORD_ID_NONE;
  }
  const char *name = tok->text + 1;
  size_t nlen = tok->len - 1;
  while (nlen > 0) {
    char c = name[nlen - 1];
    if (c == ',' || c == '.' || c == ':' || c == ';' || c == '!' || c == '?') nlen--;
    else break;
  }
  if (nlen == 0) return CCORD_ID_NONE;
  // Never scan the store for broadcast mentions.
  if (slice_eq_ci("everyone", name, nlen) || slice_eq_ci("here", name, nlen)) {
    return CCORD_ID_NONE;
  }
  ccord_id parsed = CCORD_ID_NONE;
  if (ccord_id_parse(name, nlen, &parsed)) return parsed;
  if (!ui || !ui->d.store) return CCORD_ID_NONE;
  const ccord_store *st = ui->d.store;

  ccord_id sel_g = ccord_store_selected_guild(st);
  ccord_id sel_ch = ccord_store_selected_channel(st);
  struct ui_chat_caches *cc = ui->chat_caches;
  uint64_t nh = 0;
  char lower[48];
  bool cacheable = cc != nullptr && nlen < sizeof lower;
  if (cacheable) {
    uint64_t rev = ccord_store_revision(st);
    if (cc->mention_rev != rev || cc->mention_guild != sel_g || cc->mention_chan != sel_ch) {
      cc->mention_rev = rev;
      cc->mention_guild = sel_g;
      cc->mention_chan = sel_ch;
      memset(cc->mentions, 0, sizeof cc->mentions);
    }
    nh = 1469598103934665603ULL;
    for (size_t i = 0; i < nlen; i++) {
      char ch = name[i];
      if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
      lower[i] = ch;
      nh = (nh ^ (uint8_t)ch) * 1099511628211ULL;
    }
    if (nh == 0) nh = 1u;
    const chat_mention_entry *ce = &cc->mentions[(size_t)(nh & (CHAT_MENTION_CACHE_SLOTS - 1u))];
    if (ce->hash == nh && ce->nlen == (uint8_t)nlen && memcmp(ce->name, lower, nlen) == 0) {
      return ce->user_id;
    }
  }

  ccord_id found = CCORD_ID_NONE;
  ccord_id self_id = ccord_store_self_id(st);
  if (self_id != CCORD_ID_NONE) {
    const ccord_user *su = ccord_store_user(st, self_id);
    if (user_matches_mention(su, name, nlen) || slice_eq_ci("you", name, nlen)) found = self_id;
  }

  if (found == CCORD_ID_NONE && sel_g != CCORD_ID_NONE) {
    size_t mc = ccord_store_member_count(st, sel_g);
    for (size_t i = 0; i < mc; i++) {
      const ccord_member *m = ccord_store_member_at(st, sel_g, i);
      const ccord_user *u = m ? ccord_store_user(st, m->user_id) : nullptr;
      if (user_matches_mention(u, name, nlen)) {
        found = m->user_id;
        break;
      }
    }
  }

  if (found == CCORD_ID_NONE && sel_ch != CCORD_ID_NONE) {
    size_t msg_c = ccord_store_message_count(st, sel_ch);
    for (size_t i = 0; i < msg_c; i++) {
      const ccord_message *msg = ccord_store_message_at(st, sel_ch, i);
      const ccord_user *u = (msg && msg->author_id != CCORD_ID_NONE) ? ccord_store_user(st, msg->author_id) : nullptr;
      if (user_matches_mention(u, name, nlen)) {
        found = msg->author_id;
        break;
      }
    }
  }

  if (found == CCORD_ID_NONE) {
    size_t rc = ccord_store_relationship_count(st);
    for (size_t i = 0; i < rc; i++) {
      const ccord_relationship *rel = ccord_store_relationship_at(st, i);
      const ccord_user *u = rel ? ccord_store_user(st, rel->user_id) : nullptr;
      if (user_matches_mention(u, name, nlen)) {
        found = rel->user_id;
        break;
      }
    }
  }

  if (found == CCORD_ID_NONE) {
    size_t dmc = ccord_store_channel_count(st, CCORD_DM_GUILD_ID);
    for (size_t i = 0; i < dmc; i++) {
      const ccord_channel *c = ccord_store_channel_at(st, CCORD_DM_GUILD_ID, i);
      const ccord_user *u = (c && c->user_id != CCORD_ID_NONE) ? ccord_store_user(st, c->user_id) : nullptr;
      if (user_matches_mention(u, name, nlen)) {
        found = c->user_id;
        break;
      }
    }
  }

  if (found == CCORD_ID_NONE) {
    size_t gc = ccord_store_guild_count(st);
    for (size_t g = 0; g < gc && found == CCORD_ID_NONE; g++) {
      const ccord_guild *gd = ccord_store_guild_at(st, g);
      if (!gd || gd->id == sel_g) continue;
      size_t mc = ccord_store_member_count(st, gd->id);
      for (size_t i = 0; i < mc; i++) {
        const ccord_member *m = ccord_store_member_at(st, gd->id, i);
        const ccord_user *u = m ? ccord_store_user(st, m->user_id) : nullptr;
        if (user_matches_mention(u, name, nlen)) {
          found = m->user_id;
          break;
        }
      }
    }
  }

  if (cacheable) {
    chat_mention_entry *ce = &cc->mentions[(size_t)(nh & (CHAT_MENTION_CACHE_SLOTS - 1u))];
    ce->hash = nh;
    ce->nlen = (uint8_t)nlen;
    memcpy(ce->name, lower, nlen);
    ce->user_id = found;
  }
  return found;
}

static size_t format_discord_timestamp(const ccord_ui *ui, int64_t sec, char style, char out[static 96])
{
  int64_t ts_ms = 0;
  if (ckd_mul(&ts_ms, sec, 1000LL)) {
    ts_ms = sec < 0 ? INT64_MIN : INT64_MAX;
  }
  int64_t now_ms = (ui && ui->now_unix_ms > 0) ? ui->now_unix_ms : (ts_ms + 60000LL);
  bool use_utc = ui ? ui->use_utc : true;
  if (style == 'R') {
    int64_t diff = (now_ms / 1000LL) - sec;
    int64_t abs_d = diff < 0 ? -diff : diff;
    int n = 0;
    if (abs_d < 45) {
      n = snprintf(out, 96, "%s", diff >= 0 ? "just now" : "in a few seconds");
    } else if (abs_d < 3600) {
      long m = (long)((abs_d + 30) / 60);
      if (m < 1) m = 1;
      n = diff >= 0 ? snprintf(out, 96, "%ld minute%s ago", m, m == 1 ? "" : "s")
                    : snprintf(out, 96, "in %ld minute%s", m, m == 1 ? "" : "s");
    } else if (abs_d < 86400) {
      long h = (long)((abs_d + 1800) / 3600);
      if (h < 1) h = 1;
      n = diff >= 0 ? snprintf(out, 96, "%ld hour%s ago", h, h == 1 ? "" : "s")
                    : snprintf(out, 96, "in %ld hour%s", h, h == 1 ? "" : "s");
    } else {
      long d = (long)((abs_d + 43200) / 86400);
      if (d < 1) d = 1;
      n = diff >= 0 ? snprintf(out, 96, "%ld day%s ago", d, d == 1 ? "" : "s")
                    : snprintf(out, 96, "in %ld day%s", d, d == 1 ? "" : "s");
    }
    return (n > 0 && (size_t)n < 96u) ? (size_t)n : strlen(out);
  }
  if (style == 't' || style == 'T') {
    return ccord_timefmt_clock(out, 96, ts_ms, use_utc);
  }
  if (style == 'd') {
    return ccord_timefmt_message(out, 96, ts_ms, ts_ms + 5LL * 86400000LL, use_utc);
  }
  if (style == 'D') {
    return ccord_timefmt_date(out, 96, ts_ms, use_utc);
  }
  if (style == 'F') {
    return ccord_timefmt_full(out, 96, ts_ms, use_utc);
  }
  char date_part[48];
  char time_part[32];
  size_t dl = ccord_timefmt_date(date_part, sizeof date_part, ts_ms, use_utc);
  size_t tl = ccord_timefmt_clock(time_part, sizeof time_part, ts_ms, use_utc);
  if (dl + 1u + tl < 96u) {
    memcpy(out, date_part, dl);
    out[dl] = ' ';
    memcpy(out + dl + 1u, time_part, tl);
    out[dl + 1u + tl] = '\0';
    return dl + 1u + tl;
  }
  memcpy(out, date_part, dl);
  out[dl] = '\0';
  return dl;
}

// Cuts at a UTF-8 boundary so a truncated summary never splits a code point.
size_t dc_utf8_boundary(const char *s, size_t n, size_t cap)
{
  if (n < cap) return n;
  size_t k = cap ? cap - 1 : 0;
  while (k > 0 && ((unsigned char)s[k] & 0xC0u) == 0x80u) k--;
  return k;
}

static size_t resolve_token_label(const ccord_ui *ui, const ui_md_token *tok, char buf[static 160],
                                  const char **out_str, bool *out_pill)
{
  *out_pill = false;
  if (tok->style == UI_MD_USER_MENTION) {
    *out_pill = true;
    const ccord_user *u = (ui && ui->d.store) ? ccord_store_user(ui->d.store, tok->id) : nullptr;
    ccord_id gid = (ui && ui->d.store) ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE;
    const ccord_member *gm = (gid != CCORD_ID_NONE && ui && ui->d.store) ? ccord_store_find_member(ui->d.store, gid, tok->id) : nullptr;
    const char *name = (gm && gm->nick[0]) ? gm->nick
                       : ((u && u->display_name && u->display_name[0]) ? u->display_name
                       : ((u && u->name && u->name[0]) ? u->name : nullptr));
    buf[0] = '@';
    if (name) {
      size_t nl = dc_utf8_boundary(name, strlen(name), 150u);
      memcpy(buf + 1, name, nl);
      buf[1 + nl] = '\0';
      *out_str = buf;
      return 1u + nl;
    }
    size_t il = tok->len < 150u ? tok->len : 150u;
    memcpy(buf + 1, tok->text, il);
    buf[1 + il] = '\0';
    *out_str = buf;
    return 1u + il;
  }
  if (tok->style == UI_MD_CHANNEL_MENTION) {
    *out_pill = true;
    const ccord_channel *ch = (ui && ui->d.store) ? ccord_store_channel(ui->d.store, tok->id) : nullptr;
    const char *name = (ch && ch->name && ch->name[0]) ? ch->name : nullptr;
    buf[0] = '#';
    if (name) {
      size_t nl = dc_utf8_boundary(name, strlen(name), 150u);
      memcpy(buf + 1, name, nl);
      buf[1 + nl] = '\0';
      *out_str = buf;
      return 1u + nl;
    }
    size_t il = tok->len < 150u ? tok->len : 150u;
    memcpy(buf + 1, tok->text, il);
    buf[1 + il] = '\0';
    *out_str = buf;
    return 1u + il;
  }
  if (tok->style == UI_MD_ROLE_MENTION) {
    *out_pill = true;
    buf[0] = '@';
    ccord_id gid = (ui && ui->d.store) ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE;
    const char *rname = nullptr;
    if (gid != CCORD_ID_NONE && ui->d.store) {
      size_t nroles = 0;
      const ccord_role *roles = ccord_store_roles(ui->d.store, gid, &nroles);
      for (size_t ri = 0; ri < nroles; ri++) {
        if (roles[ri].id == tok->id) {
          if (roles[ri].name && roles[ri].name[0]) rname = roles[ri].name;
          break;
        }
      }
    }
    const char *name = rname ? rname : nullptr;
    if (name) {
      size_t nl = dc_utf8_boundary(name, strlen(name), 150u);
      memcpy(buf + 1, name, nl);
      buf[1 + nl] = '\0';
      *out_str = buf;
      return 1u + nl;
    }
    size_t il = tok->len < 150u ? tok->len : 150u;
    memcpy(buf + 1, tok->text, il);
    buf[1 + il] = '\0';
    *out_str = buf;
    return 1u + il;
  }
  if (tok->style == UI_MD_TIMESTAMP) {
    *out_pill = true;
    size_t l = format_discord_timestamp(ui, tok->timestamp_sec, tok->timestamp_style, buf);
    *out_str = buf;
    return l;
  }
  if (tok->style == UI_MD_CUSTOM_EMOJI) {
    buf[0] = ':';
    size_t nl = tok->len < 150u ? tok->len : 150u;
    memcpy(buf + 1, tok->text, nl);
    buf[1 + nl] = ':';
    buf[2 + nl] = '\0';
    *out_str = buf;
    return 2u + nl;
  }
  if (tok->style == UI_MD_MENTION) {
    *out_pill = true;
  }
  *out_str = tok->text;
  return tok->len;
}

bool dc_is_spoiler_revealed(const ccord_ui *ui, ccord_id msg_id)
{
  if (!ui || msg_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nrevealed_spoilers; i++) {
    if (ui->revealed_spoilers[i] == msg_id) return true;
  }
  return false;
}

void dc_chat_sel_append(ccord_ui *ui, const char *s, size_t n, bool new_line)
{
  if (!ui || !s || n == 0) return;
  if (ui->chat_sel_len > 0 && ui->chat_sel_len + 1 < sizeof ui->chat_sel_buf) {
    char prev = ui->chat_sel_buf[ui->chat_sel_len - 1];
    if (new_line && prev != '\n') {
      ui->chat_sel_buf[ui->chat_sel_len++] = '\n';
    } else if (!new_line && prev != ' ' && prev != '\n') {
      ui->chat_sel_buf[ui->chat_sel_len++] = ' ';
    }
  }
  size_t avail = (sizeof ui->chat_sel_buf - 1) - ui->chat_sel_len;
  if (n > avail) n = avail;
  if (n > 0) {
    memcpy(ui->chat_sel_buf + ui->chat_sel_len, s, n);
    ui->chat_sel_len += n;
    ui->chat_sel_buf[ui->chat_sel_len] = '\0';
  }
}

// The part of the drag selection that falls on the line box [ty, ty + lh), normalised so it always runs top to bottom.
// *x0 / *x1 limit it horizontally (infinite when the selection runs through the whole line: only its first line has a
// left limit and only its last line a right one) and *goes_on says the selection continues below this line, which means
// the line break itself is selected too. False when the line is outside the selection.
bool dc_chat_sel_line(const ccord_ui *ui, float ty, float lh, float *x0, float *x1, bool *goes_on)
{
  if (!ui || !ui->chat_sel_active || ui->chat_sel_channel != ccord_store_selected_channel(ui->d.store)) return false;
  float ax = ui->chat_sel_ax, ay = ui->chat_sel_ay;
  float cx = ui->chat_sel_cx, cy = ui->chat_sel_cy;
  if (fabsf(ax - cx) < 2.0f && fabsf(ay - cy) < 2.0f) return false;
  float sy0 = ay, sx0 = ax, sy1 = cy, sx1 = cx;
  if (ay > cy + 4.0f || (fabsf(ay - cy) <= 4.0f && ax > cx)) {
    sy0 = cy; sx0 = cx;
    sy1 = ay; sx1 = ax;
  }
  if (ty + lh <= sy0 || ty >= sy1) return false;
  bool first = (sy0 >= ty && sy0 < ty + lh);
  bool last = (sy1 >= ty && sy1 < ty + lh);
  *x0 = first ? sx0 : -INFINITY;
  *x1 = last ? sx1 : INFINITY;
  *goes_on = !last;
  return true;
}

void dc_chat_sel_fill(ccord_ui *ui, ccord_renderer *r, ccord_rect rc)
{
  ccord_draw_rect(r, rc, COL(selection));
  if (ui->dbg_nsel < sizeof ui->dbg_sel / sizeof ui->dbg_sel[0]) ui->dbg_sel[ui->dbg_nsel++] = rc;
}

// A run of text inside the selection: only the selected characters, snapped to character boundaries.
// `pad` extends the highlight over a pill's padding when the whole run is selected, so the pill reads as one block.
void dc_chat_sel_process_run(ccord_ui *ui, ccord_renderer *r, const char *str, size_t len,
                                 const ccord_text_style *st, float tx, float ty, float tw, float lh, float pad)
{
  if (len == 0) return;
  float sel_x0, sel_x1;
  bool goes_on;
  if (!dc_chat_sel_line(ui, ty, lh, &sel_x0, &sel_x1, &goes_on)) return;
  if (sel_x1 <= tx || sel_x0 >= tx + tw) return;
  size_t c0 = sel_x0 <= tx ? 0 : ccord_text_hit_test(r, str, len, st, 0.0f, sel_x0 - tx, lh * 0.5f);
  size_t c1 = sel_x1 >= tx + tw ? len : ccord_text_hit_test(r, str, len, st, 0.0f, sel_x1 - tx, lh * 0.5f);
  if (c1 <= c0) return;
  float x0 = c0 > 0 ? ccord_text_measure(r, str, c0, st, 0.0f).x : 0.0f;
  float x1 = ccord_text_measure(r, str, c1, st, 0.0f).x;
  float left = tx + x0, right = tx + x1;
  if (c0 == 0) left -= pad;
  if (c1 == len) right += pad;
  dc_chat_sel_fill(ui, r, (ccord_rect){left, ty, maxf(right - left, 2.0f), lh});
  dc_chat_sel_append(ui, str + c0, c1 - c0, false);
}

// The blank between two words is part of the selection like any other character, so a selected sentence is one
// unbroken band and not a string of separate words. `count` spaces of `space_w` each start at `tx`.
void dc_chat_sel_process_gap(ccord_ui *ui, ccord_renderer *r, float tx, float ty, size_t count, float space_w,
                                 float lh)
{
  float sel_x0, sel_x1;
  bool goes_on;
  if (count == 0 || space_w <= 0.0f || !dc_chat_sel_line(ui, ty, lh, &sel_x0, &sel_x1, &goes_on)) return;
  float tw = space_w * (float)count;
  if (sel_x1 <= tx || sel_x0 >= tx + tw) return;
  float a = sel_x0 <= tx ? 0.0f : roundf((sel_x0 - tx) / space_w);
  float b = sel_x1 >= tx + tw ? (float)count : roundf((sel_x1 - tx) / space_w);
  if (b <= a) return;
  dc_chat_sel_fill(ui, r, (ccord_rect){tx + a * space_w, ty, (b - a) * space_w, lh});
}

// The end of a line the selection runs past: Discord (a browser) shows the line break as a short highlighted stub after
// the last character, and so do we; it also keeps the band continuous from one line to the next. `hard` marks the end
// of a logical line (a newline in the text, a message), where a copy gets a line break, not a space.
void dc_chat_sel_line_end(ccord_ui *ui, ccord_renderer *r, float end_x, float ty, float stub_w, float lh, bool hard)
{
  float sel_x0, sel_x1;
  bool goes_on;
  if (!dc_chat_sel_line(ui, ty, lh, &sel_x0, &sel_x1, &goes_on) || !goes_on || sel_x0 > end_x + stub_w) return;
  dc_chat_sel_fill(ui, r, (ccord_rect){end_x, ty, stub_w, lh});
  if (hard && ui->chat_sel_len > 0 && ui->chat_sel_len + 1 < sizeof ui->chat_sel_buf &&
      ui->chat_sel_buf[ui->chat_sel_len - 1] != '\n') {
    ui->chat_sel_buf[ui->chat_sel_len++] = '\n';
    ui->chat_sel_buf[ui->chat_sel_len] = '\0';
  }
}

// Finds the first Unicode emoji sequence in s[0..len). Fast-path rejects ASCII non-keycaps in O(1) per byte.
static bool md_inline_find_emoji(const char *s, size_t len, size_t *out_off, size_t *out_len)
{
  size_t i = 0;
  while (i < len) {
    size_t ml = ccord_emoji_match_prefix(s + i, len - i);
    if (ml > 0) {
      *out_off = i;
      *out_len = ml;
      return true;
    }
    i = ccord_utf8_next(s, len, i);
  }
  return false;
}

static float md_inline_measure_token(const ccord_ui *ui, ccord_renderer *r, const char *s, size_t len,
                                     const ccord_text_style *st, float emoji_px)
{
  size_t off = 0, elen = 0;
  if (!md_inline_find_emoji(s, len, &off, &elen)) {
    return ccord_text_measure(r, s, len, st, 0.0f).x;
  }
  float total = 0.0f;
  size_t pos = 0;
  ccord_text_style em_st = ui_style(ui, CCORD_FONT_EMOJI, emoji_px, st->color);
  while (pos < len) {
    size_t rel = 0, ml = 0;
    if (!md_inline_find_emoji(s + pos, len - pos, &rel, &ml)) {
      total += ccord_text_measure(r, s + pos, len - pos, st, 0.0f).x;
      break;
    }
    if (rel > 0) total += ccord_text_measure(r, s + pos, rel, st, 0.0f).x;
    total += ccord_text_measure(r, s + pos + rel, ml, &em_st, 0.0f).x;
    pos += rel + ml;
  }
  return total;
}

static void md_inline_draw_token(ccord_ui *ui, ccord_renderer *r, const char *s, size_t len,
                                 const ccord_text_style *st, float emoji_px, float x, float y, float inner_w,
                                 float lh, float pad_x)
{
  size_t off = 0, elen = 0;
  if (!md_inline_find_emoji(s, len, &off, &elen)) {
    dc_chat_sel_process_run(ui, r, s, len, st, x + pad_x, y, inner_w, lh, pad_x);
    ccord_text_draw(r, s, len, x + pad_x, y, st, 0.0f);
    return;
  }
  float rx = x + pad_x;
  size_t pos = 0;
  ccord_text_style em_st = ui_style(ui, CCORD_FONT_EMOJI, emoji_px, st->color);
  while (pos < len) {
    size_t rel = 0, ml = 0;
    if (!md_inline_find_emoji(s + pos, len - pos, &rel, &ml)) {
      size_t rem = len - pos;
      float sw = ccord_text_measure(r, s + pos, rem, st, 0.0f).x;
      dc_chat_sel_process_run(ui, r, s + pos, rem, st, rx, y, sw, lh, pos == 0 ? pad_x : 0.0f);
      ccord_text_draw(r, s + pos, rem, rx, y, st, 0.0f);
      break;
    }
    if (rel > 0) {
      float sw = ccord_text_measure(r, s + pos, rel, st, 0.0f).x;
      dc_chat_sel_process_run(ui, r, s + pos, rel, st, rx, y, sw, lh, pos == 0 ? pad_x : 0.0f);
      ccord_text_draw(r, s + pos, rel, rx, y, st, 0.0f);
      rx += sw;
    }
    ccord_vec2 em = ccord_text_measure(r, s + pos + rel, ml, &em_st, 0.0f);
    dc_chat_sel_process_run(ui, r, s + pos + rel, ml, &em_st, rx, y, em.x, lh, (pos + rel == 0) ? pad_x : 0.0f);
    ccord_text_draw(r, s + pos + rel, ml, rx, y + (lh - em.y) * 0.5f, &em_st, 0.0f);
    rx += em.x;
    pos += rel + ml;
  }
}

// Lays out one logical line (words with mixed styles), wrapping at the width. Returns its height. When
// draw is true the runs are painted at (x, y).
// mask_stars: Streamer Mode pre-masked line (see dc_md_inline_walk_sized). text then carries
// UI_STREAMER_MASK_BYTE over each invite code and mask_stars is the same line with '*' there; token text
// is drawn from mask_stars so every code is masked as one run even if the tokenizer splits it. nullptr
// when the line carries no masked invite.
static float md_inline_walk_core(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y,
                                 float wrap, ccord_color base, float font_size, bool force_bold, bool draw,
                                 const char *mask_stars)
{
  float lh = ui_line_h(ui, font_size);
  if (!text || len == 0) {
    if (draw) dc_chat_sel_line_end(ui, r, x, y, font_size * 0.3f, lh, true);   // a blank line inside a selection
    return lh;
  }
  ui_md_iter it;
  ui_md_iter_init(&it, text, len);
  if (force_bold) it.bold = true;
  ui_md_token tok;
  ccord_text_style base_st = ui_style(ui, force_bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, font_size, base);
  float space_w = ccord_text_measure(r, " ", 1, &base_st, 0.0f).x;
  float cx = 0.0f, cy = 0.0f;
  bool line_has = false;
  while (ui_md_next(&it, &tok)) {
    bool space = tok.len > 0 && (tok.text[0] == ' ' || tok.text[0] == '\t') && tok.style == UI_MD_TEXT;
    ccord_text_style st = dc_md_token_style_sized(ui, &tok, base, font_size);
    if (force_bold && st.font == CCORD_FONT_REGULAR && tok.style != UI_MD_CODE) st.font = CCORD_FONT_BOLD;

    bool is_custom_emoji = (tok.style == UI_MD_CUSTOM_EMOJI && tok.id != CCORD_ID_NONE);
    bool is_spoiler = (tok.style == UI_MD_SPOILER || tok.spoiler);
    bool spoiler_hidden = is_spoiler && !dc_is_spoiler_revealed(ui, ui ? ui->current_drawing_msg_id : CCORD_ID_NONE);

    char label_buf[160];
    const char *draw_str = tok.text;
    bool is_pill = false;
    size_t draw_len = resolve_token_label(ui, &tok, label_buf, &draw_str, &is_pill);
    // Streamer Mode: an invite code in this token was masked line-wide (see dc_md_inline_walk_sized);
    // draw the '*' copy of the token and make the token inert (no link hit for a masked invite).
    // Mask-free tokens only record any unmasked code for the verify seam.
    bool invite_masked = false;
    if (mask_stars) {
      bool in_text = draw_str >= text && draw_str < text + len;
      bool hit = (in_text && memchr(draw_str, UI_STREAMER_MASK_BYTE, draw_len) != nullptr) ||
                 (tok.url && tok.url_len > 0 && memchr(tok.url, UI_STREAMER_MASK_BYTE, tok.url_len) != nullptr);
      if (in_text && hit) draw_str = mask_stars + (draw_str - text);
      invite_masked = hit;
    }
    if (draw && draw_len > 0) ui_streamer_note_drawn(ui, draw_str, draw_len, invite_masked && draw_str != tok.text);
    bool bg_pill = is_pill || tok.style == UI_MD_CODE || is_spoiler;

    float tw = 0.0f;
    float pad_x = bg_pill ? 4.0f : 0.0f;
    float esz = font_size + 4.0f;
    float inline_em_sz = (tok.style == UI_MD_CODE) ? st.size : esz;
    if (space) {
      tw = space_w * (float)tok.len;
    } else if (is_custom_emoji) {
      tw = esz + 2.0f;
    } else {
      tw = md_inline_measure_token(ui, r, draw_str, draw_len, &st, inline_em_sz) + pad_x * 2.0f;
    }

    if (!space && line_has && cx + tw > wrap) {
      if (draw) {
        dc_chat_sel_line_end(ui, r, x + cx, y + cy, space_w, lh, false);
        dc_record_text_hit(ui, (ccord_rect){x, y + cy, cx, lh});
      }
      cx = 0.0f;
      cy += lh;
      line_has = false;
    }
    if (space && !line_has) continue;   // no leading spaces on a wrapped line
    if (draw && space) dc_chat_sel_process_gap(ui, r, x + cx, y + cy, tok.len, space_w, lh);
    if (draw && !space && ui && ui->dbg_md_msg != CCORD_ID_NONE && ui->dbg_md_msg == ui->current_drawing_msg_id &&
        ui->ndbg_md_runs < 128) {
      unsigned fl = 0;
      if (tok.style == UI_MD_CODE) {
        fl = UI_CF_CODE;
      } else {
        if (tok.style == UI_MD_BOLD || tok.style == UI_MD_BOLD_ITALIC) fl |= UI_CF_BOLD;
        if (tok.style == UI_MD_ITALIC || tok.style == UI_MD_BOLD_ITALIC) fl |= UI_CF_ITALIC;
        if (tok.underline) fl |= UI_CF_UNDER;
        if (tok.strikethrough) fl |= UI_CF_STRIKE;
        if (is_spoiler) fl |= UI_CF_SPOILER;
      }
      size_t di = ui->ndbg_md_runs++;
      size_t dn = draw_len < sizeof ui->dbg_md_runs[di].text - 1 ? draw_len : sizeof ui->dbg_md_runs[di].text - 1;
      memcpy(ui->dbg_md_runs[di].text, draw_str, dn);
      ui->dbg_md_runs[di].text[dn] = '\0';
      ui->dbg_md_runs[di].flags = fl;
    }
    if (draw && !space) {
      if (is_custom_emoji) {
        ccord_rect er = {x + cx + 1.0f, y + cy + (lh - esz) * 0.5f, esz, esz};
        ui_expr_draw_custom_emoji(ui, r, tok.id, tok.text, tok.len, tok.animated, er);
      } else {
        if (bg_pill) {
          ccord_color bg = (tok.style == UI_MD_USER_MENTION || tok.style == UI_MD_CHANNEL_MENTION ||
                            tok.style == UI_MD_ROLE_MENTION || tok.style == UI_MD_MENTION)
                             ? ui_alpha(COL(brand), 0.22f)
                             : (is_spoiler && !spoiler_hidden ? ui_alpha(COL(bg_tertiary), 0.55f) : COL(bg_tertiary));
          ccord_draw_rrect(r, (ccord_rect){x + cx, y + cy + 1.0f, tw, lh - 2.0f}, 3.0f, bg);
        }
        if (is_spoiler && ui && ui->nspoiler_hits < 32 && ui->current_drawing_msg_id != CCORD_ID_NONE) {
          ui->spoiler_hits[ui->nspoiler_hits].rect = (ccord_rect){x + cx, y + cy, tw, lh};
          ui->spoiler_hits[ui->nspoiler_hits].msg_id = ui->current_drawing_msg_id;
          ui->nspoiler_hits++;
        }
        if (!spoiler_hidden) {
          if (is_spoiler) st.color = COL(text_normal);
          md_inline_draw_token(ui, r, draw_str, draw_len, &st, inline_em_sz, x + cx, y + cy, tw - pad_x * 2.0f, lh, pad_x);
        }
        if (tok.style == UI_MD_USER_MENTION || tok.style == UI_MD_MENTION) {
          ccord_id muid = resolve_mention_user_id(ui, &tok);
          if (muid != CCORD_ID_NONE) {
            dc_record_user_hit(ui, (ccord_rect){x + cx, y + cy, tw, lh}, muid);
          }
        }
        if (tok.style == UI_MD_CHANNEL_MENTION && tok.id != CCORD_ID_NONE && ui && ui->nchan_hits < 32) {
          ui->chan_hits[ui->nchan_hits].rect = (ccord_rect){x + cx, y + cy, tw, lh};
          ui->chan_hits[ui->nchan_hits].channel_id = tok.id;
          ui->nchan_hits++;
        }
        if (!spoiler_hidden && tok.style == UI_MD_TIMESTAMP) {
          int64_t ts_ms = 0;
          if (ckd_mul(&ts_ms, tok.timestamp_sec, 1000LL)) {
            ts_ms = tok.timestamp_sec < 0 ? INT64_MIN : INT64_MAX;
          }
          dc_record_ts_hit(ui, (ccord_rect){x + cx, y + cy, tw, lh}, ts_ms, false);
        }
        if (tok.style == UI_MD_LINK && tok.url && tok.url_len > 0 && !invite_masked) {
          ccord_draw_rect(r, (ccord_rect){x + cx, y + cy + lh - 2.0f, tw, 1.0f}, COL(text_link));
          dc_record_link_hit(ui, (ccord_rect){x + cx, y + cy, tw, lh}, tok.url, tok.url_len);
        }
        if (!spoiler_hidden && (tok.style == UI_MD_UNDERLINE || tok.underline)) {
          ccord_draw_rect(r, (ccord_rect){x + cx, y + cy + lh - 2.0f, tw, 1.0f}, st.color);
        }
        if (!spoiler_hidden && (tok.style == UI_MD_STRIKETHROUGH || tok.strikethrough)) {
          ccord_draw_rect(r, (ccord_rect){x + cx, y + cy + lh * 0.52f, tw, 1.0f}, st.color);
        }
      }
    }
    cx += tw;
    line_has = true;
  }
  if (draw) {
    dc_chat_sel_line_end(ui, r, x + cx, y + cy, space_w, lh, true);
    dc_record_text_hit(ui, (ccord_rect){x, y + cy, cx, lh});
  }
  return cy + lh;
}

// Lays out one logical line. While Streamer Mode hides invite links the whole line is masked first
// (invite codes become '*', links to them stop being clickable), so a code the markdown tokenizer
// would split is still masked as one run and nothing is truncated at a token buffer.
float dc_md_inline_walk_sized(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y,
                                  float wrap, ccord_color base, float font_size, bool force_bold, bool draw)
{
  if (text && len > 0 && ui_streamer_invites_active(ui)) {
    char stack_buf[1024];
    char *heap_buf = nullptr;
    char *buf = stack_buf;
    if (len > 512u) {
      heap_buf = ccord_alloc_array(len * 2u, 1u);
      if (!heap_buf) return ui_line_h(ui, font_size);   // out of memory: draw nothing rather than leak the code
      buf = heap_buf;
    }
    char *marked = buf, *stars = buf + len;
    if (ui_streamer_premask(text, len, marked, stars)) {
      float h = md_inline_walk_core(ui, r, marked, len, x, y, wrap, base, font_size, force_bold, draw, stars);
      ccord_free(heap_buf);
      return h;
    }
    ccord_free(heap_buf);
  }
  return md_inline_walk_core(ui, r, text, len, x, y, wrap, base, font_size, force_bold, draw, nullptr);
}

static float md_inline_walk(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y,
                            float wrap, ccord_color base, bool draw)
{
  return dc_md_inline_walk_sized(ui, r, text, len, x, y, wrap, base, M(font_size_base), false, draw);
}

static bool md_fence_is_ansi(const char *tag, size_t n)
{
  while (n && (*tag == ' ' || *tag == '\t')) { tag++; n--; }
  while (n && (tag[n - 1] == ' ' || tag[n - 1] == '\t')) n--;
  return n == 4 && memcmp(tag, "ansi", 4) == 0;
}

static ccord_color md_rgb(uint32_t v)
{
  return (ccord_color){(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, 255};
}

// Records what a code block drew this frame (slice and test hook).
static void md_dbg_code_run(ccord_ui *ui, const char *t, size_t n, ccord_color fg, bool has_bg, ccord_color bg,
                            bool bold, bool underline, bool ansi)
{
  if (!ui || ui->ndbg_code_runs >= 32) return;
  size_t i = ui->ndbg_code_runs++;
  size_t m = n < sizeof ui->dbg_code_runs[i].text - 1 ? n : sizeof ui->dbg_code_runs[i].text - 1;
  memcpy(ui->dbg_code_runs[i].text, t, m);
  ui->dbg_code_runs[i].text[m] = '\0';
  ui->dbg_code_runs[i].fg = fg;
  ui->dbg_code_runs[i].bg = bg;
  ui->dbg_code_runs[i].has_bg = has_bg;
  ui->dbg_code_runs[i].bold = bold;
  ui->dbg_code_runs[i].underline = underline;
  ui->dbg_code_runs[i].ansi = ansi;
}

// Draws one ```ansi line as consecutive styled runs on a single row.
static void md_ansi_draw_runs(ccord_ui *ui, ccord_renderer *r, const char *text, const ccord_ansi_run *runs,
                              size_t nruns, float x, float y, float size, float lh, const ccord_text_style *base)
{
  float cx = x;
  for (size_t i = 0; i < nruns; i++) {
    const ccord_ansi_state *s = &runs[i].st;
    ccord_text_style st = ui_style(ui, s->bold ? CCORD_FONT_MONO_BOLD : CCORD_FONT_MONO, size,
                                   s->has_fg ? md_rgb(s->fg) : base->color);
    ccord_vec2 m = ccord_text_measure(r, text + runs[i].off, runs[i].len, &st, 0.0f);
    if (s->has_bg) ccord_draw_rect(r, (ccord_rect){cx, y, m.x, lh}, md_rgb(s->bg));
    ccord_text_draw(r, text + runs[i].off, runs[i].len, cx, y, &st, 0.0f);
    if (s->underline) ccord_draw_rect(r, (ccord_rect){cx, y + lh - 2.0f, m.x, 1.0f}, st.color);
    md_dbg_code_run(ui, text + runs[i].off, runs[i].len, st.color, s->has_bg, s->has_bg ? md_rgb(s->bg) : st.color,
                    s->bold, s->underline, true);
    cx += m.x;
  }
}

// Draws one syntax-highlighted code line as consecutive monospace token runs on a single row.
static void md_hl_draw_runs(ccord_ui *ui, ccord_renderer *r, const char *text, const ccord_hl_run *runs, size_t nruns,
                            float x, float y, float size, const ccord_text_style *base)
{
  float cx = x;
  ccord_color bg = COL(bg_tertiary);
  for (size_t i = 0; i < nruns; i++) {
    ccord_hl_kind k = runs[i].kind;
    ccord_font_style font = (k == CCORD_HL_KEYWORD) ? CCORD_FONT_MONO_BOLD : CCORD_FONT_MONO;
    ccord_color fg = ccord_md_hl_color(k, bg, base->color);
    ccord_text_style st = ui_style(ui, font, size, fg);
    ccord_vec2 m = ccord_text_measure(r, text + runs[i].off, runs[i].len, &st, 0.0f);
    ccord_text_draw(r, text + runs[i].off, runs[i].len, cx, y, &st, 0.0f);
    cx += m.x;
  }
}

// Fenced code blocks (``` ... ```), headings (#/##/###/-#), > / >>> blockquotes, lists, and plain paragraphs.
// Returns total height and, when draw is true, paints the block decorations and text.
float dc_md_body_walk(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y,
                          float wrap, ccord_color base, bool draw)
{
  if (!text || len == 0) return ui_line_h(ui, M(font_size_base));
  float lh = ui_line_h(ui, M(font_size_base));
  float code_size = maxf(M(font_size_small), 10.0f);
  float code_lh = ui_line_h(ui, code_size);
  ccord_text_style code_st = ui_style(ui, CCORD_FONT_MONO, code_size, COL(text_secondary));
  float total = 0.0f, py = y;
  size_t pos = 0;
  bool fenced = false;
  bool multiline_quote = false;
  bool fence_first_line = false;
  size_t fence_code_start = 0;
  bool fence_ansi = false;
  ccord_hl_lang fence_lang = CCORD_HL_NONE;
  char fence_label[24] = {0};
  ccord_ansi_state ansi_st = {0};
  ccord_hl_state hl_st = {0};
  while (pos <= len) {
    size_t end = pos;
    while (end < len && text[end] != '\n') end++;
    const char *line = text + pos;
    size_t ll = end - pos;
    if (ll > 0 && line[ll - 1] == '\r') ll--;
    if (ll >= 3 && memcmp(line, "```", 3) == 0) {
      fenced = !fenced;
      if (fenced) {
        fence_first_line = true;
        fence_ansi = md_fence_is_ansi(line + 3, ll - 3);
        fence_label[0] = '\0';
        fence_lang = fence_ansi ? CCORD_HL_NONE : ccord_md_hl_detect_lang(line + 3, ll - 3, fence_label, sizeof fence_label);
        ansi_st = (ccord_ansi_state){0};
        hl_st = (ccord_hl_state){0};
        fence_code_start = (end < len) ? (end + 1) : len;
      }
    } else if (fenced) {
      // Streamer Mode: invite codes inside code blocks are masked too (drawn text and Copy).
      char fence_buf[CCORD_MESSAGE_MAX_BYTES + 1];
      size_t fence_ml = ui_streamer_mask_invites(ui, line, ll, fence_buf, sizeof fence_buf, draw);
      const char *fline = fence_ml > 0 ? fence_buf : line;
      size_t flen = fence_ml > 0 ? fence_ml : ll;
      float cw = maxf(wrap - 16.0f, 20.0f);
      // ```ansi: SGR escapes are consumed; the stripped text is measured, selected and drawn in styled runs.
      char ansi_buf[CCORD_MESSAGE_MAX_BYTES + 1];
      ccord_ansi_run ansi_runs[32];
      size_t ansi_nruns = 0;
      if (fence_ansi) {
        flen = ccord_md_ansi_parse(fline, flen, ansi_buf, sizeof ansi_buf, ansi_runs, 32, &ansi_nruns, &ansi_st);
        fline = ansi_buf;
      }
      ccord_hl_run hl_runs[64];
      size_t hl_nruns = 0;
      if (!fence_ansi && fence_lang != CCORD_HL_NONE && flen > 0) {
        hl_nruns = ccord_md_hl_tokenize(fence_lang, fline, flen, hl_runs, 64, &hl_st);
      }
      ccord_vec2 ct = ccord_text_measure(r, fline, flen, &code_st, cw);
      float btn_h = (ui && ui->mobile_layout) ? 26.0f : 18.0f;
      float h = maxf(ct.y, code_lh) + 8.0f;
      if (fence_first_line && h < btn_h + 6.0f) h = btn_h + 6.0f;
      if (draw) {
        ccord_draw_rrect(r, (ccord_rect){x, py, wrap, h}, 4.0f, COL(bg_tertiary));
        ccord_draw_rect(r, (ccord_rect){x, py + 4.0f, 3.0f, h - 8.0f}, COL(brand));
        dc_chat_sel_process_run(ui, r, fline, flen, &code_st, x + 10.0f, py + 4.0f, ct.x, code_lh, 0.0f);
        dc_chat_sel_line_end(ui, r, x + 10.0f + ct.x, py + 4.0f, code_st.size * 0.6f, code_lh, true);
        dc_record_text_hit(ui, (ccord_rect){x + 10.0f, py + 4.0f, ct.x, maxf(ct.y, code_lh)});
        if (fence_ansi && flen && ct.x <= cw + 0.5f) {
          md_ansi_draw_runs(ui, r, fline, ansi_runs, ansi_nruns, x + 10.0f, py + 4.0f, code_size, code_lh, &code_st);
        } else if (hl_nruns > 0 && flen && ct.x <= cw + 0.5f) {
          md_hl_draw_runs(ui, r, fline, hl_runs, hl_nruns, x + 10.0f, py + 4.0f, code_size, &code_st);
          md_dbg_code_run(ui, fline, flen, code_st.color, false, code_st.color, false, false, false);
        } else if (flen) {
          ccord_text_draw(r, fline, flen, x + 10.0f, py + 4.0f, &code_st, cw);
          md_dbg_code_run(ui, fline, flen, code_st.color, false, code_st.color, false, false, fence_ansi);
        }
        if (fence_first_line && ui && ui->ncode_copy_hits < 16) {
          fence_first_line = false;
          float btn_w = ui->mobile_layout ? 56.0f : 44.0f;
          ccord_rect cbtn = {x + wrap - btn_w - 4.0f, py + 3.0f, btn_w, btn_h};
          ccord_draw_rrect(r, cbtn, 3.0f, COL(bg_secondary));
          ccord_text_style cbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_muted));
          ccord_vec2 cbt = ccord_text_measure(r, "Copy", 4, &cbs, 0.0f);
          ccord_text_draw(r, "Copy", 4, cbtn.x + (cbtn.w - cbt.x) * 0.5f, cbtn.y + (cbtn.h - cbt.y) * 0.5f, &cbs, 0.0f);
          if (fence_label[0] != '\0' && wrap > btn_w + 90.0f) {
            size_t llen = strlen(fence_label);
            ccord_vec2 lsz = ccord_text_measure(r, fence_label, llen, &cbs, 0.0f);
            float lw = lsz.x + 10.0f;
            if (x + 10.0f + ct.x + 8.0f < cbtn.x - lw - 4.0f) {
              ccord_rect lpill = {cbtn.x - lw - 4.0f, cbtn.y, lw, btn_h};
              ccord_draw_rrect(r, lpill, 3.0f, COL(bg_secondary));
              ccord_text_draw(r, fence_label, llen, lpill.x + 5.0f, lpill.y + (lpill.h - lsz.y) * 0.5f, &cbs, 0.0f);
            }
          }
          size_t cend = fence_code_start;
          while (cend + 3 <= len) {
            if ((cend == fence_code_start || text[cend - 1] == '\n') && memcmp(text + cend, "```", 3) == 0) break;
            cend++;
          }
          size_t clen = (cend > fence_code_start) ? (cend - fence_code_start) : ll;
          if (clen > 0 && text[fence_code_start + clen - 1] == '\n') clen--;
          if (clen > 0 && text[fence_code_start + clen - 1] == '\r') clen--;
          size_t idx = ui->ncode_copy_hits++;
          ui->code_copy_hits[idx].rect = cbtn;
          ui->code_copy_hits[idx].code = ui_hit_dup(ui, text + fence_code_start, clen);
        }
      }
      py += h;
      total += h;
    } else if (multiline_quote || (ll > 0 && line[0] == '>')) {
      size_t off = 0;
      if (!multiline_quote && line[0] == '>') {
        off = 1;
        if (off < ll && line[off] == '>') {
          off++;
          if (off < ll && line[off] == '>') {
            off++;
            multiline_quote = true;
          }
        }
        if (off < ll && line[off] == ' ') off++;
      }
      float qh = md_inline_walk(ui, r, line + off, ll - off, x + 12.0f, py, maxf(wrap - 12.0f, 20.0f),
                                COL(text_secondary), false);
      if (draw) {
        ccord_draw_rect(r, (ccord_rect){x, py, 3.0f, qh}, COL(brand));
        md_inline_walk(ui, r, line + off, ll - off, x + 12.0f, py, maxf(wrap - 12.0f, 20.0f),
                       COL(text_secondary), true);
      }
      py += qh;
      total += qh;
    } else if (ll > 2 && memcmp(line, "# ", 2) == 0) {
      float h = dc_md_inline_walk_sized(ui, r, line + 2, ll - 2, x, py, wrap, COL(text_header),
                                     M(font_size_large) + 4.0f, true, draw);
      py += h;
      total += h;
    } else if (ll > 3 && memcmp(line, "## ", 3) == 0) {
      float h = dc_md_inline_walk_sized(ui, r, line + 3, ll - 3, x, py, wrap, COL(text_header),
                                     M(font_size_large) + 2.0f, true, draw);
      py += h;
      total += h;
    } else if (ll > 4 && memcmp(line, "### ", 4) == 0) {
      float h = dc_md_inline_walk_sized(ui, r, line + 4, ll - 4, x, py, wrap, COL(text_header),
                                     M(font_size_large), true, draw);
      py += h;
      total += h;
    } else if (ll > 3 && memcmp(line, "-# ", 3) == 0) {
      float h = dc_md_inline_walk_sized(ui, r, line + 3, ll - 3, x, py, wrap, COL(text_muted),
                                     maxf(M(font_size_small) - 1.0f, 10.0f), false, draw);
      py += h;
      total += h;
    } else {
      size_t lead = 0;
      while (lead < ll && lead < 8 && line[lead] == ' ') lead++;
      bool is_bullet = (lead + 2 <= ll && (line[lead] == '-' || line[lead] == '*') && line[lead + 1] == ' ');
      size_t num_end = lead;
      while (num_end < ll && num_end < lead + 4 && line[num_end] >= '0' && line[num_end] <= '9') num_end++;
      bool is_numbered = (!is_bullet && num_end > lead && num_end + 2 <= ll &&
                          line[num_end] == '.' && line[num_end + 1] == ' ');
      if (is_bullet) {
        float indent = (float)lead * 6.0f + 14.0f;
        if (draw) {
          ccord_draw_circle(r, x + (float)lead * 6.0f + 5.0f, py + lh * 0.5f, 2.5f, base);
        }
        float bh = md_inline_walk(ui, r, line + lead + 2, ll - (lead + 2), x + indent, py,
                                  maxf(wrap - indent, 20.0f), base, draw);
        py += bh;
        total += bh;
      } else if (is_numbered) {
        size_t pfx_len = (num_end - lead) + 1u;
        ccord_text_style nst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), base);
        float nw = ccord_text_measure(r, line + lead, pfx_len, &nst, 0.0f).x;
        float indent = (float)lead * 6.0f + nw + 6.0f;
        if (draw) {
          ccord_text_draw(r, line + lead, pfx_len, x + (float)lead * 6.0f, py, &nst, 0.0f);
        }
        float bh = md_inline_walk(ui, r, line + num_end + 2, ll - (num_end + 2), x + indent, py,
                                  maxf(wrap - indent, 20.0f), base, draw);
        py += bh;
        total += bh;
      } else {
        float ph = md_inline_walk(ui, r, line, ll, x, py, wrap, base, draw);
        py += ph;
        total += ph;
      }
    }
    if (end >= len) break;
    pos = end + 1;
  }
  return maxf(total, lh);
}

