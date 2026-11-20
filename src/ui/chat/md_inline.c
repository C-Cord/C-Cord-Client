// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- inline Markdown

void ui_md_iter_init(ui_md_iter *it, const char *text, size_t len)
{
  *it = (ui_md_iter){.p = text, .end = text + (text ? len : 0), .base = text};
}

static bool try_parse_discord_angle_token(const char *p, const char *end, ui_md_token *out, const char **out_next)
{
  if (p >= end || *p != '<') return false;
  const char *gt = p + 1;
  while (gt < end && *gt != '>' && *gt != '<' && *gt != ' ' && *gt != '\t' && *gt != '\n') gt++;
  if (gt >= end || *gt != '>') return false;
  const char *inner = p + 1;
  size_t ilen = (size_t)(gt - inner);
  if (ilen == 0) return false;

  // <@123> or <@!123> user mention
  if (ilen >= 2 && inner[0] == '@' && inner[1] != '&') {
    const char *id_s = (inner[1] == '!') ? inner + 2 : inner + 1;
    size_t id_l = (size_t)(gt - id_s);
    ccord_id uid = CCORD_ID_NONE;
    if (ccord_id_parse(id_s, id_l, &uid)) {
      out->text = id_s;
      out->len = id_l;
      out->id = uid;
      out->style = UI_MD_USER_MENTION;
      *out_next = gt + 1;
      return true;
    }
  }

  // <@&123> role mention
  if (ilen >= 3 && inner[0] == '@' && inner[1] == '&') {
    ccord_id rid = CCORD_ID_NONE;
    if (ccord_id_parse(inner + 2, ilen - 2, &rid)) {
      out->text = inner + 2;
      out->len = ilen - 2;
      out->id = rid;
      out->style = UI_MD_ROLE_MENTION;
      *out_next = gt + 1;
      return true;
    }
  }

  // <#123> channel mention
  if (ilen >= 2 && inner[0] == '#') {
    ccord_id cid = CCORD_ID_NONE;
    if (ccord_id_parse(inner + 1, ilen - 1, &cid)) {
      out->text = inner + 1;
      out->len = ilen - 1;
      out->id = cid;
      out->style = UI_MD_CHANNEL_MENTION;
      *out_next = gt + 1;
      return true;
    }
  }

  // <:name:123> or <a:name:123> custom emoji
  if ((ilen >= 4 && inner[0] == ':') ||
      (ilen >= 5 && (inner[0] == 'a' || inner[0] == 'A') && inner[1] == ':')) {
    bool anim = (inner[0] != ':');
    const char *ns = anim ? inner + 2 : inner + 1;
    const char *colon2 = memchr(ns, ':', (size_t)(gt - ns));
    if (colon2 && colon2 > ns && colon2 + 1 < gt) {
      ccord_id eid = CCORD_ID_NONE;
      if (ccord_id_parse(colon2 + 1, (size_t)(gt - (colon2 + 1)), &eid)) {
        out->text = ns;
        out->len = (size_t)(colon2 - ns);
        out->id = eid;
        out->animated = anim;
        out->style = UI_MD_CUSTOM_EMOJI;
        *out_next = gt + 1;
        return true;
      }
    }
  }

  // <t:1700000000> or <t:1700000000:R> inline timestamp
  if (ilen >= 3 && inner[0] == 't' && inner[1] == ':') {
    const char *ts_s = inner + 2;
    const char *q = ts_s;
    bool neg = (q < gt && *q == '-');
    if (neg) q++;
    int64_t val = 0;
    bool has_digits = false;
    size_t dcount = 0;
    bool overflow = false;
    while (q < gt && *q >= '0' && *q <= '9') {
      has_digits = true;
      dcount++;
      if (dcount > 15) { overflow = true; break; }
      int64_t next_val = 0;
      if (ckd_mul(&next_val, val, 10) || ckd_add(&next_val, next_val, (int64_t)(*q - '0'))) {
        overflow = true;
        break;
      }
      val = next_val;
      q++;
    }
    if (neg) val = -val;
    char tstyle = 'f';
    bool ok = !overflow && has_digits && (q == gt ||
              (q + 2 == gt && q[0] == ':' &&
               (q[1] == 't' || q[1] == 'T' || q[1] == 'd' || q[1] == 'D' ||
                q[1] == 'f' || q[1] == 'F' || q[1] == 'R')));
    if (ok) {
      if (q + 2 == gt) tstyle = q[1];
      out->text = ts_s;
      out->len = (size_t)(gt - ts_s);
      out->timestamp_sec = val;
      out->timestamp_style = tstyle;
      out->style = UI_MD_TIMESTAMP;
      *out_next = gt + 1;
      return true;
    }
  }

  // <https://...> or <http://...> autolink
  if ((ilen >= 9 && memcmp(inner, "https://", 8) == 0) ||
      (ilen >= 8 && memcmp(inner, "http://", 7) == 0)) {
    out->text = inner;
    out->len = ilen;
    out->url = inner;
    out->url_len = ilen;
    out->style = UI_MD_LINK;
    *out_next = gt + 1;
    return true;
  }

  return false;
}

size_t ui_parse_jumbo_emojis(const char *text, size_t len, ui_jumbo_item out[static UI_JUMBO_EMOJI_MAX])
{
  if (!text || len == 0) return 0;
  const char *p = text, *end = text + len;
  size_t count = 0;
  bool pending_nl = false;
  while (p < end) {
    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
      if (*p == '\n') pending_nl = true;
      p++;
      continue;
    }
    if (count >= UI_JUMBO_EMOJI_MAX) return 0;
    if (*p == '<') {
      ui_md_token tok = {0};
      const char *next = nullptr;
      if (try_parse_discord_angle_token(p, end, &tok, &next) &&
          tok.style == UI_MD_CUSTOM_EMOJI && tok.id != CCORD_ID_NONE) {
        out[count] = (ui_jumbo_item){
          .custom = true,
          .animated = tok.animated,
          .line_break_before = count > 0 && pending_nl,
          .id = tok.id,
          .text = tok.text,
          .len = tok.len,
          .raw = p,
          .raw_len = (size_t)(next - p),
        };
        count++;
        pending_nl = false;
        p = next;
        continue;
      }
      return 0;
    }
    size_t el = ccord_emoji_match_prefix(p, (size_t)(end - p));
    if (el == 0) return 0;
    out[count] = (ui_jumbo_item){
      .custom = false,
      .animated = false,
      .line_break_before = count > 0 && pending_nl,
      .id = CCORD_ID_NONE,
      .text = p,
      .len = el,
      .raw = p,
      .raw_len = el,
    };
    count++;
    pending_nl = false;
    p += el;
  }
  return count;
}

// ---- marker pairing. One set of rules for posted messages and the composer (composer_format.c runs this same
// tokenizer): a marker only styles when a closer follows on the line, a backslash before a marker character makes
// it literal, `_x_` is italic only at word edges, and markers inside URLs, <...> tokens, [label](url) links and
// code spans are plain text.
typedef enum { MD_STAR3, MD_STAR2, MD_STAR1, MD_USC2, MD_USC1, MD_TILDE2, MD_PIPE2, MD_NONE } md_mk;

static bool md_space(char c)
{
  return c == ' ' || c == '\t';
}

static bool md_alnum(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80;
}

static bool md_marker_char(char c)
{
  return c == '*' || c == '_' || c == '~' || c == '|' || c == '`' || c == '\\';
}

static size_t md_run(const char *p, const char *end, char c)
{
  size_t n = 0;
  while (p + n < end && p[n] == c) n++;
  return n;
}

// The marker that starts at p (a run is split the way Discord pairs it: *** then ** then *), or MD_NONE.
static md_mk md_marker_run(const char *p, const char *end, size_t *take)
{
  if (*p == '*') {
    size_t k = md_run(p, end, '*');
    *take = k >= 3 ? 3 : k;
    return k >= 3 ? MD_STAR3 : (k == 2 ? MD_STAR2 : MD_STAR1);
  }
  if (*p == '_') {
    size_t k = md_run(p, end, '_');
    *take = k >= 2 ? 2 : 1;
    return k >= 2 ? MD_USC2 : MD_USC1;
  }
  if (*p == '~' && p + 1 < end && p[1] == '~') {
    *take = 2;
    return MD_TILDE2;
  }
  if (*p == '|' && p + 1 < end && p[1] == '|') {
    *take = 2;
    return MD_PIPE2;
  }
  return MD_NONE;
}

static void md_can_pair(const char *base, const char *p, const char *e, const char *end, md_mk t, bool *can_open,
                        bool *can_close)
{
  bool prev_ok = p > base && !md_space(p[-1]) && p[-1] != '\n';
  bool next_ok = e < end && !md_space(*e);
  *can_open = next_ok;
  *can_close = prev_ok;
  if (t == MD_USC1) {   // `_` only at word edges, so snake_case_name stays plain
    *can_open = *can_open && (p == base || !md_alnum(p[-1]));
    *can_close = *can_close && (e >= end || !md_alnum(*e));
  }
}

// A `<@id>`, `[label](url)` or bare URL that starts at p; fills out and next when it does.
static bool md_span_at(const char *base, const char *p, const char *end, ui_md_token *out, const char **next)
{
  if (*p == '<') return try_parse_discord_angle_token(p, end, out, next);
  if (*p == '[') {
    const char *cb = p + 1;
    while (cb < end && *cb != ']' && *cb != '[' && *cb != '\n') cb++;
    if (cb > p + 1 && cb + 1 < end && cb[0] == ']' && cb[1] == '(') {
      const char *us = cb + 2;
      if ((end - us >= 8 && memcmp(us, "https://", 8) == 0) || (end - us >= 7 && memcmp(us, "http://", 7) == 0)) {
        const char *cp = us;
        while (cp < end && *cp != ')' && *cp != ' ' && *cp != '\t' && *cp != '\n') cp++;
        if (cp > us && cp < end && *cp == ')') {
          out->text = p + 1;
          out->len = (size_t)(cb - (p + 1));
          out->url = us;
          out->url_len = (size_t)(cp - us);
          out->style = UI_MD_LINK;
          *next = cp + 1;
          return true;
        }
      }
    }
    return false;
  }
  if (*p == 'h' && (p == base || !md_alnum(p[-1])) &&
      ((end - p >= 9 && memcmp(p, "https://", 8) == 0) || (end - p >= 8 && memcmp(p, "http://", 7) == 0))) {
    const char *q = p;
    while (q < end && *q != ' ' && *q != '\t' && *q != ')' && *q != '>' && *q != ']' && *q != '`') q++;
    while (q > p + 8 && (q[-1] == '.' || q[-1] == ',' || q[-1] == ';')) q--;
    out->text = p;
    out->len = (size_t)(q - p);
    out->url = p;
    out->url_len = out->len;
    out->style = UI_MD_LINK;
    *next = q;
    return true;
  }
  return false;
}

// The start of the first run of exactly n backticks in [from, end), or nullptr. (A code span needs a closer of its
// own length.)
static const char *md_code_closer(const char *from, const char *end, size_t n)
{
  for (const char *k = from; k < end;) {
    if (*k != '`') {
      k++;
      continue;
    }
    size_t m = md_run(k, end, '`');
    if (m == n) return k;
    k += m;
  }
  return nullptr;
}

// Does a marker of type t that opened just before `from` have a closer later on the line?
static bool md_has_closer(const char *base, const char *from, const char *end, md_mk t)
{
  ui_md_token scratch;
  const char *p = from;
  while (p < end) {
    if (*p == '\\' && p + 1 < end && md_marker_char(p[1])) {
      p += 2;
      continue;
    }
    if (*p == '`') {
      size_t n = md_run(p, end, '`');
      const char *k = md_code_closer(p + n, end, n);
      p = k ? k + n : p + n;
      continue;
    }
    const char *nx = nullptr;
    memset(&scratch, 0, sizeof scratch);
    if ((*p == '<' || *p == '[' || *p == 'h') && md_span_at(base, p, end, &scratch, &nx)) {
      p = nx;
      continue;
    }
    size_t take = 0;
    md_mk m = md_marker_run(p, end, &take);
    if (m == MD_NONE) {
      p++;
      continue;
    }
    bool co, cc;
    md_can_pair(base, p, p + take, end, m, &co, &cc);
    if (m == t && cc && p > from) return true;
    p += take;
  }
  return false;
}

static void md_set_state(ui_md_iter *it, md_mk t)
{
  uint8_t o = it->open;
  if (t == MD_STAR3 || t == MD_STAR2) it->bold = (o & ((1u << MD_STAR3) | (1u << MD_STAR2))) != 0;
  if (t == MD_STAR3 || t == MD_STAR1 || t == MD_USC1)
    it->italic = (o & ((1u << MD_STAR3) | (1u << MD_STAR1) | (1u << MD_USC1))) != 0;
  if (t == MD_USC2) it->underline = (o & (1u << MD_USC2)) != 0;
  if (t == MD_TILDE2) it->strikethrough = (o & (1u << MD_TILDE2)) != 0;
  if (t == MD_PIPE2) it->spoiler = (o & (1u << MD_PIPE2)) != 0;
}

static ui_md_style md_state_style(const ui_md_iter *it)
{
  if (it->code) return UI_MD_CODE;
  if (it->bold && it->italic) return UI_MD_BOLD_ITALIC;
  if (it->bold) return UI_MD_BOLD;
  if (it->italic) return UI_MD_ITALIC;
  if (it->spoiler) return UI_MD_SPOILER;
  if (it->underline) return UI_MD_UNDERLINE;
  if (it->strikethrough) return UI_MD_STRIKETHROUGH;
  return UI_MD_TEXT;
}

// Yields words/spaces and strips **bold**, *italic*, _italic_, ***bold-italic***, __underline__, ~~strike~~,
// ||spoiler||, and `code` markers that close on the line; any other marker, and a marker after a backslash, comes
// out as plain text (the backslash itself is dropped unless it->composer). Parses Discord <@id>, <#id>, <@&id>,
// <:name:id>, <t:ts:style>, <url>, [label](url), and bare URLs (it->composer keeps those as plain words).
bool ui_md_next(ui_md_iter *it, ui_md_token *out)
{
  if (!it || !out) return false;
  memset(out, 0, sizeof *out);
  const char *p = it->p, *end = it->end;
  size_t lit = 0;   // bytes at the start of the token that are literal text even though they look like a marker
  for (;;) {
    if (p >= end) {
      it->p = p;
      return false;
    }
    if (it->code) {
      if (*p == '`') {
        size_t n = md_run(p, end, '`');
        if (n == it->code_n) {
          it->code = false;
          p += n;
          continue;
        }
        lit = n;
      }
      break;
    }
    if (*p == '\\' && p + 1 < end && md_marker_char(p[1])) {
      if (it->composer) {
        lit = 2;
      } else {
        p++;
        lit = 1;
      }
      break;
    }
    if (*p == '`') {
      size_t n = md_run(p, end, '`');
      if (md_code_closer(p + n, end, n)) {
        it->code = true;
        it->code_n = n;
        p += n;
        continue;
      }
      lit = n;
      break;
    }
    size_t take = 0;
    md_mk t = md_marker_run(p, end, &take);
    if (t == MD_NONE) break;
    const char *e = p + take;
    uint8_t bit = (uint8_t)(1u << t);
    bool can_open, can_close;
    md_can_pair(it->base, p, e, end, t, &can_open, &can_close);
    if (it->open & bit) {
      if (can_close && p > it->open_end[t]) {
        it->open &= (uint8_t)~bit;
        md_set_state(it, t);
        p = e;
        continue;
      }
    } else if (can_open && md_has_closer(it->base, e, end, t)) {
      it->open |= bit;
      it->open_end[t] = e;
      md_set_state(it, t);
      p = e;
      continue;
    }
    lit = take;
    break;
  }
  out->underline = it->underline;
  out->strikethrough = it->strikethrough;
  out->spoiler = it->spoiler;
  const char *next_p = nullptr;
  if (!it->code && lit == 0 && md_span_at(it->base, p, end, out, &next_p)) {
    if (it->composer) {   // the composer shows these as ordinary words
      memset(out, 0, sizeof *out);
      out->underline = it->underline;
      out->strikethrough = it->strikethrough;
      out->spoiler = it->spoiler;
      out->literal_span = true;
      out->text = p;
      out->len = (size_t)(next_p - p);
      out->style = md_state_style(it);
    }
    it->p = next_p;
    return true;
  }
  const char *start = p;
  ui_md_style style = md_state_style(it);
  bool mention = !it->code && !it->composer && lit == 0 && *p == '@';
  if (lit == 0 && (*p == ' ' || *p == '\t')) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
  } else {
    if (lit == 0 && *p == '[') lit = 1;   // a bracket that opens no link
    p += lit;
    ui_md_token scratch = {0};
    while (p < end) {
      if (*p == ' ' || *p == '\t' || *p == '`') break;
      if (!it->code) {
        if (*p == '*' || *p == '[' || *p == '_') break;
        if (*p == '<' && p > start) break;
        if (*p == '\\' && p + 1 < end && md_marker_char(p[1])) break;
        if (p + 1 < end && ((p[0] == '~' && p[1] == '~') || (p[0] == '|' && p[1] == '|'))) break;
        if (*p == 'h' && p > start && md_span_at(it->base, p, end, &scratch, &next_p)) break;   // a URL after punctuation
      }
      p++;
    }
  }
  if (p == start) p++;   // never stand still
  out->text = start;
  out->len = (size_t)(p - start);
  out->style = mention ? UI_MD_MENTION : style;
  it->p = p;
  return true;
}

ccord_text_style dc_md_token_style_sized(const ccord_ui *ui, const ui_md_token *tok, ccord_color base, float size)
{
  if (tok->style == UI_MD_LINK) return ui_style(ui, CCORD_FONT_REGULAR, size, COL(text_link));
  if (tok->style == UI_MD_MENTION || tok->style == UI_MD_USER_MENTION ||
      tok->style == UI_MD_CHANNEL_MENTION || tok->style == UI_MD_ROLE_MENTION) {
    return ui_style(ui, CCORD_FONT_BOLD, size, COL(text_link));
  }
  if (tok->style == UI_MD_BOLD) return ui_style(ui, CCORD_FONT_BOLD, size, base);
  if (tok->style == UI_MD_BOLD_ITALIC) {
    return ui_style(ui, CCORD_FONT_BOLD, size, ui_mix(base, COL(text_secondary), 0.25f));
  }
  if (tok->style == UI_MD_CODE) return ui_style(ui, CCORD_FONT_REGULAR, maxf(size - 1.0f, 10.0f), COL(text_header));
  if (tok->style == UI_MD_ITALIC) return ui_style(ui, CCORD_FONT_REGULAR, size, ui_mix(base, COL(text_secondary), 0.35f));
  if (tok->style == UI_MD_SPOILER || tok->spoiler) {
    return ui_style(ui, CCORD_FONT_REGULAR, size, COL(text_secondary));
  }
  return ui_style(ui, CCORD_FONT_REGULAR, size, base);
}

void dc_record_link_hit(ccord_ui *ui, ccord_rect rect, const char *url, size_t url_len)
{
  if (!ui || !url || url_len == 0 || url_len >= UI_URL_MAX) return;
  if (ui->nlinks >= UI_LINK_HIT_MAX) return;
  ui_link_hit *hit = &ui->links[ui->nlinks++];
  hit->rect = rect;
  hit->url = ui_hit_dup(ui, url, url_len);
}

// A line of selectable message text: the I-beam cursor shows over these and nowhere else in the message list.
void dc_record_text_hit(ccord_ui *ui, ccord_rect rect)
{
  if (!ui || rect.w <= 0.0f || rect.h <= 0.0f || ui->ntext_hits >= UI_TEXT_HIT_MAX) return;
  ui->text_hits[ui->ntext_hits++] = rect;
}

void dc_record_user_hit(ccord_ui *ui, ccord_rect rect, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE || rect.w <= 0.0f || rect.h <= 0.0f) return;
  if (ui->nuser_hits >= UI_USER_HIT_MAX) return;
  ui->user_hits[ui->nuser_hits++] = (ui_user_hit){rect, user_id};
}

void dc_record_ts_hit(ccord_ui *ui, ccord_rect rect, int64_t ts_ms, bool is_edited)
{
  if (!ui || rect.w <= 0.0f || rect.h <= 0.0f) return;
  if (ui->nts_hits >= UI_TS_HIT_MAX) return;
  // A timestamp scrolled under the header or the composer is not hoverable: keep only the part inside the timeline.
  const ccord_layout_rect *ma = &ui->layout.message_area;
  if (ma->h > 0.0f) {
    float top = fmaxf(rect.y, ma->y), bot = fminf(rect.y + rect.h, ma->y + ma->h);
    if (bot - top < 1.0f) return;
    rect.y = top;
    rect.h = bot - top;
  }
  ui->ts_hits[ui->nts_hits++] = (ui_ts_hit){rect, ts_ms, is_edited};
}

void dc_record_spoiler_hit(ccord_ui *ui, ccord_rect rect, ccord_id key)
{
  if (!ui || key == CCORD_ID_NONE || rect.w <= 0.0f || rect.h <= 0.0f) return;
  if (ui->nspoiler_hits >= 32u) return;
  ui->spoiler_hits[ui->nspoiler_hits].rect = rect;
  ui->spoiler_hits[ui->nspoiler_hits].msg_id = key;
  ui->nspoiler_hits++;
}

void dc_record_img_hit(ccord_ui *ui, ccord_rect rect, const char *const *urls, size_t count, size_t index)
{
  if (!ui || !urls || count == 0 || index >= count || rect.w <= 0.0f || rect.h <= 0.0f) return;
  if (!urls[index] || !urls[index][0] || ui->nimg_hits >= UI_IMG_HIT_MAX) return;
  ui_img_hit *hit = &ui->img_hits[ui->nimg_hits++];
  hit->rect = rect;
  if (count > CCORD_RICH_MAX_ATTACHMENTS) count = CCORD_RICH_MAX_ATTACHMENTS;
  hit->count = count;
  hit->index = index < count ? index : 0;
  for (size_t i = 0; i < count; i++) {
    const char *u = (urls[i] && strlen(urls[i]) < UI_URL_MAX) ? urls[i] : "";
    hit->urls[i] = ui_hit_cstr(ui, u);
  }
  for (size_t i = count; i < CCORD_RICH_MAX_ATTACHMENTS; i++) {
    hit->urls[i] = "";
  }
}

