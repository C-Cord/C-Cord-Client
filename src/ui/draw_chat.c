// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#include "settings_m6_prefs.h"
#include <stdckdint.h>
#include "ui_style.h"
#include "ui_voice_surfaces.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/emoji/emoji.h>
#include <ccord/net/admin.h>
#include <ccord/store/expressions.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

static float maxf(float a, float b)
{
  return a > b ? a : b;
}

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---- per-UI chat layout & resolution caches (lazily heap-allocated) ----
#define CHAT_MENTION_CACHE_SLOTS 64u
typedef struct chat_mention_entry {
  uint64_t hash;        // 0 = empty; FNV-1a of lowercase name
  uint8_t nlen;
  char name[48];
  ccord_id user_id;
} chat_mention_entry;

#define CHAT_ROWH_CACHE_SLOTS 64u
typedef struct chat_rowh_entry {
  uint64_t content_hash;
  ccord_id msg_id;
  int64_t edited_ts;
  uint32_t edit_rev;
  size_t content_len;
  int width_q;
  int scale_q;
  uint32_t flags;
  uint32_t meta_sig;
  float height;
  uint64_t lru_tick;
} chat_rowh_entry;

#define CHAT_EMBEDH_CACHE_SLOTS 32u
typedef struct chat_embedh_entry {
  ccord_id msg_id;
  uint32_t edit_rev;
  uintptr_t emb_ptr;
  int width_q;
  int scale_q;
  bool streamer;
  float height;
  uint64_t lru_tick;
} chat_embedh_entry;

struct ui_chat_caches {
  uint64_t mention_rev;
  ccord_id mention_guild;
  ccord_id mention_chan;
  chat_mention_entry mentions[CHAT_MENTION_CACHE_SLOTS];

  bool br_valid;
  uint64_t br_rev;
  uint64_t br_blocked_sig;
  ccord_id br_chan;
  size_t br_a, br_b;
  ccord_id br_first_id;

  const ccord_renderer *rh_r;
  const ccord_theme *rh_theme;
  int rh_tex_count;
  size_t rh_nrevealed_spoilers;
  size_t rh_nblocked_revealed;
  uint64_t rh_blocked_sig;
  uint64_t rh_tick;
  chat_rowh_entry rowh[CHAT_ROWH_CACHE_SLOTS];
  chat_embedh_entry embedh[CHAT_EMBEDH_CACHE_SLOTS];
};

static struct ui_chat_caches *chat_caches_ensure(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (!ui->chat_caches) ui->chat_caches = ccord_calloc(1, sizeof *ui->chat_caches);
  return ui->chat_caches;
}

void ui_chat_caches_clear(ccord_ui *ui)
{
  if (!ui || !ui->chat_caches) return;
  memset(ui->chat_caches, 0, sizeof *ui->chat_caches);
}

void ui_chat_caches_free(ccord_ui *ui)
{
  if (!ui) return;
  ccord_free(ui->chat_caches);
  ui->chat_caches = nullptr;
}

// Discord plays animated chat media only while gif_auto_play is on (default when the
// settings object has not arrived yet); with it off, hovering one tile still plays it.
static bool ui_gif_auto_play(const ccord_ui *ui)
{
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return us ? us->gif_auto_play : true;
}

static bool ui_media_animate(const ccord_ui *ui, ccord_rect rect)
{
  return ui_gif_auto_play(ui) || (ui->mouse_in && ui_in_rect(ui->mx, ui->my, rect));
}

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
        out[count++] = (ui_jumbo_item){
          .custom = true,
          .animated = tok.animated,
          .line_break_before = count > 0 && pending_nl,
          .id = tok.id,
          .text = tok.text,
          .len = tok.len,
          .raw = p,
          .raw_len = (size_t)(next - p),
        };
        pending_nl = false;
        p = next;
        continue;
      }
      return 0;
    }
    size_t el = ccord_emoji_match_prefix(p, (size_t)(end - p));
    if (el == 0) return 0;
    out[count++] = (ui_jumbo_item){
      .custom = false,
      .animated = false,
      .line_break_before = count > 0 && pending_nl,
      .id = CCORD_ID_NONE,
      .text = p,
      .len = el,
      .raw = p,
      .raw_len = el,
    };
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

static ccord_text_style md_token_style_sized(const ccord_ui *ui, const ui_md_token *tok, ccord_color base, float size)
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

static void record_link_hit(ccord_ui *ui, ccord_rect rect, const char *url, size_t url_len)
{
  if (!ui || !url || url_len == 0 || url_len >= UI_URL_MAX) return;
  if (ui->nlinks >= UI_LINK_HIT_MAX) return;
  ui_link_hit *hit = &ui->links[ui->nlinks++];
  hit->rect = rect;
  hit->url = ui_hit_dup(ui, url, url_len);
}

// A line of selectable message text: the I-beam cursor shows over these and nowhere else in the message list.
static void record_text_hit(ccord_ui *ui, ccord_rect rect)
{
  if (!ui || rect.w <= 0.0f || rect.h <= 0.0f || ui->ntext_hits >= UI_TEXT_HIT_MAX) return;
  ui->text_hits[ui->ntext_hits++] = rect;
}

static void record_user_hit(ccord_ui *ui, ccord_rect rect, ccord_id user_id)
{
  if (!ui || user_id == CCORD_ID_NONE || rect.w <= 0.0f || rect.h <= 0.0f) return;
  if (ui->nuser_hits >= UI_USER_HIT_MAX) return;
  ui->user_hits[ui->nuser_hits++] = (ui_user_hit){rect, user_id};
}

static void record_ts_hit(ccord_ui *ui, ccord_rect rect, int64_t ts_ms, bool is_edited)
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

static void record_spoiler_hit(ccord_ui *ui, ccord_rect rect, ccord_id key)
{
  if (!ui || key == CCORD_ID_NONE || rect.w <= 0.0f || rect.h <= 0.0f) return;
  if (ui->nspoiler_hits >= 32u) return;
  ui->spoiler_hits[ui->nspoiler_hits].rect = rect;
  ui->spoiler_hits[ui->nspoiler_hits].msg_id = key;
  ui->nspoiler_hits++;
}

static void record_img_hit(ccord_ui *ui, ccord_rect rect, const char *const *urls, size_t count, size_t index)
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

static ccord_id spoiler_att_key(ccord_id msg_id, const ccord_attachment *a, size_t idx)
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
static size_t utf8_boundary(const char *s, size_t n, size_t cap)
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
      size_t nl = utf8_boundary(name, strlen(name), 150u);
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
      size_t nl = utf8_boundary(name, strlen(name), 150u);
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
      size_t nl = utf8_boundary(name, strlen(name), 150u);
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

static bool is_spoiler_revealed(const ccord_ui *ui, ccord_id msg_id)
{
  if (!ui || msg_id == CCORD_ID_NONE) return false;
  for (size_t i = 0; i < ui->nrevealed_spoilers; i++) {
    if (ui->revealed_spoilers[i] == msg_id) return true;
  }
  return false;
}

static void chat_sel_append(ccord_ui *ui, const char *s, size_t n, bool new_line)
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
static bool chat_sel_line(const ccord_ui *ui, float ty, float lh, float *x0, float *x1, bool *goes_on)
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

static void chat_sel_fill(ccord_ui *ui, ccord_renderer *r, ccord_rect rc)
{
  ccord_draw_rect(r, rc, COL(selection));
  if (ui->dbg_nsel < sizeof ui->dbg_sel / sizeof ui->dbg_sel[0]) ui->dbg_sel[ui->dbg_nsel++] = rc;
}

// A run of text inside the selection: only the selected characters, snapped to character boundaries.
// `pad` extends the highlight over a pill's padding when the whole run is selected, so the pill reads as one block.
static void chat_sel_process_run(ccord_ui *ui, ccord_renderer *r, const char *str, size_t len,
                                 const ccord_text_style *st, float tx, float ty, float tw, float lh, float pad)
{
  if (len == 0) return;
  float sel_x0, sel_x1;
  bool goes_on;
  if (!chat_sel_line(ui, ty, lh, &sel_x0, &sel_x1, &goes_on)) return;
  if (sel_x1 <= tx || sel_x0 >= tx + tw) return;
  size_t c0 = sel_x0 <= tx ? 0 : ccord_text_hit_test(r, str, len, st, 0.0f, sel_x0 - tx, lh * 0.5f);
  size_t c1 = sel_x1 >= tx + tw ? len : ccord_text_hit_test(r, str, len, st, 0.0f, sel_x1 - tx, lh * 0.5f);
  if (c1 <= c0) return;
  float x0 = c0 > 0 ? ccord_text_measure(r, str, c0, st, 0.0f).x : 0.0f;
  float x1 = ccord_text_measure(r, str, c1, st, 0.0f).x;
  float left = tx + x0, right = tx + x1;
  if (c0 == 0) left -= pad;
  if (c1 == len) right += pad;
  chat_sel_fill(ui, r, (ccord_rect){left, ty, maxf(right - left, 2.0f), lh});
  chat_sel_append(ui, str + c0, c1 - c0, false);
}

// The blank between two words is part of the selection like any other character, so a selected sentence is one
// unbroken band and not a string of separate words. `count` spaces of `space_w` each start at `tx`.
static void chat_sel_process_gap(ccord_ui *ui, ccord_renderer *r, float tx, float ty, size_t count, float space_w,
                                 float lh)
{
  float sel_x0, sel_x1;
  bool goes_on;
  if (count == 0 || space_w <= 0.0f || !chat_sel_line(ui, ty, lh, &sel_x0, &sel_x1, &goes_on)) return;
  float tw = space_w * (float)count;
  if (sel_x1 <= tx || sel_x0 >= tx + tw) return;
  float a = sel_x0 <= tx ? 0.0f : roundf((sel_x0 - tx) / space_w);
  float b = sel_x1 >= tx + tw ? (float)count : roundf((sel_x1 - tx) / space_w);
  if (b <= a) return;
  chat_sel_fill(ui, r, (ccord_rect){tx + a * space_w, ty, (b - a) * space_w, lh});
}

// The end of a line the selection runs past: Discord (a browser) shows the line break as a short highlighted stub after
// the last character, and so do we; it also keeps the band continuous from one line to the next. `hard` marks the end
// of a logical line (a newline in the text, a message), where a copy gets a line break, not a space.
static void chat_sel_line_end(ccord_ui *ui, ccord_renderer *r, float end_x, float ty, float stub_w, float lh, bool hard)
{
  float sel_x0, sel_x1;
  bool goes_on;
  if (!chat_sel_line(ui, ty, lh, &sel_x0, &sel_x1, &goes_on) || !goes_on || sel_x0 > end_x + stub_w) return;
  chat_sel_fill(ui, r, (ccord_rect){end_x, ty, stub_w, lh});
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
    chat_sel_process_run(ui, r, s, len, st, x + pad_x, y, inner_w, lh, pad_x);
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
      chat_sel_process_run(ui, r, s + pos, rem, st, rx, y, sw, lh, pos == 0 ? pad_x : 0.0f);
      ccord_text_draw(r, s + pos, rem, rx, y, st, 0.0f);
      break;
    }
    if (rel > 0) {
      float sw = ccord_text_measure(r, s + pos, rel, st, 0.0f).x;
      chat_sel_process_run(ui, r, s + pos, rel, st, rx, y, sw, lh, pos == 0 ? pad_x : 0.0f);
      ccord_text_draw(r, s + pos, rel, rx, y, st, 0.0f);
      rx += sw;
    }
    ccord_vec2 em = ccord_text_measure(r, s + pos + rel, ml, &em_st, 0.0f);
    chat_sel_process_run(ui, r, s + pos + rel, ml, &em_st, rx, y, em.x, lh, (pos + rel == 0) ? pad_x : 0.0f);
    ccord_text_draw(r, s + pos + rel, ml, rx, y + (lh - em.y) * 0.5f, &em_st, 0.0f);
    rx += em.x;
    pos += rel + ml;
  }
}

// Lays out one logical line (words with mixed styles), wrapping at the width. Returns its height. When
// draw is true the runs are painted at (x, y).
// mask_stars: Streamer Mode pre-masked line (see md_inline_walk_sized). text then carries
// UI_STREAMER_MASK_BYTE over each invite code and mask_stars is the same line with '*' there; token text
// is drawn from mask_stars so every code is masked as one run even if the tokenizer splits it. nullptr
// when the line carries no masked invite.
static float md_inline_walk_core(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y,
                                 float wrap, ccord_color base, float font_size, bool force_bold, bool draw,
                                 const char *mask_stars)
{
  float lh = ui_line_h(ui, font_size);
  if (!text || len == 0) {
    if (draw) chat_sel_line_end(ui, r, x, y, font_size * 0.3f, lh, true);   // a blank line inside a selection
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
    ccord_text_style st = md_token_style_sized(ui, &tok, base, font_size);
    if (force_bold && st.font == CCORD_FONT_REGULAR && tok.style != UI_MD_CODE) st.font = CCORD_FONT_BOLD;

    bool is_custom_emoji = (tok.style == UI_MD_CUSTOM_EMOJI && tok.id != CCORD_ID_NONE);
    bool is_spoiler = (tok.style == UI_MD_SPOILER || tok.spoiler);
    bool spoiler_hidden = is_spoiler && !is_spoiler_revealed(ui, ui ? ui->current_drawing_msg_id : CCORD_ID_NONE);

    char label_buf[160];
    const char *draw_str = tok.text;
    bool is_pill = false;
    size_t draw_len = resolve_token_label(ui, &tok, label_buf, &draw_str, &is_pill);
    // Streamer Mode: an invite code in this token was masked line-wide (see md_inline_walk_sized);
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
        chat_sel_line_end(ui, r, x + cx, y + cy, space_w, lh, false);
        record_text_hit(ui, (ccord_rect){x, y + cy, cx, lh});
      }
      cx = 0.0f;
      cy += lh;
      line_has = false;
    }
    if (space && !line_has) continue;   // no leading spaces on a wrapped line
    if (draw && space) chat_sel_process_gap(ui, r, x + cx, y + cy, tok.len, space_w, lh);
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
            record_user_hit(ui, (ccord_rect){x + cx, y + cy, tw, lh}, muid);
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
          record_ts_hit(ui, (ccord_rect){x + cx, y + cy, tw, lh}, ts_ms, false);
        }
        if (tok.style == UI_MD_LINK && tok.url && tok.url_len > 0 && !invite_masked) {
          ccord_draw_rect(r, (ccord_rect){x + cx, y + cy + lh - 2.0f, tw, 1.0f}, COL(text_link));
          record_link_hit(ui, (ccord_rect){x + cx, y + cy, tw, lh}, tok.url, tok.url_len);
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
    chat_sel_line_end(ui, r, x + cx, y + cy, space_w, lh, true);
    record_text_hit(ui, (ccord_rect){x, y + cy, cx, lh});
  }
  return cy + lh;
}

// Lays out one logical line. While Streamer Mode hides invite links the whole line is masked first
// (invite codes become '*', links to them stop being clickable), so a code the markdown tokenizer
// would split is still masked as one run and nothing is truncated at a token buffer.
static float md_inline_walk_sized(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y,
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
  return md_inline_walk_sized(ui, r, text, len, x, y, wrap, base, M(font_size_base), false, draw);
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
    ccord_text_style st = ui_style(ui, s->bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, s->has_fg ? md_rgb(s->fg) : base->color);
    ccord_vec2 m = ccord_text_measure(r, text + runs[i].off, runs[i].len, &st, 0.0f);
    if (s->has_bg) ccord_draw_rect(r, (ccord_rect){cx, y, m.x, lh}, md_rgb(s->bg));
    ccord_text_draw(r, text + runs[i].off, runs[i].len, cx, y, &st, 0.0f);
    if (s->underline) ccord_draw_rect(r, (ccord_rect){cx, y + lh - 2.0f, m.x, 1.0f}, st.color);
    md_dbg_code_run(ui, text + runs[i].off, runs[i].len, st.color, s->has_bg, s->has_bg ? md_rgb(s->bg) : st.color,
                    s->bold, s->underline, true);
    cx += m.x;
  }
}

// Fenced code blocks (``` ... ```), headings (#/##/###/-#), > / >>> blockquotes, lists, and plain paragraphs.
// Returns total height and, when draw is true, paints the block decorations and text.
static float md_body_walk(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float x, float y,
                          float wrap, ccord_color base, bool draw)
{
  if (!text || len == 0) return ui_line_h(ui, M(font_size_base));
  float lh = ui_line_h(ui, M(font_size_base));
  float code_size = maxf(M(font_size_small), 10.0f);
  float code_lh = ui_line_h(ui, code_size);
  ccord_text_style code_st = ui_style(ui, CCORD_FONT_REGULAR, code_size, COL(text_secondary));
  float total = 0.0f, py = y;
  size_t pos = 0;
  bool fenced = false;
  bool multiline_quote = false;
  bool fence_first_line = false;
  size_t fence_code_start = 0;
  bool fence_ansi = false;
  ccord_ansi_state ansi_st = {0};
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
        ansi_st = (ccord_ansi_state){0};
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
      ccord_vec2 ct = ccord_text_measure(r, fline, flen, &code_st, cw);
      float h = maxf(ct.y, code_lh) + 8.0f;
      if (draw) {
        ccord_draw_rrect(r, (ccord_rect){x, py, wrap, h}, 4.0f, COL(bg_tertiary));
        ccord_draw_rect(r, (ccord_rect){x, py + 4.0f, 3.0f, h - 8.0f}, COL(brand));
        chat_sel_process_run(ui, r, fline, flen, &code_st, x + 10.0f, py + 4.0f, ct.x, code_lh, 0.0f);
        chat_sel_line_end(ui, r, x + 10.0f + ct.x, py + 4.0f, code_st.size * 0.6f, code_lh, true);
        record_text_hit(ui, (ccord_rect){x + 10.0f, py + 4.0f, ct.x, maxf(ct.y, code_lh)});
        if (fence_ansi && flen && ct.x <= cw + 0.5f) {
          md_ansi_draw_runs(ui, r, fline, ansi_runs, ansi_nruns, x + 10.0f, py + 4.0f, code_size, code_lh, &code_st);
        } else if (flen) {
          ccord_text_draw(r, fline, flen, x + 10.0f, py + 4.0f, &code_st, cw);
          md_dbg_code_run(ui, fline, flen, code_st.color, false, code_st.color, false, false, fence_ansi);
        }
        if (fence_first_line && ui && ui->ncode_copy_hits < 16) {
          fence_first_line = false;
          ccord_rect cbtn = {x + wrap - 48.0f, py + 3.0f, 44.0f, 18.0f};
          ccord_draw_rrect(r, cbtn, 3.0f, COL(bg_secondary));
          ccord_text_style cbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_muted));
          ccord_vec2 cbt = ccord_text_measure(r, "Copy", 4, &cbs, 0.0f);
          ccord_text_draw(r, "Copy", 4, cbtn.x + (cbtn.w - cbt.x) * 0.5f, cbtn.y + (cbtn.h - cbt.y) * 0.5f, &cbs, 0.0f);
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
      float h = md_inline_walk_sized(ui, r, line + 2, ll - 2, x, py, wrap, COL(text_header),
                                     M(font_size_large) + 4.0f, true, draw);
      py += h;
      total += h;
    } else if (ll > 3 && memcmp(line, "## ", 3) == 0) {
      float h = md_inline_walk_sized(ui, r, line + 3, ll - 3, x, py, wrap, COL(text_header),
                                     M(font_size_large) + 2.0f, true, draw);
      py += h;
      total += h;
    } else if (ll > 4 && memcmp(line, "### ", 4) == 0) {
      float h = md_inline_walk_sized(ui, r, line + 4, ll - 4, x, py, wrap, COL(text_header),
                                     M(font_size_large), true, draw);
      py += h;
      total += h;
    } else if (ll > 3 && memcmp(line, "-# ", 3) == 0) {
      float h = md_inline_walk_sized(ui, r, line + 3, ll - 3, x, py, wrap, COL(text_muted),
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

// ---------------------------------------------------------------- chat header

// A bookmark ribbon: rectangle body with a notched (swallowtail) bottom, drawn from primitives.
static void ui_icon_bookmark(ccord_renderer *r, float cx, float cy, float size, ccord_color c)
{
  float w = size * 0.62f, h = size * 0.86f;
  float x = cx - w * 0.5f, y = cy - h * 0.5f;
  float notch = h * 0.28f;
  ccord_draw_rect(r, (ccord_rect){x, y, w, h - notch}, c);
  ccord_draw_triangle(r, (ccord_vec2){x, y + h - notch}, (ccord_vec2){x + w * 0.5f, y + h - notch},
                      (ccord_vec2){x, y + h}, c);
  ccord_draw_triangle(r, (ccord_vec2){x + w, y + h - notch}, (ccord_vec2){x + w * 0.5f, y + h - notch},
                      (ccord_vec2){x + w, y + h}, c);
}

void ui_draw_chat_header(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_layout_rect *h = &ui->layout.chat_header;
  ccord_draw_rect(r, ui_rect(*h), COL(bg_primary));
  ccord_draw_rect(r, (ccord_rect){h->x, h->y + h->h - 1.0f, h->w, 1.0f}, COL(border_subtle));
  bool is_1to1_dm_panel = ui_panel_is_profile(ui);
  uint32_t tot_mentions = 0;
  uint32_t tot_unreads = ui_guild_unread(ui, CCORD_DM_GUILD_ID, &tot_mentions);
  {
    size_t ng_hdr = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng_hdr; gi++) {
      const ccord_guild *gg = ccord_store_guild_at(ui->d.store, gi);
      if (!gg) continue;
      uint32_t gm = 0;
      tot_unreads += ui_guild_unread(ui, gg->id, &gm);
      tot_mentions += gm;
    }
  }
  ccord_rect db = ui_drawer_btn_rect(ui);
  if (db.w > 0.0f) {
    ccord_color dic = uis_icon_button(ui, r, db, (uis_state){.selected = ui->left_drawer_open}, "hdrib", 5);
    float dcx = db.x + db.w * 0.5f, dcy = db.y + db.h * 0.5f;
    for (int bi = -1; bi <= 1; bi++) {
      ccord_draw_rrect(r, (ccord_rect){dcx - 8.0f, dcy + (float)bi * 5.0f - 1.0f, 16.0f, 2.0f}, 1.0f, dic);
    }
    if (tot_mentions > 0) {
      char blbl[16];
      if (tot_mentions > 99u) snprintf(blbl, sizeof blbl, "99+");
      else snprintf(blbl, sizeof blbl, "%u", (unsigned)tot_mentions);
      ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
      ccord_vec2 bt = ccord_text_measure(r, blbl, strlen(blbl), &bst, 0.0f);
      float bh = 14.0f, bw = maxf(bt.x + 8.0f, 14.0f);
      float bx = db.x + db.w - bw + 2.0f, by = db.y + 1.0f;
      ccord_draw_rrect(r, (ccord_rect){bx, by, bw, bh}, bh * 0.5f, COL(danger));
      ccord_text_draw(r, blbl, strlen(blbl), bx + (bw - bt.x) * 0.5f, by + (bh - bt.y) * 0.5f, &bst, 0.0f);
    } else if (tot_unreads > 0) {
      ccord_draw_circle(r, db.x + db.w - 6.0f, db.y + 6.0f, 3.5f, COL(brand));
    }
  }
  ccord_rect mb = ui_members_btn_rect(ui);
  if (mb.w > 0.0f) {
    bool hover = ui->hover_members_btn && !ui->menu.open && !ui->settings_open;
    bool hidden = ui->mobile_layout ? !ui->right_drawer_open : ui_panel_hidden(ui);
    // Shown panel: the button reads as active (selected fill); hidden: a plain secondary glyph.
    ccord_color ic = uis_icon_button(ui, r, mb, (uis_state){.hover = hover, .selected = !hidden}, "hdrib", 0);
    if (!hidden) ic = COL(text_header);
    if (is_1to1_dm_panel) {
      ui_icon_user_profile(r, mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f, 22.0f, ic, COL(bg_primary));
    } else {
      ui_icon_members(r, mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f, 24.0f, ic, COL(bg_primary));
    }
  }
  ccord_rect sr = ui_search_rect(ui);
  ccord_color sic = uis_icon_button(ui, r, sr, (uis_state){.hover = ui->hover_search && !ui->go_live_open, .selected = ui->search_open},
                                    "hdrib", 1);
  ui_icon_search(r, sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f, 18.0f, sic);
  ccord_rect pr = ui_pin_rect(ui);
  ccord_color pic = uis_icon_button(ui, r, pr, (uis_state){.hover = ui->hover_pin && !ui->go_live_open, .selected = ui->pins_open},
                                    "hdrib", 2);
  ui_icon_pin(r, pr.x + pr.w * 0.5f, pr.y + pr.h * 0.5f, 18.0f, pic);
  ccord_rect ir = ui_inbox_rect(ui);
  if (ir.w > 0.0f) {
    ccord_color iic = uis_icon_button(ui, r, ir, (uis_state){.hover = ui->hover_inbox && !ui->go_live_open, .selected = ui->inbox_open},
                                      "hdrib", 3);
    ui_icon_inbox(r, ir.x + ir.w * 0.5f, ir.y + ir.h * 0.5f, 18.0f, iic, COL(bg_primary));
    if (tot_mentions > 0) {
      char blbl[16];
      if (tot_mentions > 99u) snprintf(blbl, sizeof blbl, "99+");
      else snprintf(blbl, sizeof blbl, "%u", (unsigned)tot_mentions);
      ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
      ccord_vec2 bt = ccord_text_measure(r, blbl, strlen(blbl), &bst, 0.0f);
      float bh = 14.0f, bw = maxf(bt.x + 8.0f, 14.0f);
      float bx = ir.x + ir.w - bw + 2.0f, by = ir.y + 1.0f;
      ccord_draw_rrect(r, (ccord_rect){bx, by, bw, bh}, bh * 0.5f, COL(danger));
      ccord_text_draw(r, blbl, strlen(blbl), bx + (bw - bt.x) * 0.5f, by + (bh - bt.y) * 0.5f, &bst, 0.0f);
    } else if (tot_unreads > 0) {
      ccord_draw_circle(r, ir.x + ir.w - 6.0f, ir.y + 6.0f, 3.5f, COL(brand));
    }
  }
  ccord_rect bmr = ui_saved_messages_rect(ui);
  if (bmr.w > 0.0f) {
    ccord_color bic = uis_icon_button(
        ui, r, bmr, (uis_state){.hover = ui->hover_saved_msgs && !ui->go_live_open, .selected = ui->saved_popover_open}, "hdrib", 4);
    ui_icon_bookmark(r, bmr.x + bmr.w * 0.5f, bmr.y + bmr.h * 0.5f, 18.0f, bic);
  }
  float hdr_leftmost = bmr.w > 0.0f ? bmr.x : (ir.w > 0.0f ? ir.x : pr.x);
  if (ui->account_view.demo_chip_visible && !(ui->mobile_layout && ui->win_w < 500.0f)) {
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_secondary));
    const char *label = "Demo data";
    ccord_vec2 ct = ccord_text_measure(r, label, strlen(label), &cs, 0.0f);
    float right = hdr_leftmost - 8.0f;
    ccord_rect chip = {right - (ct.x + 18.0f), h->y + (h->h - 22.0f) * 0.5f, ct.x + 18.0f, 22.0f};
    ccord_draw_rrect(r, chip, 5.0f, COL(bg_tertiary));
    ccord_text_draw(r, label, strlen(label), chip.x + 9.0f, chip.y + (chip.h - ct.y) * 0.5f, &cs, 0.0f);
    hdr_leftmost = chip.x;
  }
  float left_x = db.w > 0.0f ? (db.x + db.w + 4.0f) : (h->x + 16.0f);
  ui->gdm_add_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->gdm_edit_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->dm_requests_tab_rects[0] = (ccord_rect){0, 0, 0, 0};
  ui->dm_requests_tab_rects[1] = (ccord_rect){0, 0, 0, 0};
  float cy = h->y + h->h * 0.5f;
  if (ui_is_dm_requests_view(ui)) {
    ui->add_friend_btn_rect = (ccord_rect){0, 0, 0, 0};
    ui_icon_inbox(r, left_x + 10.0f, cy, 20.0f, COL(text_muted), COL(bg_primary));
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    float ty = cy - ui_line_h(ui, ns.size) * 0.5f;
    float nw = ccord_text_draw_ellipsis(r, "Message Requests", 16, left_x + 30.0f, ty, maxf(hdr_leftmost - (left_x + 36.0f), 60.0f), &ns);
    float dx = left_x + 30.0f + nw + 16.0f;
    ccord_draw_rect(r, (ccord_rect){dx, cy - 12.0f, 1.0f, 24.0f}, COL(divider));
    uint32_t req_cnt = 0, spam_cnt = 0;
    size_t dmn = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
    for (size_t di = 0; di < dmn; di++) {
      const ccord_channel *mc = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, di);
      if (!mc || !mc->is_message_request) continue;
      if (mc->is_spam) spam_cnt++;
      else req_cnt++;
    }
    char tab0[32], tab1[32];
    snprintf(tab0, sizeof tab0, "Requests (%u)", (unsigned)req_cnt);
    snprintf(tab1, sizeof tab1, "Spam (%u)", (unsigned)spam_cnt);
    const char *rtabs[2] = {tab0, tab1};
    float tx = dx + 16.0f;
    for (int t = 0; t < 2; t++) {
      bool active = (ui->dm_requests_tab == t);
      ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     active ? COL(text_header) : COL(text_secondary));
      ccord_vec2 tt = ccord_text_measure(r, rtabs[t], strlen(rtabs[t]), &ts, 0.0f);
      ccord_rect tab_r = {tx, cy - 13.0f, tt.x + 20.0f, 26.0f};
      ui->dm_requests_tab_rects[t] = tab_r;
      ui_row_state tab_rs = ui_row_state_of(ui, "reqtab", (uint64_t)t, ui->mouse_in && ui_in_rect(ui->mx, ui->my, tab_r),
                                            active, COL(bg_primary));
      ui_row_state_fill(ui, r, &tab_rs, tab_r, 4.0f);
      ccord_text_draw(r, rtabs[t], strlen(rtabs[t]), tab_r.x + 10.0f, tab_r.y + (tab_r.h - tt.y) * 0.5f, &ts, 0.0f);
      tx += tab_r.w + 8.0f;
    }
    return;
  }
  if (ui_is_dm_friends_view(ui)) {
    ui_icon_members(r, left_x + 10.0f, cy, 20.0f, COL(text_muted), COL(bg_primary));
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    float ty = cy - ui_line_h(ui, ns.size) * 0.5f;
    float nw = ccord_text_draw_ellipsis(r, "Friends", 7, left_x + 30.0f, ty, 120.0f, &ns);
    float dx = left_x + 30.0f + nw + 16.0f;
    ccord_draw_rect(r, (ccord_rect){dx, cy - 12.0f, 1.0f, 24.0f}, COL(divider));
    static const char *const k_tabs[4] = {"Online", "All", "Pending", "Blocked"};
    float tx = dx + 16.0f;
    for (int t = 0; t < 4; t++) {
      bool active = (ui->dm_friends_tab == t);
      ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     active ? COL(text_header) : COL(text_secondary));
      ccord_vec2 tt = ccord_text_measure(r, k_tabs[t], strlen(k_tabs[t]), &ts, 0.0f);
      ccord_rect tab_r = {tx, cy - 13.0f, tt.x + 20.0f, 26.0f};
      ui->dm_friends_tab_rects[t] = tab_r;
      ui_row_state tab_rs = ui_row_state_of(ui, "ftab", (uint64_t)t, ui->mouse_in && ui_in_rect(ui->mx, ui->my, tab_r), active,
                                            COL(bg_primary));
      ui_row_state_fill(ui, r, &tab_rs, tab_r, 4.0f);
      ccord_text_draw(r, k_tabs[t], strlen(k_tabs[t]), tab_r.x + 10.0f, tab_r.y + (tab_r.h - tt.y) * 0.5f, &ts, 0.0f);
      tx += tab_r.w + 8.0f;
    }
    ccord_text_style afs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
    ccord_vec2 aft = ccord_text_measure(r, "Add Friend", 10, &afs, 0.0f);
    ccord_rect af_r = {tx + 4.0f, cy - 13.0f, aft.x + 20.0f, 26.0f};
    ui->add_friend_btn_rect = (ccord_rect){0, 0, 0, 0};
    if (af_r.x + af_r.w < ir.x - 12.0f) {
      ui->add_friend_btn_rect = af_r;
      ccord_draw_rrect(r, af_r, 4.0f, COL(status_online));
      ccord_text_draw(r, "Add Friend", 10, af_r.x + 10.0f, af_r.y + (af_r.h - aft.y) * 0.5f, &afs, 0.0f);
    }
    return;
  }
  ui->add_friend_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->thread_join_rect = (ccord_rect){0, 0, 0, 0};
  ui->thread_archive_rect = (ccord_rect){0, 0, 0, 0};
  ui->thread_breadcrumb_rect = (ccord_rect){0, 0, 0, 0};
  ui_forum_set_breadcrumb_rect(ui, (ccord_rect){0}, CCORD_ID_NONE);
  const ccord_channel *ch = ui_selected_channel(ui);
  bool voice_stage = false;
  if (ui->stage_channel != CCORD_ID_NONE && !ui_stage_split_active(ui)) {
    const ccord_channel *sc = ccord_store_channel(ui->d.store, ui->stage_channel);
    if (sc) {
      ch = sc;
      voice_stage = true;
    }
  }
  if (!ch) return;
  bool is_dm = ch->guild_id == CCORD_DM_GUILD_ID && !voice_stage;
  bool group_dm = is_dm && ui_channel_is_group_dm(ch);
  const ccord_user *dm_u = is_dm ? ui_dm_recipient(ui, ch) : nullptr;
  const char *ch_name = is_dm ? ui_dm_display_name(ui, ch) : ch->name;
  bool is_thread_ch = !voice_stage && !is_dm && ccord_discord_type_is_thread(ch->discord_type);
  const ccord_channel *parent_ch = (!voice_stage && !is_dm && ch->parent_id != CCORD_ID_NONE &&
                                    (is_thread_ch || ccord_store_channel_is_forum(ui->d.store, ch->parent_id)))
                                     ? ccord_store_channel(ui->d.store, ch->parent_id)
                                     : nullptr;
  bool is_forum_parent = parent_ch && ccord_store_channel_is_forum(ui->d.store, parent_ch->id);
  if (voice_stage) {
    ui_icon_speaker(r, left_x + 10.0f, cy, 20.0f, COL(text_muted));
  } else if (group_dm) {
    ui_icon_members(r, left_x + 10.0f, cy, 20.0f, COL(text_muted), COL(bg_primary));
  } else if (is_dm) {
    ccord_text_style ats = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_muted));
    ccord_vec2 att = ccord_text_measure(r, "@", 1, &ats, 0.0f);
    ccord_text_draw(r, "@", 1, left_x + 10.0f - att.x * 0.5f, cy - att.y * 0.5f, &ats, 0.0f);
  } else if (is_forum_parent || ccord_store_channel_is_forum(ui->d.store, ch->id)) {
    ui_icon_forum(r, left_x + 10.0f, cy, 20.0f, COL(text_muted));
  } else {
    ui_icon_hash(r, left_x + 10.0f, cy, 20.0f, COL(text_muted));
  }
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  float tx = left_x + 30.0f;
  float ty = cy - ui_line_h(ui, ns.size) * 0.5f;
  float nw = 0.0f;
  float hdr_btn_left = hdr_leftmost - 8.0f;
  float title_max_w = ui->mobile_layout ? maxf(hdr_btn_left - tx - 8.0f, 40.0f) : (h->w * 0.5f);
  if (parent_ch && parent_ch->name) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_secondary));
    float pnw = ccord_text_draw_ellipsis(r, parent_ch->name, strlen(parent_ch->name), tx, ty, h->w * 0.25f, &ps);
    ccord_rect bc_r = {left_x - 4.0f, h->y + 6.0f, (tx - (left_x - 4.0f)) + pnw + 6.0f, h->h - 12.0f};
    ui->thread_breadcrumb_rect = bc_r;
    ui_forum_set_breadcrumb_rect(ui, bc_r, parent_ch->id);
    ui_icon_chevron(r, tx + pnw + 10.0f, cy, 10.0f, ICON_RIGHT, COL(text_muted));
    float post_x = tx + pnw + 20.0f;
    float post_w = ccord_text_draw_ellipsis(r, ch_name, strlen(ch_name), post_x, ty, maxf(hdr_btn_left - post_x - 8.0f, 30.0f), &ns);
    nw = (post_x - tx) + post_w;
    if (is_thread_ch && ch->thread_archived) {
      ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_vec2 abt = ccord_text_measure(r, "Archived", 8, &abs, 0.0f);
      ccord_rect arch_tag = {tx + nw + 8.0f, cy - 10.0f, abt.x + 12.0f, 20.0f};
      ccord_draw_rrect(r, arch_tag, 4.0f, COL(bg_tertiary));
      ccord_text_draw(r, "Archived", 8, arch_tag.x + 6.0f, arch_tag.y + (arch_tag.h - abt.y) * 0.5f, &abs, 0.0f);
      nw += arch_tag.w + 8.0f;
    }
  } else {
    nw = ccord_text_draw_ellipsis(r, ch_name, strlen(ch_name), tx, ty, title_max_w, &ns);
  }
  if (is_thread_ch) {
    ccord_id self_id = ccord_store_self_id(ui->d.store);
    ccord_id perm_ch = ch->parent_id != CCORD_ID_NONE ? ch->parent_id : ch->id;
    bool can_manage_threads = ccord_store_has_permission(ui->d.store, ch->guild_id, perm_ch, self_id,
                                                         CCORD_PERM_MANAGE_THREADS);
    bool can_toggle_arch = can_manage_threads || (ch->thread_archived && !ch->thread_locked);
    if (can_toggle_arch) {
      const char *albl = ch->thread_archived ? "Unarchive" : "Archive";
      ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_secondary));
      ccord_vec2 abt = ccord_text_measure(r, albl, strlen(albl), &abs, 0.0f);
      float bw = abt.x + 18.0f;
      ccord_rect ar = {hdr_btn_left - bw, cy - 12.0f, bw, 24.0f};
      if (ar.x > tx + nw + 12.0f) {
        ui->thread_archive_rect = ar;
        bool ahv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ar);
        ccord_draw_rrect(r, ar, 4.0f, ahv ? COL(bg_item_hover) : COL(bg_tertiary));
        ccord_text_draw(r, albl, strlen(albl), ar.x + 9.0f, ar.y + (ar.h - abt.y) * 0.5f, &abs, 0.0f);
        hdr_btn_left = ar.x - 6.0f;
      }
    }
    if (!ch->thread_archived) {
      const char *jlbl = ch->thread_joined ? "Joined" : "Join";
      ccord_text_style jbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                      ch->thread_joined ? COL(text_secondary) : COL(text_on_brand));
      ccord_vec2 jbt = ccord_text_measure(r, jlbl, strlen(jlbl), &jbs, 0.0f);
      float bw = jbt.x + 18.0f;
      ccord_rect jr = {hdr_btn_left - bw, cy - 12.0f, bw, 24.0f};
      if (jr.x > tx + nw + 12.0f) {
        ui->thread_join_rect = jr;
        bool jhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, jr);
        ccord_color jbg = ch->thread_joined ? (jhv ? COL(bg_item_hover) : COL(bg_tertiary)) : COL(brand);
        ccord_draw_rrect(r, jr, 4.0f, jbg);
        ccord_text_draw(r, jlbl, strlen(jlbl), jr.x + 9.0f, jr.y + (jr.h - jbt.y) * 0.5f, &jbs, 0.0f);
        hdr_btn_left = jr.x - 6.0f;
      }
    }
  } else if (group_dm) {
    const char *elbl = "Edit Group";
    ccord_text_style ebs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_secondary));
    ccord_vec2 ebt = ccord_text_measure(r, elbl, strlen(elbl), &ebs, 0.0f);
    float ebw = ebt.x + 16.0f;
    ccord_rect er = {hdr_btn_left - ebw, cy - 12.0f, ebw, 24.0f};
    if (er.x > tx + nw + 12.0f) {
      ui->gdm_edit_btn_rect = er;
      bool ehv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, er);
      ccord_draw_rrect(r, er, 4.0f, ehv ? COL(bg_item_hover) : COL(bg_tertiary));
      ccord_text_draw(r, elbl, strlen(elbl), er.x + 8.0f, er.y + (er.h - ebt.y) * 0.5f, &ebs, 0.0f);
      hdr_btn_left = er.x - 6.0f;
    }
    const char *albl = "Add Member";
    ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
    ccord_vec2 abt = ccord_text_measure(r, albl, strlen(albl), &abs, 0.0f);
    float abw = abt.x + 16.0f;
    ccord_rect ar = {hdr_btn_left - abw, cy - 12.0f, abw, 24.0f};
    if (ar.x > tx + nw + 12.0f) {
      ui->gdm_add_btn_rect = ar;
      ccord_draw_rrect(r, ar, 4.0f, COL(brand));
      ccord_text_draw(r, albl, strlen(albl), ar.x + 8.0f, ar.y + (ar.h - abt.y) * 0.5f, &abs, 0.0f);
      hdr_btn_left = ar.x - 6.0f;
    }
  }
  if (is_dm && dm_u) {
    ccord_color dot = COL(status_offline);
    switch (dm_u->presence) {
      case CCORD_PRESENCE_ONLINE: dot = COL(status_online); break;
      case CCORD_PRESENCE_IDLE: dot = COL(status_idle); break;
      case CCORD_PRESENCE_DND: dot = COL(status_dnd); break;
      case CCORD_PRESENCE_OFFLINE: dot = COL(status_offline); break;
    }
    ccord_draw_circle(r, tx + nw + 10.0f, cy, 4.5f, dot);
    nw += 18.0f;
  }
  if (is_dm) ui_call_draw_header_button(ui, r, &hdr_btn_left, cy, tx + nw + 12.0f);
  char gdm_topic[48];
  const char *topic = voice_stage ? "Voice / Stream Stage" : ch->topic;
  if (group_dm && (!topic || !topic[0])) {
    snprintf(gdm_topic, sizeof gdm_topic, "%zu Members", ui_dm_member_count(ch));
    topic = gdm_topic;
  }
  if (topic && topic[0]) {
    float dx = tx + nw + 12.0f;
    float right = hdr_btn_left < (mb.w > 0.0f ? mb.x - 8.0f : h->x + h->w - 16.0f)
                    ? hdr_btn_left
                    : (mb.w > 0.0f ? mb.x - 8.0f : h->x + h->w - 16.0f);
    if (right > dx + 24.0f) {
      ccord_draw_rect(r, (ccord_rect){dx, cy - 12.0f, 1.0f, 24.0f}, COL(divider));
      ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
      float tty = cy - ui_line_h(ui, ts.size) * 0.5f;
      ccord_text_draw_ellipsis(r, topic, strlen(topic), dx + 13.0f, tty, right - (dx + 13.0f), &ts);
    }
  }
}

void ui_draw_chat_header_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->hover_members_btn || ui->menu.open || ui->settings_open) return;
  ccord_rect mb = ui_members_btn_rect(ui);
  if (mb.w <= 0.0f) return;
  bool is_1to1_dm = ui_panel_is_profile(ui);
  bool hidden = ui_panel_hidden(ui);
  const char *label = is_1to1_dm ? (hidden ? "Show User Profile" : "Hide User Profile")
                                 : (hidden ? "Show Member List" : "Hide Member List");
  (void)uis_tooltip(ui, r, mb, label, UIS_TIP_BELOW, 1);
}

// The hover bar's Edit button is offered for the messages the context menu's "Edit Message" entry is: your own.
bool ui_message_editable_by_self(const ccord_ui *ui, const ccord_message *m)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!m || self == CCORD_ID_NONE || m->author_id != self) return false;
  if (ccord_message_type_is_system(ccord_message_type(m))) return false;
  if ((ccord_message_flags(m) & (CCORD_MSGFLAG_VOICE_MESSAGE | CCORD_MSGFLAG_IS_COMPONENTS_V2)) != 0) return false;
  if (m->rich && (m->rich->present & (CCORD_RICH_HAS_POLL | CCORD_RICH_HAS_FORWARD)) != 0) return false;
  return true;
}

bool ui_bar_edit_hovered(const ccord_ui *ui)
{
  return ui->mouse_in && !ui->menu.open && !ui->settings_open && ui->hover_bar_edit_rect.w > 0.0f &&
         ui_in_rect(ui->mx, ui->my, ui->hover_bar_edit_rect);
}

// Drawn in the overlay pass (ui.c) so the message area's clip does not cut it off.
void ui_draw_bar_edit_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_bar_edit_hovered(ui)) return;
  (void)uis_tooltip(ui, r, ui->hover_bar_edit_rect, "Edit", UIS_TIP_ABOVE, 1);
}

void ui_draw_timestamp_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  ui->tooltip_text[0] = '\0';
  if (ui->hover_ts_hit < 0 || (size_t)ui->hover_ts_hit >= ui->nts_hits ||
      ui->menu.open || ui->settings_open || ui->pins_open || ui->search_open) {
    return;
  }
  const ui_ts_hit *hit = &ui->ts_hits[ui->hover_ts_hit];
  char full[96];
  if (ccord_timefmt_full(full, sizeof full, hit->ts_ms, ui->use_utc) == 0) return;
  if (hit->is_edited) snprintf(ui->tooltip_text, sizeof ui->tooltip_text, "Edited %s", full);
  else snprintf(ui->tooltip_text, sizeof ui->tooltip_text, "%s", full);

  (void)uis_tooltip(ui, r, hit->rect, ui->tooltip_text, UIS_TIP_ABOVE, 2);
}

// ---------------------------------------------------------------- messages

typedef struct msg_ctx {
  ccord_ui *ui;
  ccord_renderer *r;
  ccord_id channel;
} msg_ctx;

static float content_wrap(const ccord_ui *ui, float width)
{
  return maxf(width - M(message_text_x) - M(message_pad_x), 40.0f);
}

// The newest message gets breathing room above the composer (like the real client's scroller spacer).
static float tail_pad(const ccord_ui *ui, size_t i)
{
  return i + 1 == ui->vlist.count ? M(message_group_gap) : 0.0f;
}

static bool message_edited(const ccord_message *m)
{
  return m && (m->edited || m->edited_timestamp_ms != 0);
}

// ---- chat motion -------------------------------------------------------------------------------------------------
// Only drawing moves here: row heights, the layout and every other row's y stay as measured.

#define CHAT_SEEN_PROBE 16u

static size_t chat_seen_bucket(uint64_t key)
{
  return (size_t)((key * 0x9E3779B97F4A7C15ULL) >> 56) & (UI_CHAT_SEEN_MAX - 1u);
}

static ui_chat_seen *chat_seen_peek(ccord_ui *ui, uint64_t key)
{
  if (key == 0) return nullptr;
  size_t base = chat_seen_bucket(key);
  const size_t mask = UI_CHAT_SEEN_MAX - 1u;
  for (size_t i = 0; i < CHAT_SEEN_PROBE; i++) {
    ui_chat_seen *e = &ui->chat_seen[(base + i) & mask];
    if (e->key == key) return e;
  }
  return nullptr;
}

static ui_chat_seen *chat_seen_find(ccord_ui *ui, uint64_t key)
{
  ui_chat_seen *e = chat_seen_peek(ui, key);
  if (e) e->stamp = ui->chat_seq;
  return e;
}

// Takes a free slot in the probe window, else the least recently drawn one that was not drawn this frame, so a frame's
// own rows and pills keep their memory. Nullptr only when every probed slot was drawn this frame.
static ui_chat_seen *chat_seen_put(ccord_ui *ui, uint64_t key)
{
  if (key == 0) return nullptr;
  size_t base = chat_seen_bucket(key);
  const size_t mask = UI_CHAT_SEEN_MAX - 1u;
  ui_chat_seen *victim = nullptr;
  for (size_t i = 0; i < CHAT_SEEN_PROBE; i++) {
    ui_chat_seen *e = &ui->chat_seen[(base + i) & mask];
    if (e->key == 0) {
      victim = e;
      break;
    }
    if (e->stamp >= ui->chat_seq) continue;
    if (!victim || e->stamp < victim->stamp) victim = e;
  }
  if (!victim) return nullptr;
  memset(victim, 0, sizeof *victim);
  victim->key = key;
  victim->stamp = ui->chat_seq;
  return victim;
}

// Strength of the jump highlight: full for UI_JUMP_HOLD_MS, then an eased fade to 0 at UI_JUMP_FLASH_MS.
// Reduced motion shows it at full strength and then drops it at once.
static float jump_flash_strength(const ccord_ui *ui)
{
  if (ui->now_ms >= ui->jump_highlight_until_ms) return 0.0f;
  if (ui->anim.reduced_motion) return 1.0f;
  float left = (float)(ui->jump_highlight_until_ms - ui->now_ms);
  float elapsed = (float)UI_JUMP_FLASH_MS - left;
  if (elapsed <= (float)UI_JUMP_HOLD_MS) return 1.0f;
  float t = (elapsed - (float)UI_JUMP_HOLD_MS) / ((float)UI_JUMP_FLASH_MS - (float)UI_JUMP_HOLD_MS);
  return 1.0f - ccord_ease_out(clampf(t, 0.0f, 1.0f));
}

// A pill that appears on a row that was on screen last frame pops in, including one taken off and put back while its
// row stayed on screen; a count change on such a row ticks once. Anything else (history, a row scrolled back into view)
// is recorded silently. Pills are walked on every frame their row is drawn, so the stamps tell which frame saw what.
static void reaction_pill_motion(ccord_ui *ui, const ccord_message *m, uint64_t pid, unsigned count, float *pop,
                                 float *tick)
{
  *pop = 1.0f;
  *tick = 1.0f;
  const ui_chat_seen *me = chat_seen_peek(ui, m->id);
  const ui_chat_seen *pe = chat_seen_peek(ui, pid);
  bool shown = me && me->stamp + 1 == ui->chat_seq;
  bool pill_was_shown = pe && pe->stamp + 1 == ui->chat_seq;
  bool pop_now = false, tick_now = false;
  ui_chat_seen *e = chat_seen_find(ui, pid);
  if (!e) {
    e = chat_seen_put(ui, pid);
    if (!e) return;   // every slot was drawn this frame: this pill gets no motion
    pop_now = shown;
  } else if (!pill_was_shown && shown) {
    pop_now = true;
  } else if (e->count != count) {
    tick_now = shown;
  }
  e->count = count;
  if (pop_now) {
    e->event = UI_CHAT_EV_NEW;
    e->event_ms = ui->now_ms;
    ccord_anim_reset(&ui->anim, ccord_anim_key("reactpop", pid));
  } else if (tick_now) {
    e->event = UI_CHAT_EV_TICK;
    e->event_ms = ui->now_ms;
    ccord_anim_reset(&ui->anim, ccord_anim_key("reacttick", pid));
  }
  if (e->event == UI_CHAT_EV_NEW) {
    *pop = ui->now_ms < e->event_ms + UI_REACT_POP_MS ? ui_enter(ui, "reactpop", pid, UI_REACT_POP_MS) : 1.0f;
    ui->dbg_reaction_pop = *pop;
  } else if (e->event == UI_CHAT_EV_TICK && ui->now_ms < e->event_ms + UI_REACT_TICK_MS) {
    *tick = ui_enter(ui, "reacttick", pid, UI_REACT_TICK_MS);
  }
}

static bool message_has_reactions(const ccord_message *m)
{
  if (!m || !m->reactions || m->reaction_kind_count == 0) return false;
  for (size_t i = 0; i < m->reaction_kind_count; i++) {
    if (m->reactions[i].emoji && m->reactions[i].count > 0) return true;
  }
  return false;
}

static float reaction_badge_h(const ccord_ui *ui)
{
  (void)ui;
  return UIS_H_CHIP + 2.0f;   // the shared chip height plus a 1 px margin
}

typedef struct reaction_pill {
  const char *emoji;
  size_t elen;
  ccord_id emoji_id;
  bool animated;
  unsigned count;
  uint32_t burst_count;
  bool mine;
  bool me_burst;
} reaction_pill;

static void reaction_pill_fill(const ccord_reaction *r, reaction_pill *out)
{
  out->emoji = r->emoji;
  out->elen = strlen(r->emoji);
  out->emoji_id = r->emoji_id;
  out->animated = r->animated;
  if (out->emoji_id == CCORD_ID_NONE) {
    const char *colon = strrchr(r->emoji, ':');
    if (colon && colon > r->emoji && colon[1] != '\0') {
      ccord_id parsed = CCORD_ID_NONE;
      if (ccord_id_parse(colon + 1, strlen(colon + 1), &parsed)) {
        out->emoji_id = parsed;
      }
    }
  }
  out->count = r->count;
  out->burst_count = r->burst_count;
  out->me_burst = r->me_burst;
  out->mine = r->self_reacted || r->me_burst;
}

// One pill per emoji (emoji + count), wrapping to new rows. Returns total height; draws when asked.
static float reaction_pills_walk(ccord_ui *ui, ccord_renderer *r, const ccord_message *m, float x, float y,
                                 float wrap, bool draw)
{
  if (!message_has_reactions(m)) return 0.0f;
  float bh = reaction_badge_h(ui);
  float gap_x = 6.0f, gap_y = 4.0f;
  float cx = x, cy = y;
  bool any = false;
  for (size_t ri = 0; ri < m->reaction_kind_count; ri++) {
    const ccord_reaction *rc = &m->reactions[ri];
    if (!rc->emoji || rc->count == 0) continue;
    reaction_pill p;
    reaction_pill_fill(rc, &p);
    char label[24];
    int ll = snprintf(label, sizeof label, "%u", p.count);
    if (ll < 0) ll = 0;
    bool is_custom = (p.emoji_id != CCORD_ID_NONE);
    bool is_uemoji = !is_custom && p.elen > 0 && ccord_emoji_match_prefix(p.emoji, p.elen) == p.elen;
    ccord_text_style es = ui_style(ui, is_uemoji ? CCORD_FONT_EMOJI : CCORD_FONT_REGULAR,
                                   is_uemoji ? M(font_size_small) : (M(font_size_small) - 2.0f),
                                   p.mine ? COL(text_on_brand) : COL(text_normal));
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f,
                                    p.mine ? COL(text_on_brand) : COL(text_secondary));
    size_t custom_nlen = p.elen;
    if (is_custom) {
      const char *colon = strrchr(p.emoji, ':');
      custom_nlen = colon ? (size_t)(colon - p.emoji) : p.elen;
    }
    float ew = is_custom ? (bh - 6.0f) : ccord_text_measure(r, p.emoji, p.elen, &es, 0.0f).x;
    float cw = ccord_text_measure(r, label, (size_t)ll, &cs, 0.0f).x;
    float pw = maxf(ew + cw + 20.0f, 34.0f);
    if (any && cx > x && cx + pw > x + wrap) {
      cx = x;
      cy += bh + gap_y;
    }
    if (draw) {
      ccord_rect pr = {cx, cy, pw, bh};
      // A pill eases into "mine" when you react and out when you take it back, and warms up under the pointer.
      uint64_t pid = m->id ^ (ui_name_id(p.emoji) * 31u);
      float mf = ui_fade(ui, "reactm", pid, p.mine);
      float hf = ui_fade(ui, "reacth", pid, ui->mouse_in && ui_in_rect(ui->mx, ui->my, pr)) * (1.0f - mf);
      float pop, tick;
      reaction_pill_motion(ui, m, pid, p.count, &pop, &tick);
      if (pop < 1.0f) {
        ccord_push_alpha(r, pop);
        ccord_push_offset(r, 0.0f, (1.0f - pop) * 4.0f);
      }
      if (p.burst_count > 0) {
        ccord_color gold = (ccord_color){245, 178, 50, 235};
        ccord_draw_rrect(r, (ccord_rect){pr.x - 1.0f, pr.y - 1.0f, pr.w + 2.0f, pr.h + 2.0f}, bh * 0.5f + 1.0f, gold);
        ccord_color base_bg = ui_mix(COL(bg_tertiary), (ccord_color){70, 52, 24, 255}, p.me_burst ? 0.75f : 0.4f);
        ccord_draw_rrect(r, pr, bh * 0.5f, ui_mix(base_bg, COL(bg_item_hover), hf));
        es.color = COL(text_header);
        cs.color = gold;
        ccord_draw_circle(r, pr.x + pr.w - 4.0f, pr.y + 4.0f, 2.2f, gold);
        ui->dbg_burst_reaction_pills++;
      } else {
        uis_chip(ui, r, pr, "", (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pr), .selected = p.mine},
                 "reactchip", pid);
        es.color = ui_mix(COL(text_normal), COL(text_on_brand), mf);
        cs.color = ui_mix(COL(text_secondary), COL(text_on_brand), mf);
      }
      ccord_rect emoji_box;
      if (is_custom) {
        ccord_rect er = {cx + 6.0f, cy + 3.0f, ew, ew};
        ui_expr_draw_custom_emoji(ui, r, p.emoji_id, p.emoji, custom_nlen, p.animated, er);
        emoji_box = er;
      } else {
        ccord_vec2 et = ccord_text_measure(r, p.emoji, p.elen, &es, 0.0f);
        ccord_text_draw(r, p.emoji, p.elen, cx + 7.0f, cy + (bh - et.y) * 0.5f, &es, 0.0f);
        emoji_box = (ccord_rect){cx + 7.0f, cy + (bh - et.y) * 0.5f, et.x, et.y};
      }
      if (ui->ndbg_reaction_draws < UI_REACTION_DRAW_MAX) {
        ui->dbg_reaction_draws[ui->ndbg_reaction_draws++] =
            (ui_reaction_draw){.message_id = m->id, .pill = pr, .emoji = emoji_box, .custom = is_custom,
                               .burst = p.burst_count > 0};
      }
      if (tick < 1.0f) ccord_push_offset(r, 0.0f, -(1.0f - tick) * 3.0f);   // the count nudges up and settles
      ccord_text_draw(r, label, (size_t)ll, cx + 9.0f + ew, cy + (bh - ui_line_h(ui, cs.size)) * 0.5f, &cs, 0.0f);
      if (tick < 1.0f) ccord_pop_offset(r);
      if (pop < 1.0f) {
        ccord_pop_offset(r);
        ccord_pop_alpha(r);
      }
      if (ui->nreaction_hits < UI_REACTION_HIT_MAX && p.elen < sizeof ui->reaction_hits[0].emoji) {
        ui_reaction_hit *hit = &ui->reaction_hits[ui->nreaction_hits++];
        hit->rect = pr;
        hit->channel_id = m->channel_id;
        hit->message_id = m->id;
        memcpy(hit->emoji, p.emoji, p.elen);
        hit->emoji[p.elen] = '\0';
        hit->is_add_btn = false;
      }
    }
    cx += pw + gap_x;
    any = true;
  }
  if (any && draw && ui->nreaction_hits < UI_REACTION_HIT_MAX) {
    float add_w = 26.0f;
    if (cx + add_w <= x + wrap) {
      ccord_rect ar = {cx, cy, add_w, bh};
      uis_chip(ui, r, ar, "+", (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ar)}, "reactadd", m->id);
      ui_reaction_hit *hit = &ui->reaction_hits[ui->nreaction_hits++];
      hit->rect = ar;
      hit->channel_id = m->channel_id;
      hit->message_id = m->id;
      hit->emoji[0] = '\0';
      hit->is_add_btn = true;
    }
  }
  return any ? cy - y + bh : 0.0f;
}

// ---------------------------------------------------------------- rich cards (reply / attachment / embed / ...)

typedef enum decor_kind : uint8_t {
  DECOR_REPLY, DECOR_ATTACHMENT, DECOR_GALLERY, DECOR_MEDIA_PLAYER,
  DECOR_EMBED, DECOR_STICKER, DECOR_FORWARD, DECOR_POLL, DECOR_COMPONENTS, DECOR_COMPONENTS_V2, DECOR_THREAD,
  DECOR_INVITE,
} decor_kind;
#define DECOR_MAX_PARTS 24u

// Reply reference (one compact line above the message header) and composer reply bar metrics, in logical px.
#define UI_REPLY_LINE_H 22.0f      // the whole reference line; a card would be taller than this
#define UI_REPLY_LINE_GAP 2.0f     // space between the line and the message header
#define UI_REPLY_AVATAR 16.0f      // replied-to author's round avatar
#define UI_REPLY_SPINE_W 2.0f      // connector thickness
#define UI_REPLY_SPINE_R 6.0f      // radius of the connector's turn
#define UI_REPLY_SPINE_GAP 4.0f    // connector end to the avatar
#define UI_REPLY_BAR_H 28.0f       // composer reply bar; ui.c hit-tests this same height
#define UI_REPLY_BAR_PAD 12.0f
#define UI_REPLY_BTN_H 20.0f       // toggle and close buttons
#define UI_REPLY_CLOSE_W 20.0f
#define UI_REPLY_PING_W 26.0f

// One card under a message's text. The pointers point into the stored message's rich block and are only used while that
// message is being measured or drawn.
typedef struct decor_part {
  decor_kind kind;
  const ccord_attachment *att;
  const ccord_embed *emb;
  const ccord_msg_sticker *stk;
  const ccord_poll *poll;
  const ccord_message_rich *rich;
  const char *text;   // forward: the forwarded text
  size_t len;
  size_t count;       // gallery / multi-image embed item count
  size_t row_idx;     // component row index
  ccord_id msg_id;
  ccord_id author_id;
  char invite_code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
  uint32_t edit_rev;
} decor_part;

typedef struct decor_info {
  char body[CCORD_MESSAGE_MAX_BYTES + 1];
  size_t body_len;
  decor_part parts[DECOR_MAX_PARTS];
  size_t nparts;
  ccord_id channel_id;
  ccord_id msg_id;
  ccord_id author_id;
  uint32_t edit_rev;
  ccord_id reply_target_id;
  bool reply_deleted;
  bool reply_attach_only;   // the original has no text, only an attachment
  ccord_id reply_author_id;
  char reply_name[CCORD_NAME_MAX_BYTES + 8];
  char reply_text[256];
} decor_info;

#define DECOR_POLL_ROW_H 24.0f

static float gallery_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);
static float embed_card_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);
static float components_row_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);
static float cv2_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw);

// The size of a video's picture box: its own shape, as large as fits max_w x max_h, never narrower than the
// controls need. Unknown dimensions are taken as 16:9.
static void video_box_size(uint32_t vw, uint32_t vh, float max_w, float max_h, float *bw, float *bh)
{
  float aw = vw > 0 && vh > 0 ? (float)vw : 16.0f, ah = vw > 0 && vh > 0 ? (float)vh : 9.0f;
  float sc = fminf(max_w / aw, max_h / ah);
  float w = floorf(aw * sc), h = floorf(ah * sc);
  float min_w = fminf(200.0f, max_w);
  *bw = w < min_w ? min_w : w;
  *bh = h < 90.0f ? 90.0f : h;
}

#define MEDIA_VIDEO_MAX_H 320.0f
#define MEDIA_CONTROLS_H 56.0f

static float media_player_w(const ccord_attachment *a, float w)
{
  float cw = clampf(w, 140.0f, 460.0f);
  if (!a || !ccord_attachment_is_video(a)) return cw;
  float bw, bh;
  video_box_size(a->width, a->height, cw - 12.0f, MEDIA_VIDEO_MAX_H, &bw, &bh);
  return bw + 12.0f;
}

static float media_player_h(const ccord_attachment *a, float w)
{
  if (!a || !ccord_attachment_is_video(a)) return 58.0f;
  float bw, bh;
  video_box_size(a->width, a->height, clampf(w, 140.0f, 460.0f) - 12.0f, MEDIA_VIDEO_MAX_H, &bw, &bh);
  return 6.0f + bh + 2.0f + MEDIA_CONTROLS_H;
}

static float decor_part_h(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float w)
{
  switch (p->kind) {
    case DECOR_REPLY: return 0.0f;   // drawn as a line above the header, see decor_reply_h
    case DECOR_ATTACHMENT: return 46.0f;
    case DECOR_GALLERY: return gallery_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_MEDIA_PLAYER: return media_player_h(p->att, w);
    case DECOR_EMBED: return embed_card_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_STICKER: return 164.0f;
    case DECOR_FORWARD: return 46.0f;
    case DECOR_POLL: return 52.0f + (p->poll ? (float)p->poll->answer_count : 0.0f) * DECOR_POLL_ROW_H;
    case DECOR_COMPONENTS: return components_row_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_COMPONENTS_V2: return cv2_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_THREAD: return 42.0f;
    case DECOR_INVITE: return 88.0f;
  }
  return 0.0f;
}

static void decor_add(decor_info *d, decor_kind kind, const ccord_attachment *att, const ccord_embed *emb,
                      const ccord_msg_sticker *stk, const ccord_poll *poll, const char *text, size_t len, size_t count)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){kind, att, emb, stk, poll, nullptr, text, len, count, 0u, d->msg_id, d->author_id, "", d->edit_rev};
}

static void decor_add_comp(decor_info *d, const ccord_message_rich *rich, size_t row_idx)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){DECOR_COMPONENTS, nullptr, nullptr, nullptr, nullptr, rich,
                                       nullptr, 0u, 1u, row_idx, d->msg_id, d->author_id, "", d->edit_rev};
}

// Components v2: the whole tree is one part (the message's attachments stay reachable for attachment:// media).
static void decor_add_v2(decor_info *d, const ccord_message_rich *rich)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){DECOR_COMPONENTS_V2, nullptr, nullptr, nullptr, nullptr, rich,
                                       nullptr, 0u, 1u, 0u, d->msg_id, d->author_id, "", d->edit_rev};
}

static void decor_add_thread(decor_info *d, const ccord_message_rich *rich)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){DECOR_THREAD, nullptr, nullptr, nullptr, nullptr, rich,
                                       nullptr, 0u, 1u, 0u, d->msg_id, d->author_id, "", d->edit_rev};
}

static bool inv_ieq_at_n(const char *s, size_t rem, const char *lit, size_t n)
{
  if (rem < n) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char a = (unsigned char)s[i];
    unsigned char b = (unsigned char)lit[i];
    if (a >= 'A' && a <= 'Z') a += (unsigned char)('a' - 'A');
    if (a != b) return false;   // table literals are already lowercase
  }
  return true;
}

static void decor_scan_invites(decor_info *d, const char *text, size_t len)
{
  if (!d || !text || len == 0) return;
  typedef struct inv_prefix {
    const char *str;
    size_t len;
  } inv_prefix;
#define INV_PFX(s) {s, sizeof(s) - 1u}
  static const inv_prefix k_prefixes[] = {
    INV_PFX("https://www.discord.gg/"),         INV_PFX("http://www.discord.gg/"),         INV_PFX("www.discord.gg/"),
    INV_PFX("https://discord.gg/"),             INV_PFX("http://discord.gg/"),             INV_PFX("discord.gg/"),
    INV_PFX("https://www.discord.com/invite/"), INV_PFX("http://www.discord.com/invite/"), INV_PFX("www.discord.com/invite/"),
    INV_PFX("https://discord.com/invite/"),     INV_PFX("http://discord.com/invite/"),     INV_PFX("discord.com/invite/"),
    INV_PFX("https://ptb.discord.com/invite/"), INV_PFX("http://ptb.discord.com/invite/"), INV_PFX("ptb.discord.com/invite/"),
    INV_PFX("https://canary.discord.com/invite/"), INV_PFX("http://canary.discord.com/invite/"), INV_PFX("canary.discord.com/invite/"),
    INV_PFX("https://discordapp.com/invite/"),  INV_PFX("http://discordapp.com/invite/"),  INV_PFX("discordapp.com/invite/"),
    INV_PFX("discord://-/invite/"),             INV_PFX("discord://invite/"),
  };
#undef INV_PFX
  size_t found = 0;
  for (size_t i = 0; i < len && found < 3u && d->nparts < DECOR_MAX_PARTS; i++) {
    unsigned char c0 = (unsigned char)text[i];
    if (c0 >= 'A' && c0 <= 'Z') c0 += (unsigned char)('a' - 'A');
    if (c0 != 'h' && c0 != 'w' && c0 != 'd' && c0 != 'p' && c0 != 'c') continue;
    if (i > 0) {
      unsigned char prev = (unsigned char)text[i - 1u];
      bool word_prev = (prev >= '0' && prev <= '9') || (prev >= 'a' && prev <= 'z') ||
                       (prev >= 'A' && prev <= 'Z') || prev == '.' || prev == '_' || prev == '/';
      if (word_prev) continue;
    }
    size_t plen = 0;
    for (size_t pi = 0; pi < sizeof k_prefixes / sizeof k_prefixes[0]; pi++) {
      if ((unsigned char)k_prefixes[pi].str[0] != c0) continue;
      if (inv_ieq_at_n(text + i, len - i, k_prefixes[pi].str, k_prefixes[pi].len)) {
        plen = k_prefixes[pi].len;
        break;
      }
    }
    if (plen == 0) continue;
    size_t cs = i + plen;
    size_t ce = cs;
    while (ce < len && ce - cs <= CCORD_ADMIN_INVITE_CODE_MAX) {
      char c = text[ce];
      bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
      if (!ok) break;
      ce++;
    }
    size_t clen = ce - cs;
    if (clen >= 2 && clen <= CCORD_ADMIN_INVITE_CODE_MAX) {
      char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
      memcpy(code, text + cs, clen);
      code[clen] = '\0';
      bool dup = false;
      for (size_t k = 0; k < d->nparts; k++) {
        if (d->parts[k].kind == DECOR_INVITE && strcmp(d->parts[k].invite_code, code) == 0) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        decor_part part = {.kind = DECOR_INVITE, .count = 1u, .msg_id = d->msg_id, .author_id = d->author_id};
        memcpy(part.invite_code, code, clen + 1u);
        d->parts[d->nparts++] = part;
        found++;
      }
    }
    i = ce;
  }
}

static bool embed_is_extra_gallery_image(const ccord_embed *head, const ccord_embed *e)
{
  if (!head || !e || !head->url || !head->url[0] || !e->url || strcmp(head->url, e->url) != 0) return false;
  if (!e->image.url || !e->image.url[0]) return false;
  if ((e->title && e->title[0]) || (e->description && e->description[0]) || e->field_count > 0) return false;
  if ((e->footer_text && e->footer_text[0]) || e->timestamp_ms > 0) return false;
  return true;
}

static bool url_host_in_list(const char *url, const char *const *hosts, size_t nhosts)
{
  if (!url || !url[0]) return false;
  const char *p = nullptr;
  if (strncasecmp(url, "https://", 8) == 0) p = url + 8;
  else if (strncasecmp(url, "http://", 7) == 0) p = url + 7;
  else return false;
  const char *end = p;
  while (*end && *end != '/' && *end != '?' && *end != '#') end++;
  const char *colon = memchr(p, ':', (size_t)(end - p));
  if (colon) end = colon;
  size_t hlen = (size_t)(end - p);
  if (hlen == 0) return false;
  for (size_t i = 0; i < nhosts; i++) {
    if (strlen(hosts[i]) == hlen && strncasecmp(p, hosts[i], hlen) == 0) return true;
  }
  return false;
}

// Formats that play inline as looping animated images (and must not be flattened to a WebP still by the proxy).
static bool url_is_anim_image(const char *u)
{
  if (!u || !u[0]) return false;
  return ui_url_path_ends(u, ".gif") || ui_url_path_ends(u, ".webp") || ui_url_path_ends(u, ".apng") ||
         ui_url_path_ends(u, ".gifv");
}

static bool embed_is_gif_provider(const ccord_embed *e)
{
  if (!e) return false;
  if (e->provider && e->provider[0]) {
    static const char *const k_provs[] = {"Tenor", "GIPHY", "Klipy", "Gfycat", "RedGIFs"};
    for (size_t i = 0; i < sizeof k_provs / sizeof k_provs[0]; i++) {
      if (strcasecmp(e->provider, k_provs[i]) == 0) return true;
    }
  }
  static const char *const k_gif_hosts[] = {
    "tenor.com", "www.tenor.com", "c.tenor.com", "media.tenor.com", "media1.tenor.com", "media2.tenor.com",
    "media.tenor.co", "giphy.com", "www.giphy.com", "media.giphy.com", "media0.giphy.com", "media1.giphy.com",
    "media2.giphy.com", "media3.giphy.com", "media4.giphy.com", "i.giphy.com", "gph.is",
    "klipy.com", "www.klipy.com", "static.klipy.com", "media.klipy.com",
  };
  return url_host_in_list(e->url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]) ||
         url_host_in_list(e->video.url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]) ||
         url_host_in_list(e->image.url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]) ||
         url_host_in_list(e->thumbnail.url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]);
}

// Rich text chrome other than `provider`: Tenor/GIPHY/Klipy/Imgur GIFV and image embeds routinely carry a provider
// name ("Tenor", "GIPHY", "Klipy") while still rendering framelessly in Discord.
static bool embed_has_rich_chrome(const ccord_embed *e)
{
  if (!e) return false;
  return (e->title && e->title[0]) || (e->description && e->description[0]) || (e->author_name && e->author_name[0]) ||
         (e->footer_text && e->footer_text[0]) || e->timestamp_ms > 0 || e->field_count > 0;
}

static bool embed_has_visual_media(const ccord_embed *e)
{
  if (!e) return false;
  return (e->video.proxy_url && e->video.proxy_url[0]) || (e->video.url && e->video.url[0]) ||
         (e->image.proxy_url && e->image.proxy_url[0]) || (e->image.url && e->image.url[0]) ||
         (e->thumbnail.proxy_url && e->thumbnail.proxy_url[0]) || (e->thumbnail.url && e->thumbnail.url[0]);
}

// True for GIFs and looping GIF-like embeds (Tenor/GIPHY/Klipy/Imgur gifv, direct .gif/.webp/.apng embeds, or
// GIF-provider video embeds) that should play framelessly without video-player chrome.
static bool embed_is_gif_like(const ccord_embed *e)
{
  if (!e || !embed_has_visual_media(e)) return false;
  if (e->kind == CCORD_EMBED_GIFV) return true;
  if (embed_has_rich_chrome(e)) return false;
  if (e->kind == CCORD_EMBED_IMAGE) {
    return url_is_anim_image(e->url) || url_is_anim_image(e->image.url) || url_is_anim_image(e->image.proxy_url) ||
           url_is_anim_image(e->thumbnail.url) || url_is_anim_image(e->thumbnail.proxy_url) || embed_is_gif_provider(e);
  }
  if (e->kind == CCORD_EMBED_VIDEO) {
    return embed_is_gif_provider(e) || url_is_anim_image(e->url) || url_is_anim_image(e->video.url) ||
           url_is_anim_image(e->video.proxy_url) || url_is_anim_image(e->thumbnail.url) || url_is_anim_image(e->image.url);
  }
  return false;
}

// Pure frameless media embed (GIF/GIFV/animated image or bare image with no rich text frame).
static bool embed_is_bare_media(const ccord_embed *e)
{
  if (!e || !embed_has_visual_media(e)) return false;
  if (embed_is_gif_like(e)) return true;
  return e->kind == CCORD_EMBED_IMAGE && !embed_has_rich_chrome(e);
}

static bool url_slice_matches_loose(const char *s, size_t slen, const char *cand)
{
  if (!s || slen == 0 || !cand || !cand[0]) return false;
  const char *sp = s;
  size_t sn = slen;
  if (sn >= 8 && strncasecmp(sp, "https://", 8) == 0) { sp += 8; sn -= 8; }
  else if (sn >= 7 && strncasecmp(sp, "http://", 7) == 0) { sp += 7; sn -= 7; }
  else return false;
  const char *cp = cand;
  size_t cn = strlen(cp);
  if (cn >= 8 && strncasecmp(cp, "https://", 8) == 0) { cp += 8; cn -= 8; }
  else if (cn >= 7 && strncasecmp(cp, "http://", 7) == 0) { cp += 7; cn -= 7; }
  else return false;
  if (sn > 0 && sp[sn - 1] == '/') sn--;
  if (cn > 0 && cp[cn - 1] == '/') cn--;
  if (sn != cn || sn == 0) return false;
  // Scheme-stripped host is case-insensitive; path/query are exact.
  const char *s_slash = memchr(sp, '/', sn);
  const char *c_slash = memchr(cp, '/', cn);
  size_t s_hlen = s_slash ? (size_t)(s_slash - sp) : sn;
  size_t c_hlen = c_slash ? (size_t)(c_slash - cp) : cn;
  if (s_hlen != c_hlen) return false;
  if (strncasecmp(sp, cp, s_hlen) != 0) return false;
  return memcmp(sp + s_hlen, cp + c_hlen, sn - s_hlen) == 0;
}

// Hides the raw link text when a message consists solely of a single URL whose embed renders as a frameless GIF/media
// item (like Discord does for Tenor/GIPHY/Klipy/direct-GIF posts with no accompanying text).
static bool message_should_hide_bare_link(const ccord_ui *ui, const ccord_message *m)
{
  if (!ui || !m || !m->content || !m->content[0] || !m->rich) return false;
  if (!ui->settings_embeds) return false;
  const ccord_message_rich *rich = m->rich;
  if ((rich->flags & (CCORD_MSGFLAG_SUPPRESS_EMBEDS | CCORD_MSGFLAG_IS_COMPONENTS_V2)) != 0) return false;
  if (ccord_message_type_is_system(rich->type) || rich->forward.present) return false;
  if (rich->embed_count != 1u) return false;

  const char *s = m->content;
  size_t slen = strlen(s);
  while (slen > 0 && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) { s++; slen--; }
  while (slen > 0 && (s[slen - 1] == ' ' || s[slen - 1] == '\t' || s[slen - 1] == '\r' || s[slen - 1] == '\n')) slen--;
  if (slen <= 8u) return false;
  if (strncasecmp(s, "https://", 8) != 0 && strncasecmp(s, "http://", 7) != 0) return false;
  for (size_t i = 0; i < slen; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c <= 0x20u || c == 0x7fu) return false;   // extra words / whitespace -> keep message text
  }

  const ccord_embed *e = &rich->embeds[0];
  if (embed_is_gif_like(e)) return true;
  if (embed_is_bare_media(e)) {
    return url_slice_matches_loose(s, slen, e->url) || url_slice_matches_loose(s, slen, e->image.url) ||
           url_slice_matches_loose(s, slen, e->thumbnail.url);
  }
  return false;
}

static bool decor_has_body_cards(const decor_info *d)
{
  if (!d) return false;
  for (size_t i = 0; i < d->nparts; i++) {
    if (d->parts[i].kind != DECOR_REPLY) return true;
  }
  return false;
}

static void decor_add_media(const ccord_ui *ui, decor_info *d, const ccord_attachment *atts, size_t natt,
                            const ccord_embed *embs, size_t nemb, const ccord_msg_sticker *stks, size_t nstk)
{
  if (ui->settings_attachments) {
    size_t i = 0;
    while (i < natt) {
      if (ccord_attachment_is_image(&atts[i])) {
        size_t k = 1u;
        while (i + k < natt && k < CCORD_RICH_MAX_ATTACHMENTS && ccord_attachment_is_image(&atts[i + k])) k++;
        decor_add(d, DECOR_GALLERY, &atts[i], nullptr, nullptr, nullptr, nullptr, 0, k);
        i += k;
      } else if (ccord_attachment_is_video(&atts[i]) || ccord_attachment_is_audio(&atts[i])) {
        decor_add(d, DECOR_MEDIA_PLAYER, &atts[i], nullptr, nullptr, nullptr, nullptr, 0, 1u);
        i++;
      } else {
        decor_add(d, DECOR_ATTACHMENT, &atts[i], nullptr, nullptr, nullptr, nullptr, 0, 1u);
        i++;
      }
    }
  }
  if (ui->settings_embeds) {
    size_t i = 0;
    while (i < nemb) {
      size_t k = 1u;
      if (embs[i].url && embs[i].url[0] && embs[i].image.url && embs[i].image.url[0]) {
        while (i + k < nemb && k < CCORD_RICH_MAX_ATTACHMENTS && embed_is_extra_gallery_image(&embs[i], &embs[i + k])) {
          k++;
        }
      }
      decor_add(d, DECOR_EMBED, nullptr, &embs[i], nullptr, nullptr, nullptr, 0, k);
      i += k;
    }
  }
  for (size_t i = 0; i < nstk; i++) decor_add(d, DECOR_STICKER, nullptr, nullptr, &stks[i], nullptr, nullptr, 0, 1u);
}

static void body_put(decor_info *d, size_t *out, const char *s, size_t n)
{
  if (*out + n > sizeof d->body - 1) n = sizeof d->body - 1 - *out;
  memcpy(d->body + *out, s, n);
  *out += n;
}

// Sets the reply line from what is stored, or failing that from what Discord attached to the reply.
static bool decor_reply(const ccord_ui *ui, const ccord_message *m, decor_info *d)
{
  const ccord_message_rich *rich = m->rich;
  ccord_id target = m->reply_to_id != CCORD_ID_NONE ? m->reply_to_id
                    : (rich ? rich->reference.message_id : CCORD_ID_NONE);
  d->reply_target_id = target;
  ccord_id guild = ccord_store_selected_guild(ui->d.store);
  if (target != CCORD_ID_NONE) {
    const ccord_message *r = ccord_store_find_message(ui->d.store, m->channel_id, target);
    if (r) {
      const ccord_user *u = ccord_store_user(ui->d.store, r->author_id);
      const ccord_member *gm = (guild != CCORD_ID_NONE && guild != CCORD_DM_GUILD_ID)
                                   ? ccord_store_find_member(ui->d.store, guild, r->author_id) : nullptr;
      snprintf(d->reply_name, sizeof d->reply_name, "%s", (gm && gm->nick[0] != '\0') ? gm->nick : (u ? u->name : "Unknown User"));
      d->reply_author_id = r->author_id;
      d->reply_attach_only = (!r->content || r->content[0] == '\0') && r->rich && r->rich->attachment_count > 0;
      (void)ccord_message_preview(r, d->reply_text, sizeof d->reply_text);
      return true;
    }
  }
  if (!rich) return false;
  if (rich->ref_preview.present) {
    const ccord_user *u = rich->ref_preview.author_id != CCORD_ID_NONE
                              ? ccord_store_user(ui->d.store, rich->ref_preview.author_id) : nullptr;
    const char *name = (rich->ref_preview.author_name && rich->ref_preview.author_name[0]) ? rich->ref_preview.author_name
                       : (u ? u->name : "Unknown User");
    snprintf(d->reply_name, sizeof d->reply_name, "%s", name);
    d->reply_author_id = rich->ref_preview.author_id;
    if (rich->ref_preview.content && rich->ref_preview.content[0]) {
      size_t tl = utf8_boundary(rich->ref_preview.content, strlen(rich->ref_preview.content), sizeof d->reply_text);
      memcpy(d->reply_text, rich->ref_preview.content, tl);
      d->reply_text[tl] = '\0';
    } else {
      d->reply_attach_only = rich->ref_preview.has_attachments;
      snprintf(d->reply_text, sizeof d->reply_text, "%s",
               rich->ref_preview.has_attachments ? "Click to see attachment" : "");
    }
    return true;
  }
  if (rich->ref_preview.deleted) {
    d->reply_deleted = true;
    d->reply_target_id = CCORD_ID_NONE;
    d->reply_name[0] = '\0';
    snprintf(d->reply_text, sizeof d->reply_text, "Original message was deleted");
    return true;
  }
  return false;
}

// What goes above/below a message: its text, a system message's wording, and the cards for everything it carries.
static void decor_scan(const ccord_ui *ui, const ccord_message *m, bool compact, decor_info *d)
{
  d->body_len = 0;
  d->nparts = 0;
  d->channel_id = m->channel_id;
  d->msg_id = m->id;
  d->author_id = m->author_id;
  d->edit_rev = m->edit_rev;
  d->reply_target_id = CCORD_ID_NONE;
  d->reply_deleted = false;
  d->reply_attach_only = false;
  d->reply_author_id = CCORD_ID_NONE;
  d->reply_name[0] = d->reply_text[0] = '\0';
  const ccord_message_rich *rich = m->rich;
  if (decor_reply(ui, m, d)) decor_add(d, DECOR_REPLY, nullptr, nullptr, nullptr, nullptr, d->reply_text, strlen(d->reply_text), 1u);

  const char *content = m->content ? m->content : "";
  size_t out = 0;
  if (rich && ccord_message_type_is_system(rich->type)) {
    // "<name> joined the server." in place of text. Types that carry text (a new channel name, a thread title) append it.
    const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
    const char *name = u ? u->name : "Someone";
    const char *phrase = ccord_message_type_phrase(rich->type);
    if (phrase || content[0] == '\0') {
      body_put(d, &out, name, strlen(name));
      body_put(d, &out, " ", 1);
      const char *words = phrase ? phrase : "sent a system message.";
      body_put(d, &out, words, strlen(words));
      if (phrase && content[0] != '\0' && (rich->type == 4 || rich->type == 18)) {
        body_put(d, &out, " ", 1);
        body_put(d, &out, content, strlen(content));
      }
    } else {
      body_put(d, &out, content, strlen(content));
    }
  } else if (!message_should_hide_bare_link(ui, m)) {
    body_put(d, &out, content, strlen(content));
  }
  if (rich) {
    // A Components v2 message carries its whole body in the component tree: no text, embeds, stickers or poll of its
    // own, and its attachments only feed attachment:// media inside the tree (no cards of their own).
    bool v2 = (rich->flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0;
    if (rich->forward.present) {
      decor_add(d, DECOR_FORWARD, nullptr, nullptr, nullptr, nullptr, rich->forward.content,
                rich->forward.content ? strlen(rich->forward.content) : 0u, 1u);
      decor_add_media(ui, d, rich->forward.attachments, rich->forward.attachment_count, rich->forward.embeds,
                      rich->forward.embed_count, rich->forward.stickers, rich->forward.sticker_count);
    }
    if (!v2) {
      size_t nemb = (rich->flags & CCORD_MSGFLAG_SUPPRESS_EMBEDS) ? 0u : rich->embed_count;
      decor_add_media(ui, d, rich->attachments, rich->attachment_count, rich->embeds, nemb, rich->stickers,
                      rich->sticker_count);
      if (rich->poll.present) decor_add(d, DECOR_POLL, nullptr, nullptr, nullptr, &rich->poll, nullptr, 0, 1u);
    }
    if (rich->thread.id != CCORD_ID_NONE) decor_add_thread(d, rich);
    if (v2) {
      if (rich->v2_component_count > 0) decor_add_v2(d, rich);
    } else {
      for (size_t ri = 0; ri < rich->component_row_count; ri++) {
        if (rich->components[ri].component_count > 0) decor_add_comp(d, rich, ri);
      }
    }
  }
  decor_scan_invites(d, content, strlen(content));
  if (compact && message_edited(m)) {
    const char *tag = " (edited)";
    size_t tl = strlen(tag);
    if (out + tl < sizeof d->body) {
      memcpy(d->body + out, tag, tl);
      out += tl;
    }
  }
  if (compact && m->tts) {   // compact rows have no header: the TTS marker rides at the end of the body
    const char *tag = " (tts)";
    size_t tl = strlen(tag);
    if (out + tl < sizeof d->body) {
      memcpy(d->body + out, tag, tl);
      out += tl;
    }
  }
  d->body[out] = '\0';
  d->body_len = out;
}

size_t ui_message_display_body(const ccord_ui *ui, const ccord_message *m, char *out, size_t cap)
{
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!m) return 0;
  decor_info d;
  decor_scan(ui, m, false, &d);
  size_t len = d.body_len;
  if (len >= cap) len = utf8_boundary(d.body, len, cap);
  memcpy(out, d.body, len);
  out[len] = '\0';
  return len;
}

size_t ui_message_clean_body(const ccord_ui *ui, const ccord_message *m, char *out, size_t cap)
{
  (void)ui;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!m || !m->content) return 0;
  size_t len = strlen(m->content);
  if (len >= cap) len = utf8_boundary(m->content, len, cap);
  memcpy(out, m->content, len);
  out[len] = '\0';
  return len;
}

static bool message_mentions_self(const ccord_ui *ui, const ccord_message *m)
{
  if (!ui || !ui->d.store || !m) return false;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  if (self_id == CCORD_ID_NONE) return false;
  if (m->reply_to_id != CCORD_ID_NONE) {
    const ccord_message *rm = ccord_store_find_message(ui->d.store, m->channel_id, m->reply_to_id);
    if (rm && rm->author_id == self_id) return true;
  }
  if (m->rich && (m->rich->mention_everyone || ccord_rich_mentions_user(m->rich, self_id))) return true;   // incl. reply pings
  if (!m->content || !m->content[0]) return false;
  if (strstr(m->content, "@everyone") || strstr(m->content, "@here")) return true;
  {
    const char *p = m->content;
    while ((p = strstr(p, "@you")) != nullptr) {
      bool ok_left = (p == m->content || (!((unsigned char)p[-1] >= 'a' && (unsigned char)p[-1] <= 'z') &&
                                          !((unsigned char)p[-1] >= 'A' && (unsigned char)p[-1] <= 'Z') &&
                                          !((unsigned char)p[-1] >= '0' && (unsigned char)p[-1] <= '9')));
      bool ok_right = (!((unsigned char)p[4] >= 'a' && (unsigned char)p[4] <= 'z') &&
                       !((unsigned char)p[4] >= 'A' && (unsigned char)p[4] <= 'Z') &&
                       !((unsigned char)p[4] >= '0' && (unsigned char)p[4] <= '9'));
      if (ok_left && ok_right) return true;
      p += 4;
    }
  }
  char tag1[32], tag2[32];
  snprintf(tag1, sizeof tag1, "<@%llu>", (unsigned long long)self_id);
  snprintf(tag2, sizeof tag2, "<@!%llu>", (unsigned long long)self_id);
  if (strstr(m->content, tag1) || strstr(m->content, tag2)) return true;
  const ccord_user *su = ccord_store_user(ui->d.store, self_id);
  if (su && su->name && su->name[0]) {
    char at_name[96];
    snprintf(at_name, sizeof at_name, "@%s", su->name);
    if (strstr(m->content, at_name)) return true;
  }
  return false;
}

static float decor_total_h(ccord_ui *ui, ccord_renderer *r, const decor_info *d, float w)
{
  float h = 0.0f;
  for (size_t i = 0; i < d->nparts; i++) {
    if (d->parts[i].kind == DECOR_REPLY) continue;   // the reply line sits above the header, not in the card stack
    h += decor_part_h(ui, r, &d->parts[i], w) + 4.0f;
  }
  return h;
}

// Height the reply line adds above the message header (0 when the message is not a reply).
static float decor_reply_h(const decor_info *d)
{
  for (size_t i = 0; i < d->nparts; i++) {
    if (d->parts[i].kind == DECOR_REPLY) return UI_REPLY_LINE_H + UI_REPLY_LINE_GAP;
  }
  return 0.0f;
}

static const ccord_message *view_message_at(const ccord_ui *ui, ccord_id channel, size_t view_i, size_t *out_store_i)
{
  size_t raw = (size_t)ui_view_index(ui, view_i);
  if ((raw & (1ULL << 62)) != 0) {
    size_t si = raw & ~(1ULL << 62);
    if (out_store_i) *out_store_i = SIZE_MAX;
    size_t sc = 0;
    const ccord_message *sr = ui->d.session ? ccord_session_search_results(ui->d.session, &sc) : nullptr;
    return (sr && si < sc) ? &sr[si] : nullptr;
  }
  if (out_store_i) *out_store_i = raw;
  return ccord_store_message_at(ui->d.store, channel, raw);
}

#define UI_DM_INTRO_H 184.0f
#define UI_CHAT_DIVIDER_H 24.0f

static void draw_chat_divider(ccord_ui *ui, ccord_renderer *r, float x, float y, float w,
                              int64_t ts_ms, bool has_date, bool has_new)
{
  float cy = y + UI_CHAT_DIVIDER_H * 0.5f + 2.0f;
  ccord_color line_c = has_new ? COL(danger) : COL(border_subtle);
  ccord_draw_rect(r, (ccord_rect){x, cy, w, 1.0f}, line_c);
  if (has_new) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
    ccord_vec2 nt = ccord_text_measure(r, "NEW", 3, &ns, 0.0f);
    ccord_rect pill = {x + w - (nt.x + 10.0f), cy - 7.5f, nt.x + 10.0f, 15.0f};
    ccord_draw_rrect(r, pill, 3.0f, COL(danger));
    ccord_text_draw(r, "NEW", 3, pill.x + 5.0f, pill.y + (pill.h - nt.y) * 0.5f, &ns, 0.0f);
  }
  if (has_date && ts_ms > 0) {
    char dbuf[48];
    size_t dl = ccord_timefmt_date(dbuf, sizeof dbuf, ts_ms, ui->use_utc);
    if (dl > 0) {
      ccord_text_style ds = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                     has_new ? COL(danger) : COL(text_muted));
      ccord_vec2 dt = ccord_text_measure(r, dbuf, dl, &ds, 0.0f);
      ccord_rect bg = {x + (w - (dt.x + 16.0f)) * 0.5f, cy - (dt.y + 4.0f) * 0.5f, dt.x + 16.0f, dt.y + 4.0f};
      ccord_draw_rect(r, bg, COL(bg_primary));
      ccord_text_draw(r, dbuf, dl, bg.x + 8.0f, bg.y + 2.0f, &ds, 0.0f);
    }
  }
}

static bool is_dm_intro_row(const ccord_ui *ui, ccord_id channel_id, size_t store_i)
{
  if (store_i != 0) return false;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, channel_id);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID) return false;
  if (ui->d.session && ccord_session_history_has_more(ui->d.session, channel_id)) return false;
  return true;
}

static void draw_dm_intro(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch, float x, float y, float w)
{
  if (!ch) return;
  const ccord_user *u = ch->user_id != CCORD_ID_NONE ? ccord_store_user(ui->d.store, ch->user_id) : nullptr;
  const char *dname = (u && u->display_name && u->display_name[0]) ? u->display_name : ch->name;
  const char *uname = (u && u->handle && u->handle[0]) ? u->handle
                      : ((u && u->name && u->name[0]) ? u->name : ch->name);
  ccord_user dm_u = {
    .id = u ? u->id : (ch->user_id != CCORD_ID_NONE ? ch->user_id : ch->id),
    .name = dname,
    .avatar_url = (u && u->avatar_url && u->avatar_url[0]) ? u->avatar_url : (ch->icon_url ? ch->icon_url : ""),
    .presence = u ? u->presence : CCORD_PRESENCE_OFFLINE,
    .avatar_palette = u ? u->avatar_palette : (uint8_t)(ch->id % 6),
  };
  ui_draw_avatar(ui, r, &dm_u, x + 40.0f, y + 48.0f, 80.0f, (ccord_color){0, 0, 0, 0}, false);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 10.0f, COL(text_header));
  float dname_w = ccord_text_draw_ellipsis(r, dname, strlen(dname), x, y + 96.0f, w, &ts);
  ccord_id dm_uid = u ? u->id : ch->user_id;
  if (!ui_channel_is_group_dm(ch) && dm_uid != CCORD_ID_NONE) {
    record_user_hit(ui, (ccord_rect){x, y + 8.0f, 80.0f, 80.0f}, dm_uid);
    record_user_hit(ui, (ccord_rect){x, y + 96.0f, maxf(dname_w, 4.0f), ui_line_h(ui, ts.size)}, dm_uid);
  }
  char handle[160];
  snprintf(handle, sizeof handle, "@%s", uname);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_secondary));
  ccord_text_draw_ellipsis(r, handle, strlen(handle), x, y + 124.0f, w, &hs);
  char sub[256];
  snprintf(sub, sizeof sub, "This is the beginning of your direct message history with @%s.", uname);
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), x, y + 148.0f, w, &ss);
  ccord_draw_rect(r, (ccord_rect){x, y + UI_DM_INTRO_H - 8.0f, w, 1.0f}, COL(border_subtle));
}

#define UI_EPHEMERAL_FOOTER_H 20.0f
#define UI_SEND_STATUS_H 18.0f

// M0-3 local echoes: a pending row is dimmed and carries a clock/status line, a failed row is tinted and
// exposes a Retry button. Both are visibly unconfirmed; confirmed messages draw exactly as before.
static bool message_send_unconfirmed(const ccord_message *m)
{
  return m && m->send_state != CCORD_MSG_SEND_CONFIRMED;
}

static float send_status_extra_h(const ccord_ui *ui, const ccord_message *m)
{
  return message_send_unconfirmed(m) ? M(message_line_gap) * 0.5f + UI_SEND_STATUS_H : 0.0f;
}

static void draw_send_status_clock(ccord_renderer *r, float cx, float cy, float rad, ccord_color c, ccord_color bg)
{
  ccord_draw_circle(r, cx, cy, rad, c);
  ccord_draw_circle(r, cx, cy, rad * 0.74f, bg);
  ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){cx, cy - rad * 0.48f}, 1.4f, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){cx + rad * 0.42f, cy + rad * 0.15f}, 1.4f, c);
}

static void draw_send_status(ccord_ui *ui, ccord_renderer *r, const ccord_message *m, float x, float y)
{
  if (!message_send_unconfirmed(m)) return;
  float size = M(font_size_small) - 2.0f;
  float ty = y + (UI_SEND_STATUS_H - ui_line_h(ui, size)) * 0.5f;
  if (m->send_state == CCORD_MSG_SEND_PENDING) {
    draw_send_status_clock(r, x + 6.0f, y + UI_SEND_STATUS_H * 0.5f, 5.5f, COL(text_muted), COL(bg_primary));
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, size, COL(text_muted));
    ccord_text_draw(r, "Sending...", 10, x + 16.0f, ty, &ps, 0.0f);
    ui->dbg_pending_echoes++;
    return;
  }
  ccord_text_style fs = ui_style(ui, CCORD_FONT_REGULAR, size, COL(danger));
  ccord_text_draw(r, "Failed to send", 14, x, ty, &fs, 0.0f);
  float fw = ccord_text_measure(r, "Failed to send", 14, &fs, 0.0f).x;
  ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, size, COL(danger));
  ccord_vec2 rt = ccord_text_measure(r, "Retry", 5, &rs, 0.0f);
  ccord_rect pill = {x + fw + 10.0f, y, rt.x + 16.0f, UI_SEND_STATUS_H};
  bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, pill);
  ccord_draw_rrect(r, pill, 4.0f, hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_text_draw(r, "Retry", 5, pill.x + 8.0f, y + (UI_SEND_STATUS_H - rt.y) * 0.5f, &rs, 0.0f);
  if (ui->nretry_hits < UI_RETRY_HIT_MAX) {
    ui->retry_hits[ui->nretry_hits].rect = pill;
    ui->retry_hits[ui->nretry_hits].channel_id = m->channel_id;
    ui->retry_hits[ui->nretry_hits].message_id = m->id;
    ui->nretry_hits++;
  }
  ui->dbg_failed_echoes++;
}

// Compact rows hide the header line, so an edit marker is appended to the body text there.
// ---- Blocked-author runs ----
#define UI_BLOCKED_ROW_H 30.0f
#define UI_BLOCKED_HIDDEN_H 0.01f

typedef struct blocked_run {
  bool blocked;
  bool first;
  bool revealed;
  ccord_id first_id;
  uint32_t count;
} blocked_run;

static bool msg_author_blocked(const ccord_ui *ui, const ccord_message *m)
{
  return m && m->author_id != CCORD_ID_NONE && !ccord_message_is_ephemeral(m) &&
         (ccord_store_relationship(ui->d.store, m->author_id) == CCORD_REL_BLOCKED ||
          ui_user_is_ignored(ui, m->author_id));
}

static bool blocked_is_revealed(const ccord_ui *ui, ccord_id first_id)
{
  for (size_t k = 0; k < ui->nblocked_revealed; k++) {
    if (ui->blocked_revealed[k] == first_id) return true;
  }
  return false;
}

static blocked_run blocked_run_at(const ccord_ui *ui, ccord_id channel, size_t store_i, const ccord_message *m)
{
  blocked_run br = {0};
  if (store_i == SIZE_MAX || !msg_author_blocked(ui, m)) return br;
  br.blocked = true;
  struct ui_chat_caches *cc = ui->chat_caches;
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (cc && cc->br_valid && cc->br_rev == rev && cc->br_blocked_sig == ui->blocked_sig &&
      cc->br_chan == channel && store_i >= cc->br_a && store_i <= cc->br_b) {
    br.first = cc->br_a == store_i;
    br.first_id = cc->br_first_id;
    br.count = (uint32_t)(cc->br_b - cc->br_a + 1u);
    br.revealed = blocked_is_revealed(ui, br.first_id);
    return br;
  }
  size_t a = store_i;
  while (a > 0 && msg_author_blocked(ui, ccord_store_message_at(ui->d.store, channel, a - 1))) a--;
  size_t total = ccord_store_message_count(ui->d.store, channel);
  size_t b = store_i;
  while (b + 1 < total && msg_author_blocked(ui, ccord_store_message_at(ui->d.store, channel, b + 1))) b++;
  const ccord_message *fm = ccord_store_message_at(ui->d.store, channel, a);
  br.first = a == store_i;
  br.first_id = fm ? fm->id : m->id;
  br.count = (uint32_t)(b - a + 1);
  br.revealed = blocked_is_revealed(ui, br.first_id);
  if (cc) {
    cc->br_valid = true;
    cc->br_rev = rev;
    cc->br_blocked_sig = ui->blocked_sig;
    cc->br_chan = channel;
    cc->br_a = a;
    cc->br_b = b;
    cc->br_first_id = br.first_id;
  }
  return br;
}

void ui_blocked_reveal_for(ccord_ui *ui, ccord_id channel_id, size_t store_i)
{
  const ccord_message *m = ccord_store_message_at(ui->d.store, channel_id, store_i);
  blocked_run br = blocked_run_at(ui, channel_id, store_i, m);
  if (!br.blocked || br.revealed) return;
  if (ui->nblocked_revealed == sizeof ui->blocked_revealed / sizeof ui->blocked_revealed[0]) {
    memmove(ui->blocked_revealed, ui->blocked_revealed + 1, (ui->nblocked_revealed - 1) * sizeof(ccord_id));
    ui->nblocked_revealed--;
  }
  ui->blocked_revealed[ui->nblocked_revealed++] = br.first_id;
  ui_chat_caches_clear(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

uint64_t ui_blocked_signature(const ccord_ui *ui)
{
  uint64_t sig = 1469598103934665603ULL;
  size_t n = ccord_store_relationship_count(ui->d.store);
  for (size_t i = 0; i < n; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    if (rel && rel->kind == CCORD_RELATIONSHIP_BLOCKED) sig += (rel->user_id ^ 0x9e3779b97f4a7c15ULL) * 1099511628211ULL;
  }
  for (size_t i = 0; i < ui->nignored_users; i++) {
    sig += (ui->ignored_users[i] ^ 0x9e3779b97f4a7c15ULL) * 1099511628211ULL;
  }
  return sig;
}

bool ui_blocked_toggle_at(ccord_ui *ui, float x, float y)
{
  for (size_t i = 0; i < ui->nblocked_hits; i++) {
    if (!ui_in_rect(x, y, ui->blocked_hits[i].rect)) continue;
    ccord_id first = ui->blocked_hits[i].first_id;
    if (ui->blocked_hits[i].revealed) {
      for (size_t k = 0; k < ui->nblocked_revealed; k++) {
        if (ui->blocked_revealed[k] != first) continue;
        memmove(&ui->blocked_revealed[k], &ui->blocked_revealed[k + 1],
                (ui->nblocked_revealed - k - 1) * sizeof(ccord_id));
        ui->nblocked_revealed--;
        break;
      }
    } else if (ui->nblocked_revealed < sizeof ui->blocked_revealed / sizeof ui->blocked_revealed[0]) {
      ui->blocked_revealed[ui->nblocked_revealed++] = first;
    }
    ui_chat_caches_clear(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
    return true;
  }
  return false;
}

// The strip above a run: "N blocked messages" and a Show or Hide control. Its rect is the click target.
static void draw_blocked_strip(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma, float y, const blocked_run *br)
{
  float x = ma->x + M(message_pad_x);
  float w = ma->w - 2.0f * M(message_pad_x);
  ccord_rect strip = {x, y + 2.0f, w, UI_BLOCKED_ROW_H - 4.0f};
  char txt[64];
  snprintf(txt, sizeof txt, "%u blocked message%s", (unsigned)br->count, br->count == 1u ? "" : "s");
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  float ty = strip.y + (strip.h - ui_line_h(ui, ts.size)) * 0.5f;
  ccord_text_draw(r, txt, strlen(txt), strip.x + 4.0f, ty, &ts, 0.0f);
  float tw = ccord_text_measure(r, txt, strlen(txt), &ts, 0.0f).x;
  const char *act = br->revealed ? "Hide" : "Show";
  ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_link));
  ccord_text_draw(r, act, strlen(act), strip.x + 4.0f + tw + 12.0f, ty, &as, 0.0f);
  float aw = ccord_text_measure(r, act, strlen(act), &as, 0.0f).x;
  ccord_rect hit = {strip.x, strip.y, tw + 12.0f + aw + 8.0f, strip.h};
  if (ui->nblocked_hits < sizeof ui->blocked_hits / sizeof ui->blocked_hits[0]) {
    ui->blocked_hits[ui->nblocked_hits++] = (typeof(ui->blocked_hits[0])){hit, br->first_id, br->count, br->revealed};
  }
}

static uint64_t hash_bytes_fnv(const char *s, size_t n)
{
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
  return h;
}

static void rowh_cache_sync(struct ui_chat_caches *cc, const ccord_ui *ui, const ccord_renderer *r)
{
  int tex_cnt = r ? ccord_render_texture_cache_count(r) : 0;
  if (cc->rh_r != r || cc->rh_theme != ui->theme || cc->rh_tex_count != tex_cnt ||
      cc->rh_nrevealed_spoilers != ui->nrevealed_spoilers ||
      cc->rh_nblocked_revealed != ui->nblocked_revealed || cc->rh_blocked_sig != ui->blocked_sig) {
    cc->rh_r = r;
    cc->rh_theme = ui->theme;
    cc->rh_tex_count = tex_cnt;
    cc->rh_nrevealed_spoilers = ui->nrevealed_spoilers;
    cc->rh_nblocked_revealed = ui->nblocked_revealed;
    cc->rh_blocked_sig = ui->blocked_sig;
    memset(cc->rowh, 0, sizeof cc->rowh);
    memset(cc->embedh, 0, sizeof cc->embedh);
  }
}

// Emoji-only chat messages (up to UI_JUMBO_EMOJI_MAX Unicode or custom emojis) render enlarged (3x base font = 48 px).
static size_t chat_message_jumbo(const ccord_ui *ui, const ccord_message *m,
                                 ui_jumbo_item out[static UI_JUMBO_EMOJI_MAX])
{
  if (!m || !m->content || m->content[0] == '\0') return 0;
  if (m->rich && ccord_message_type_is_system(m->rich->type)) return 0;
  if (message_should_hide_bare_link(ui, m)) return 0;
  return ui_parse_jumbo_emojis(m->content, strlen(m->content), out);
}

static float chat_jumbo_emoji_walk(ccord_ui *ui, ccord_renderer *r, const ui_jumbo_item *items, size_t count,
                                   float x, float y, float wrap, ccord_color base, bool show_edited,
                                   bool show_tts, bool draw)
{
  float jumbo_sz = M(font_size_base) * 3.0f;   // 48 px at default 16 px base font
  float gap = 4.0f;
  float line_h = jumbo_sz + 4.0f;
  float top_pad = 4.0f;
  float cx = 0.0f, cy = top_pad;
  bool line_has = false;
  for (size_t i = 0; i < count; i++) {
    const ui_jumbo_item *it = &items[i];
    if (line_has && (it->line_break_before || cx + jumbo_sz > wrap)) {
      if (draw) {
        chat_sel_line_end(ui, r, x + cx, y + cy, 6.0f, line_h, it->line_break_before);
        record_text_hit(ui, (ccord_rect){x, y + cy, cx, line_h});
      }
      cx = 0.0f;
      cy += line_h;
      line_has = false;
    }
    if (draw) {
      ccord_rect er = {x + cx, y + cy + (line_h - jumbo_sz) * 0.5f, jumbo_sz, jumbo_sz};
      if (it->custom) {
        float sel_x0, sel_x1;
        bool goes_on;
        if (chat_sel_line(ui, y + cy, line_h, &sel_x0, &sel_x1, &goes_on) &&
            sel_x1 > er.x && sel_x0 < er.x + er.w) {
          chat_sel_fill(ui, r, (ccord_rect){er.x, y + cy, er.w, line_h});
          chat_sel_append(ui, it->raw, it->raw_len, false);
        }
        ui_expr_draw_custom_emoji(ui, r, it->id, it->text, it->len, it->animated, er);
      } else {
        ccord_text_style es = ui_style(ui, CCORD_FONT_EMOJI, jumbo_sz, base);
        ccord_vec2 et = ccord_text_measure(r, it->text, it->len, &es, 0.0f);
        float ex = er.x + (er.w - et.x) * 0.5f;
        float ey = er.y + (er.h - et.y) * 0.5f;
        chat_sel_process_run(ui, r, it->text, it->len, &es, ex, y + cy, maxf(et.x, jumbo_sz), line_h, 0.0f);
        ccord_text_draw(r, it->text, it->len, ex, ey, &es, 0.0f);
      }
    }
    cx += jumbo_sz + gap;
    line_has = true;
  }
  // Grouped/compact follow-up messages carry "(edited)" / "(tts)" inline at the end of the body.
  if (show_edited || show_tts) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float tag_lh = ui_line_h(ui, ts.size);
    const char *tags[2] = {show_edited ? "(edited)" : nullptr, show_tts ? "(tts)" : nullptr};
    for (int ti = 0; ti < 2; ti++) {
      if (!tags[ti]) continue;
      size_t tl = strlen(tags[ti]);
      float tw = ccord_text_measure(r, tags[ti], tl, &ts, 0.0f).x;
      if (line_has && cx + tw > wrap) {
        if (draw) {
          chat_sel_line_end(ui, r, x + cx, y + cy, 6.0f, line_h, false);
          record_text_hit(ui, (ccord_rect){x, y + cy, cx, line_h});
        }
        cx = 0.0f;
        cy += line_h;
        line_has = false;
      }
      if (draw) {
        float ty = y + cy + (line_h - tag_lh) - 4.0f;
        chat_sel_process_run(ui, r, tags[ti], tl, &ts, x + cx, y + cy, tw, line_h, 0.0f);
        ccord_text_draw(r, tags[ti], tl, x + cx, ty, &ts, 0.0f);
      }
      cx += tw + 6.0f;
      line_has = true;
    }
  }
  if (draw) {
    chat_sel_line_end(ui, r, x + cx, y + cy, 6.0f, line_h, true);
    record_text_hit(ui, (ccord_rect){x, y + cy, cx, line_h});
  }
  return cy + line_h;
}

static float measure_row(size_t i, float width, void *ud)
{
  msg_ctx *c = ud;
  ccord_ui *ui = c->ui;
  size_t store_i = 0;
  const ccord_message *m = view_message_at(ui, c->channel, i, &store_i);
  if (!m) return 1.0f;
  blocked_run br = blocked_run_at(ui, c->channel, store_i, m);
  if (br.blocked) {
    float bintro = is_dm_intro_row(ui, c->channel, store_i) ? UI_DM_INTRO_H : 0.0f;
    float bdiv = (ui_row_has_date_divider(ui, c->channel, store_i, m) ||
                  ui_row_has_new_divider(ui, c->channel, store_i, m)) ? UI_CHAT_DIVIDER_H : 0.0f;
    if (!br.revealed) {
      return bintro + bdiv + (br.first ? UI_BLOCKED_ROW_H : UI_BLOCKED_HIDDEN_H) + tail_pad(ui, i);
    }
    // revealed: the normal row below, plus the Hide strip above the first message of the run
  }
  bool compact = store_i != SIZE_MAX && ui_is_compact(ui, c->channel, store_i, m);
  bool has_intro = is_dm_intro_row(ui, c->channel, store_i);
  bool has_div = store_i != SIZE_MAX &&
                 (ui_row_has_date_divider(ui, c->channel, store_i, m) ||
                  ui_row_has_new_divider(ui, c->channel, store_i, m));
  float tpad = tail_pad(ui, i);
  const char *content = m->content ? m->content : "";
  size_t content_len = strlen(content);
  struct ui_chat_caches *cc = chat_caches_ensure(ui);
  int width_q = (int)lroundf(width * 2.0f);
  float zoom = c->r ? ccord_render_get_zoom(c->r) : 1.0f;
  int scale_q = (int)lroundf(zoom * 100.0f) * 256 + (int)lroundf(M(font_size_base) * 4.0f);
  uint32_t flags = (compact ? 1u : 0u) | (ui_messages_compact(ui) ? 2u : 0u) | (has_intro ? 4u : 0u) |
                   (has_div ? 8u : 0u) | (br.blocked ? 16u : 0u) | (br.first ? 32u : 0u) |
                   (br.revealed ? 64u : 0u) | (ui->settings_embeds ? 128u : 0u) |
                   (ui->settings_attachments ? 256u : 0u) | (ui_streamer_invites_active(ui) ? 512u : 0u) |
                   (ccord_message_is_ephemeral(m) ? 1024u : 0u) | (((uint32_t)m->send_state & 3u) << 11) |
                   (tpad > 0.0f ? (1u << 13) : 0u) | (m->edited ? (1u << 14) : 0u) |
                   (m->reply_to_id != CCORD_ID_NONE ? (1u << 15) : 0u) | (m->tts ? (1u << 16) : 0u);
  uint32_t meta_sig = (uint32_t)(m->reaction_kind_count * 31u + m->reaction_count);
  if (m->rich) {
    meta_sig ^= (uint32_t)((m->rich->attachment_count << 8) ^ (m->rich->embed_count << 12) ^
                           (m->rich->component_row_count << 16) ^ (m->rich->v2_component_count << 20) ^
                           (m->rich->sticker_count << 24) ^ (m->rich->poll.present ? (1u << 28) : 0u) ^
                           (m->rich->flags));
  }
  uint64_t chash = hash_bytes_fnv(content, content_len);
  if (cc && m->id != CCORD_ID_NONE) {
    rowh_cache_sync(cc, ui, c->r);
    for (size_t k = 0; k < CHAT_ROWH_CACHE_SLOTS; k++) {
      chat_rowh_entry *e = &cc->rowh[k];
      if (e->msg_id == m->id && e->edited_ts == m->edited_timestamp_ms && e->edit_rev == m->edit_rev &&
          e->content_len == content_len &&
          e->content_hash == chash && e->width_q == width_q && e->scale_q == scale_q &&
          e->flags == flags && e->meta_sig == meta_sig) {
        e->lru_tick = ++cc->rh_tick;
        return e->height;
      }
    }
  }
  decor_info d;
  decor_scan(ui, m, compact, &d);
  float lh = ui_line_h(ui, M(font_size_base));
  float wrap = content_wrap(ui, width);
  bool no_body = d.body_len == 0 && decor_has_body_cards(&d);
  ui_jumbo_item jumbo_items[UI_JUMBO_EMOJI_MAX];
  size_t jumbo_n = no_body ? 0u : chat_message_jumbo(ui, m, jumbo_items);
  float body_h = no_body
                     ? 0.0f
                     : (jumbo_n > 0u
                            ? chat_jumbo_emoji_walk(ui, c->r, jumbo_items, jumbo_n, 0.0f, 0.0f, wrap,
                                                    COL(text_normal), compact && message_edited(m),
                                                    compact && m->tts, false)
                            : maxf(md_body_walk(ui, c->r, d.body, d.body_len, 0.0f, 0.0f, wrap, COL(text_normal), false), lh));
  float top = compact ? M(message_line_gap)
                      : (ui_messages_compact(ui)
                             ? M(message_line_gap) + (M(message_group_gap) - M(message_line_gap)) * 0.35f
                             : M(message_group_gap));
  float header = compact ? 0.0f : lh;
  float reactions = message_has_reactions(m)
                        ? M(message_line_gap) + reaction_pills_walk(ui, c->r, m, 0.0f, 0.0f, wrap, false)
                        : 0.0f;
  float ephem = ccord_message_is_ephemeral(m) ? UI_EPHEMERAL_FOOTER_H : 0.0f;
  float send_status = send_status_extra_h(ui, m);
  float intro = has_intro ? UI_DM_INTRO_H : 0.0f;
  float div_h = has_div ? UI_CHAT_DIVIDER_H : 0.0f;
  float card_w = maxf(width - M(message_text_x) - M(message_pad_x), 40.0f);
  float blocked_strip = (br.blocked && br.first) ? UI_BLOCKED_ROW_H : 0.0f;
  float total_h = intro + div_h + blocked_strip + top + decor_reply_h(&d) + header + body_h +
                  decor_total_h(ui, c->r, &d, card_w) + reactions + send_status + ephem + M(message_line_gap) + tpad;
  if (cc && m->id != CCORD_ID_NONE) {
    size_t victim = 0;
    for (size_t k = 0; k < CHAT_ROWH_CACHE_SLOTS; k++) {
      if (cc->rowh[k].msg_id == CCORD_ID_NONE) {
        victim = k;
        break;
      }
      if (cc->rowh[k].lru_tick < cc->rowh[victim].lru_tick) victim = k;
    }
    cc->rowh[victim] = (chat_rowh_entry){
      .content_hash = chash,
      .msg_id = m->id,
      .edited_ts = m->edited_timestamp_ms,
      .content_len = content_len,
      .edit_rev = m->edit_rev,
      .width_q = width_q,
      .scale_q = scale_q,
      .flags = flags,
      .meta_sig = meta_sig,
      .height = total_h,
      .lru_tick = ++cc->rh_tick,
    };
  }
  return total_h;
}

// One compact line above a reply's header: vertical quote bar, the replied-to author's avatar, their name in their role
// colour, then a one-line snippet of the original in muted text (brighter on hover). Whole line is the click target; a
// deleted original is not clickable.
static void draw_reply_line(ccord_ui *ui, ccord_renderer *r, const decor_info *d, ccord_id guild, float sx, float y1,
                            float x, float y, float w)
{
  (void)sx;
  (void)y1;
  float h = UI_REPLY_LINE_H;
  ccord_rect line = {x, y, w, h};
  float cy = y + h * 0.5f;
  bool clickable = d->reply_target_id != CCORD_ID_NONE && !d->reply_deleted;
  bool hovered = clickable && ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, line);
  float hv = ui_fade(ui, "replyln", d->msg_id, hovered);
  ccord_color bar_col = hv > 0.01f ? ui_mix(COL(divider), COL(brand), hv) : COL(divider);
  ccord_draw_rrect(r, (ccord_rect){x - 8.5f, y + 2.5f, 2.5f, h - 5.0f}, 1.25f, bar_col);

  ccord_rect av = {0, 0, 0, 0};
  bool has_user = !d->reply_deleted && d->reply_author_id != CCORD_ID_NONE;
  if (has_user) {
    const ccord_user *u = ccord_store_user(ui->d.store, d->reply_author_id);
    const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, d->reply_author_id);
    ccord_user ru = {
      .id = d->reply_author_id,
      .name = d->reply_name,
      .handle = (u && u->handle && u->handle[0]) ? u->handle : "",
      .status_text = "",
      .avatar_url = (u && u->avatar_url && u->avatar_url[0]) ? u->avatar_url
                    : ((prof && prof->avatar_url) ? prof->avatar_url : ""),
      .presence = CCORD_PRESENCE_OFFLINE,
      .is_bot = u ? u->is_bot : false,
      .avatar_palette = u ? u->avatar_palette : (uint8_t)(d->reply_author_id % 6u),
    };
    av = (ccord_rect){x, cy - UI_REPLY_AVATAR * 0.5f, UI_REPLY_AVATAR, UI_REPLY_AVATAR};
    ui_draw_avatar(ui, r, &ru, av.x + av.w * 0.5f, av.y + av.h * 0.5f, UI_REPLY_AVATAR, (ccord_color){0, 0, 0, 0}, false);
  } else {
    ui_icon_reply(r, x + UI_REPLY_AVATAR * 0.5f, cy, 12.0f, COL(text_muted));
  }
  float tx = x + UI_REPLY_AVATAR + 6.0f;
  float right = x + w;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small),
                                 ui_mix(COL(text_muted), COL(text_header), hv * 0.85f));
  float ty = y + (h - ui_line_h(ui, ts.size)) * 0.5f;
  ccord_rect snip = {0, 0, 0, 0};
  ccord_rect rname_rect = {0, 0, 0, 0};
  if (d->reply_deleted) {
    // Italic is the markdown convention here (a mixed-in secondary tone); there is no italic face.
    ts.color = ui_mix(COL(text_muted), COL(text_secondary), 0.35f);
    float tw = ccord_text_draw_ellipsis(r, d->reply_text, strlen(d->reply_text), tx, ty, maxf(right - tx, 0.0f), &ts);
    snip = (ccord_rect){tx, ty, tw, ui_line_h(ui, ts.size)};
  } else {
    bool has_role = false;
    ccord_color rc = ui_role_color(ui, guild, d->reply_author_id, &has_role);
    if (has_role) ns.color = rc;
    const char *nm = d->reply_name[0] ? d->reply_name : "a message";
    float name_max = fminf(maxf(right - tx, 0.0f), maxf(w * 0.4f, 60.0f));
    float nw = ccord_text_draw_ellipsis(r, nm, strlen(nm), tx, ty, name_max, &ns);
    if (has_user) rname_rect = (ccord_rect){tx, ty, maxf(nw, 4.0f), ui_line_h(ui, ns.size)};
    float sx2 = tx + nw + 6.0f;
    if (right - sx2 > 24.0f) {
      char rtext[sizeof d->reply_text];   // Streamer Mode: the replied-to text may carry an invite
      memcpy(rtext, d->reply_text, sizeof rtext);
      size_t rtl = ui_streamer_mask_inplace(ui, rtext, strlen(rtext), true);
      float avail = right - sx2;
      if (d->reply_attach_only) {
        ui_icon_file(r, sx2 + 6.0f, cy, 13.0f, ts.color);
        sx2 += 16.0f;
        avail -= 16.0f;
        ts.color = ui_mix(ts.color, COL(text_secondary), 0.35f);
      }
      float sw = ccord_text_draw_ellipsis(r, rtext, rtl, sx2, ty, maxf(avail, 0.0f), &ts);
      snip = (ccord_rect){sx2, ty, sw, ui_line_h(ui, ts.size)};
    }
  }
  ui->dbg_reply_cards++;
  bool in_area = line.y >= ui->layout.message_area.y - 0.01f &&
                 line.y + line.h <= ui->layout.message_area.y + ui->layout.message_area.h + 0.01f;
  if (clickable && in_area && ui->nreply_hits < UI_REPLY_HIT_MAX) {
    ui->reply_hits[ui->nreply_hits++] = (ui_reply_hit){line, d->reply_target_id, av, snip};
  }
  if (has_user && in_area) {
    if (av.w > 0.0f) record_user_hit(ui, av, d->reply_author_id);
    if (rname_rect.w > 0.0f) record_user_hit(ui, rname_rect, d->reply_author_id);
  }
}

// "2.0 KB" style sizes for the detail line of an attachment.
static void format_size(uint64_t bytes, char *out, size_t cap)
{
  if (bytes < 1024u) {
    snprintf(out, cap, "%llu bytes", (unsigned long long)bytes);
    return;
  }
  const char *unit = "KB";
  double v = (double)bytes / 1024.0;
  if (v >= 1024.0) { v /= 1024.0; unit = "MB"; }
  if (v >= 1024.0) { v /= 1024.0; unit = "GB"; }
  snprintf(out, cap, "%.1f %s", v, unit);
}

// The file name to show: as uploaded, or the last part of the link when Discord did not say.
static const char *attachment_label(const ccord_attachment *a, size_t *len)
{
  const char *name = a->filename ? a->filename : "";
  if (name[0] == '\0') {
    const char *u = (a->url && a->url[0]) ? a->url : (a->proxy_url ? a->proxy_url : "");
    const char *slash = strrchr(u, '/');
    name = slash ? slash + 1 : u;
    *len = strcspn(name, "?#");
    if (*len == 0) { name = "attachment"; *len = strlen(name); }
    return name;
  }
  *len = strlen(name);
  return name;
}

static void draw_attachment_card(ccord_ui *ui, ccord_renderer *r, const ccord_attachment *a, float x, float y, float w)
{
  float h = 46.0f;
  ccord_rect card = {x, y, w, h};
  ccord_draw_rrect(r, card, 4.0f, COL(bg_secondary));
  ccord_draw_rect(r, (ccord_rect){x, y, w, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){x, y + h - 1.0f, w, 1.0f}, COL(border_subtle));
  ccord_rect ib = {x + 6.0f, y + 6.0f, 34.0f, 34.0f};
  ccord_draw_rrect(r, ib, 4.0f, COL(bg_tertiary));
  const char *url = (a->url && a->url[0]) ? a->url : (a->proxy_url ? a->proxy_url : "");
  bool drew_tex = false;
  if (url[0]) {
    record_link_hit(ui, card, url, strlen(url));
    if (ccord_attachment_is_image(a) && !a->spoiler && ui_ensure_media(ui, r, url)) {
      ccord_draw_texture_rounded_anim(r, url, ib, 4.0f, 255, ui_media_animate(ui, ib));
      ui->dbg_attachment_textures_drawn++;
      drew_tex = true;
    }
  }
  if (!drew_tex) {
    ui_icon_attachment(r, ib.x + ib.w * 0.5f, ib.y + ib.h * 0.5f, 18.0f, COL(text_secondary));
  }
  size_t nl = 0;
  const char *name = attachment_label(a, &nl);
  char detail[160];
  char size[32] = "";
  if (a->size > 0) format_size(a->size, size, sizeof size);
  const char *type = (a->content_type && a->content_type[0]) ? a->content_type : "";
  snprintf(detail, sizeof detail, "%s%s%s%s", a->spoiler ? "Spoiler  " : "", type, (type[0] && size[0]) ? "  " : "", size);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_link));
  ccord_text_style ds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, name, nl, x + 48.0f, y + 6.0f, w - 56.0f, &ns);
  const char *shown = detail[0] ? detail : "attachment";
  ccord_text_draw_ellipsis(r, shown, strlen(shown), x + 48.0f, y + 22.0f, w - 56.0f, &ds);
}

static const char *attachment_url(const ccord_attachment *a)
{
  if (!a) return "";
  if (a->url && a->url[0]) return a->url;
  return a->proxy_url ? a->proxy_url : "";
}

// Embed media is loaded from Discord's copy (proxy_url): the site it came from is never contacted, and is not
// on the media allowlist anyway.
static const char *embed_media_url(const ccord_embed_media *m)
{
  if (!m) return "";
  if (m->proxy_url && m->proxy_url[0]) return m->proxy_url;
  return m->url ? m->url : "";
}

// A video file an embed points at that the inline player can fetch directly: Discord's proxied copy, a direct media
// file link, or a known direct-video CDN / tweet-fixer redirect host (excluding HLS/DASH manifests). "" for player
// pages (YouTube, X/Twitter watch pages, etc.), which resolve via embed_resolvable_video_url().
static const char *embed_video_url(const ccord_embed *e)
{
  if (e->video.proxy_url && e->video.proxy_url[0]) return e->video.proxy_url;
  const char *u = e->video.url ? e->video.url : "";
  if (!u[0]) return "";
  if (ui_url_path_ends(u, ".mp4") || ui_url_path_ends(u, ".webm") || ui_url_path_ends(u, ".mov") ||
      ui_url_path_ends(u, ".m4v") || ui_url_path_ends(u, ".mkv") || ui_url_path_ends(u, ".ogv")) {
    return u;
  }
  if (!ui_url_path_ends(u, ".m3u8") && !ui_url_path_ends(u, ".mpd")) {
    static const char *const k_direct_vid_hosts[] = {
      "video.twimg.com", "d.fxtwitter.com", "t.fxtwitter.com", "dl.fxtwitter.com", "d.vxtwitter.com",
    };
    if (url_host_in_list(u, k_direct_vid_hosts, sizeof k_direct_vid_hosts / sizeof k_direct_vid_hosts[0])) return u;
  }
  return "";
}

// Looping clip URL for a GIF/GIFV embed, plus an optional origin fallback when proxy_url fails.
static const char *embed_gif_clip_url(const ccord_embed *e, const char **fallback_out)
{
  if (fallback_out) *fallback_out = "";
  if (!e) return "";
  const char *vproxy = (e->video.proxy_url && e->video.proxy_url[0]) ? e->video.proxy_url : "";
  const char *vurl = (e->video.url && e->video.url[0]) ? e->video.url : "";
  bool vurl_ok = vurl[0] && (e->kind == CCORD_EMBED_GIFV || ui_url_path_ends(vurl, ".mp4") ||
                             ui_url_path_ends(vurl, ".webm") || ui_url_path_ends(vurl, ".mov") ||
                             ui_url_path_ends(vurl, ".m4v") || url_is_anim_image(vurl));
  if (vproxy[0]) {
    if (fallback_out && vurl_ok && strcmp(vproxy, vurl) != 0) *fallback_out = vurl;
    return vproxy;
  }
  return vurl_ok ? vurl : "";
}

static bool url_is_x_host(const char *u)
{
  static const char *const k_x[] = {
    "x.com", "www.x.com", "mobile.x.com", "twitter.com", "www.twitter.com", "mobile.twitter.com",
    "fxtwitter.com", "www.fxtwitter.com", "vxtwitter.com", "www.vxtwitter.com",
    "fixupx.com", "www.fixupx.com", "fixvx.com", "www.fixvx.com", "twittpr.com", "xcancel.com",
  };
  return url_host_in_list(u, k_x, sizeof k_x / sizeof k_x[0]);
}

// Discord often sends X/Twitter video embeds without populating `embed.video`, putting Twitter's video thumbnail URL
// (`ext_tw_video_thumb/`, `amplify_video_thumb/`, `tweet_video_thumb/`) in `image` or `thumbnail` instead.
static bool embed_is_x_video_thumb(const ccord_embed *e)
{
  if (!e || !e->url || !e->url[0]) return false;
  if (!url_is_x_host(e->url)) return false;
  const char *urls[4] = {e->image.url, e->image.proxy_url, e->thumbnail.url, e->thumbnail.proxy_url};
  for (size_t i = 0; i < 4; i++) {
    if (!urls[i] || !urls[i][0]) continue;
    if (strstr(urls[i], "ext_tw_video_thumb/") || strstr(urls[i], "amplify_video_thumb/") ||
        strstr(urls[i], "tweet_video_thumb/")) {
      return true;
    }
  }
  return false;
}

// Canonical https:// page URL for a third-party video embed (YouTube, X/Twitter, Vimeo, etc.) that needs on-click
// resolution before streaming inline. Returns "" when the embed is not a resolvable video page.
static const char *embed_resolvable_video_url(const ccord_embed *e)
{
  if (!e) return "";
  bool has_vid = (e->video.url && e->video.url[0]) || (e->video.proxy_url && e->video.proxy_url[0]);
  if (!has_vid && !embed_is_x_video_thumb(e)) return "";
  if (e->url && strncasecmp(e->url, "https://", 8) == 0) return e->url;
  if (e->video.url && strncasecmp(e->video.url, "https://", 8) == 0) return e->video.url;
  return "";
}

// Formats the media proxy must not convert to a WebP still.
static bool att_keeps_format(const ccord_attachment *a)
{
  const char *ct = a->content_type ? a->content_type : "";
  return strstr(ct, "gif") || strstr(ct, "webp") || strstr(ct, "apng") || url_is_anim_image(attachment_url(a));
}

// What to load for an attachment shown inline: the media proxy's screen-sized copy when there is one (a phone
// photo becomes a few dozen KB instead of several MB), else the file itself.
static const char *att_thumb_url(const ccord_attachment *a, char *buf, size_t cap)
{
  if (a->proxy_url && a->proxy_url[0] &&
      ui_media_sized_url(a->proxy_url, a->width, a->height, 800u, 600u, att_keeps_format(a), buf, cap)) {
    return buf;
  }
  return attachment_url(a);
}

// The loaded texture to draw for media with a preferred source and a fallback, or nullptr while neither is ready.
// Whatever is already loaded is used as it is; the fallback is only asked for once the preferred source has failed.
static const char *media_pick(ccord_ui *ui, ccord_renderer *r, const char *primary, const char *fallback)
{
  if (!primary) primary = "";
  if (r && primary[0] && ccord_render_has_texture(r, primary, nullptr, nullptr)) return primary;
  if (r && fallback && fallback[0] && ccord_render_has_texture(r, fallback, nullptr, nullptr)) return fallback;
  if (primary[0] && ui_ensure_media(ui, r, primary)) return primary;
  if (fallback && fallback[0] && strcmp(primary, fallback) != 0 && (!primary[0] || ui_media_failed(ui, primary)) &&
      ui_ensure_media(ui, r, fallback)) {
    return fallback;
  }
  return nullptr;
}

// Fits a w x h picture inside box, centred, keeping its shape.
static ccord_rect fit_rect(ccord_rect box, float w, float h)
{
  if (w <= 0.0f || h <= 0.0f) return box;
  float sc = fminf(box.w / w, box.h / h);
  float fw = roundf(w * sc), fh = roundf(h * sc);
  return (ccord_rect){box.x + floorf((box.w - fw) * 0.5f), box.y + floorf((box.h - fh) * 0.5f), fw, fh};
}

static void format_mmss(uint32_t ms, char *out, size_t cap)
{
  uint32_t sec = ms / 1000u;
  snprintf(out, cap, "%u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

static void draw_spoiler_tile(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, ccord_id key)
{
  ccord_draw_rrect(r, rc, 6.0f, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - 1.0f, rc.w, 1.0f}, COL(border_subtle));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
  ccord_vec2 st = ccord_text_measure(r, "SPOILER", 7, &ss, 0.0f);
  float pw = fminf(st.x + 22.0f, maxf(rc.w - 8.0f, 16.0f));
  float ph = fminf(st.y + 10.0f, maxf(rc.h - 6.0f, 14.0f));
  ccord_rect pill = {rc.x + (rc.w - pw) * 0.5f, rc.y + (rc.h - ph) * 0.5f, pw, ph};
  ccord_draw_rrect(r, pill, ph * 0.5f, COL(bg_primary));
  ccord_text_draw_ellipsis(r, "SPOILER", 7, pill.x + (pill.w - st.x) * 0.5f, pill.y + (pill.h - st.y) * 0.5f,
                           maxf(pw - 6.0f, 8.0f), &ss);
  record_spoiler_hit(ui, rc, key);
}

static float gallery_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw)
{
  if (!p || !p->att || p->count == 0) return 0.0f;
  size_t n = p->count < CCORD_RICH_MAX_ATTACHMENTS ? p->count : CCORD_RICH_MAX_ATTACHMENTS;
  size_t shown = n < 5u ? n : 5u;
  float gw = clampf(w, 80.0f, 520.0f);
  float gap = 4.0f;
  ccord_rect tiles[5] = {0};
  float total_h = 0.0f;

  if (n == 1u) {
    const ccord_attachment *a0 = &p->att[0];
    const char *u0 = attachment_url(a0);
    uint32_t iw = a0->width, ih = a0->height;
    if ((iw == 0 || ih == 0) && r && u0[0]) {
      int tw_px = 0, th_px = 0;
      if (ccord_render_has_texture(r, u0, &tw_px, &th_px) && tw_px > 4 && th_px > 4) {
        iw = (uint32_t)tw_px;
        ih = (uint32_t)th_px;
      }
    }
    float tw = fminf(gw, 360.0f), th = 112.0f;
    if (iw > 0 && ih > 0) {
      float max_w = fminf(gw, 440.0f);
      float max_h = 320.0f;
      float sc = fminf(1.0f, fminf(max_w / (float)iw, max_h / (float)ih));
      tw = clampf((float)iw * sc, 96.0f, max_w);
      th = clampf((float)ih * sc, 72.0f, max_h);
    }
    tiles[0] = (ccord_rect){x, y, tw, th};
    total_h = th;
  } else if (n == 2u) {
    float cell_w = (gw - gap) * 0.5f;
    float th = 168.0f;
    tiles[0] = (ccord_rect){x, y, cell_w, th};
    tiles[1] = (ccord_rect){x + cell_w + gap, y, cell_w, th};
    total_h = th;
  } else if (n == 3u) {
    float left_w = floorf((gw - gap) * 0.60f);
    float right_w = gw - gap - left_w;
    float th = 220.0f;
    float half_h = floorf((th - gap) * 0.5f);
    tiles[0] = (ccord_rect){x, y, left_w, th};
    tiles[1] = (ccord_rect){x + left_w + gap, y, right_w, half_h};
    tiles[2] = (ccord_rect){x + left_w + gap, y + half_h + gap, right_w, th - half_h - gap};
    total_h = th;
  } else if (n == 4u) {
    float cell_w = (gw - gap) * 0.5f;
    float th = 236.0f;
    float half_h = (th - gap) * 0.5f;
    tiles[0] = (ccord_rect){x, y, cell_w, half_h};
    tiles[1] = (ccord_rect){x + cell_w + gap, y, cell_w, half_h};
    tiles[2] = (ccord_rect){x, y + half_h + gap, cell_w, half_h};
    tiles[3] = (ccord_rect){x + cell_w + gap, y + half_h + gap, cell_w, half_h};
    total_h = th;
  } else {
    float top_w = (gw - gap) * 0.5f;
    float bot_w = (gw - 2.0f * gap) / 3.0f;
    float top_h = 124.0f, bot_h = 96.0f;
    tiles[0] = (ccord_rect){x, y, top_w, top_h};
    tiles[1] = (ccord_rect){x + top_w + gap, y, top_w, top_h};
    for (size_t k = 0; k < 3u; k++) {
      tiles[2u + k] = (ccord_rect){x + (float)k * (bot_w + gap), y + top_h + gap, bot_w, bot_h};
    }
    total_h = top_h + gap + bot_h;
  }

  if (!draw) return total_h;

  const char *rev_urls[CCORD_RICH_MAX_ATTACHMENTS];
  size_t rev_idx[CCORD_RICH_MAX_ATTACHMENTS];
  size_t rev_count = 0;
  for (size_t k = 0; k < n; k++) {
    ccord_id sk = spoiler_att_key(p->msg_id, &p->att[k], k);
    bool hidden = p->att[k].spoiler && !is_spoiler_revealed(ui, sk);
    const char *u = attachment_url(&p->att[k]);
    if (!hidden && u[0]) {
      rev_idx[k] = rev_count;
      rev_urls[rev_count] = u;
      rev_count++;
    } else {
      rev_idx[k] = SIZE_MAX;
    }
  }

  for (size_t k = 0; k < shown; k++) {
    ccord_id sk = spoiler_att_key(p->msg_id, &p->att[k], k);
    if (p->att[k].spoiler && !is_spoiler_revealed(ui, sk)) {
      draw_spoiler_tile(ui, r, tiles[k], sk);
      continue;
    }
    ccord_draw_rrect(r, tiles[k], 6.0f, COL(bg_secondary));
    const char *u = attachment_url(&p->att[k]);
    char tbuf[UI_URL_MAX];
    const char *tex = u[0] ? media_pick(ui, r, att_thumb_url(&p->att[k], tbuf, sizeof tbuf), u) : nullptr;
    if (tex) {
      // Fill the tile and crop the overflow: a tile is rarely the picture's own shape.
      ccord_draw_texture_cover(r, tex, tiles[k], 6.0f, 255, ui_media_animate(ui, tiles[k]));
      ui->dbg_attachment_textures_drawn++;
    } else {
      ui_icon_attachment(r, tiles[k].x + tiles[k].w * 0.5f, tiles[k].y + tiles[k].h * 0.5f, 20.0f, COL(text_secondary));
    }
    if (k == 4u && n > 5u) {
      ccord_draw_rrect(r, tiles[k], 6.0f, ui_alpha(COL(overlay), 0.72f));
      char more[16];
      snprintf(more, sizeof more, "+%zu", n - 5u);
      ccord_text_style ms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_header));
      ccord_vec2 mt = ccord_text_measure(r, more, strlen(more), &ms, 0.0f);
      ccord_text_draw(r, more, strlen(more), tiles[k].x + (tiles[k].w - mt.x) * 0.5f,
                      tiles[k].y + (tiles[k].h - mt.y) * 0.5f, &ms, 0.0f);
    }
    if (rev_idx[k] < rev_count) {
      record_img_hit(ui, tiles[k], rev_urls, rev_count, rev_idx[k]);
    }
  }
  return total_h;
}

// A voice message is an audio attachment with a waveform (Discord sends one with every voice message).
static bool decor_is_voice(const ccord_attachment *a)
{
  return a && ccord_attachment_is_audio(a) && a->waveform_b64 && a->waveform_b64[0] != '\0';
}

// Waveform player for a voice message: play button, one bar per waveform byte (played bars in brand), duration.
// Height is the audio card's 58px; the bars are the scrub area, so ui_media_hit seeks like any player.
static void draw_voice_message_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  const ccord_attachment *a = p->att;
  float cw = clampf(w, 140.0f, 460.0f);
  float h = media_player_h(a, w);
  ccord_rect card = {x, y, cw, h};
  ccord_id skey = spoiler_att_key(p->msg_id, a, 0);
  if (a->spoiler && !is_spoiler_revealed(ui, skey)) {
    draw_spoiler_tile(ui, r, card, skey);
    return;
  }
  ui->vm.dbg_cards++;

  const char *url = attachment_url(a);
  ccord_id att_id = a->id != CCORD_ID_NONE ? a->id : (p->msg_id ^ ui_name_id(url));
  bool active = (ui->media_att_id == att_id && url[0] && strcmp(ui->media_url, url) == 0);
  bool playing = active && ui->media_playing;
  uint32_t dur_ms = (active && ui->media_duration_ms > 0) ? ui->media_duration_ms : a->duration_ms;
  uint32_t pos_ms = active ? ui->media_pos_ms : 0u;
  if (pos_ms > dur_ms) pos_ms = dur_ms;
  float frac = dur_ms > 0 ? clampf((float)pos_ms / (float)dur_ms, 0.0f, 1.0f) : 0.0f;

  ccord_draw_rrect(r, card, 6.0f, COL(bg_secondary));

  ccord_rect play_r = {x + 8.0f, y + (h - 24.0f) * 0.5f, 24.0f, 24.0f};
  ccord_draw_circle(r, play_r.x + 12.0f, play_r.y + 12.0f, 12.0f, COL(brand));
  if (playing) {
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 8.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 13.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
  } else {
    ccord_draw_triangle(r, (ccord_vec2){play_r.x + 9.5f, play_r.y + 7.0f},
                        (ccord_vec2){play_r.x + 9.5f, play_r.y + 17.0f},
                        (ccord_vec2){play_r.x + 17.5f, play_r.y + 12.0f}, COL(text_on_brand));
  }

  char t_lbl[16];
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
  format_mmss(active ? pos_ms : dur_ms, t_lbl, sizeof t_lbl);
  ccord_vec2 tw = ccord_text_measure(r, t_lbl, strlen(t_lbl), &ts, 0.0f);
  float time_x = x + cw - tw.x - 10.0f;
  ccord_text_draw(r, t_lbl, strlen(t_lbl), time_x, y + (h - tw.y) * 0.5f, &ts, 0.0f);

  float bx = play_r.x + play_r.w + 10.0f;
  ccord_rect bars = {bx, y + 12.0f, maxf(time_x - 10.0f - bx, 12.0f), h - 24.0f};
  uint8_t wv[UI_VM_BARS];
  size_t nw = ui_vm_waveform_bars(a->waveform_b64, wv, sizeof wv);
  size_t nb = (size_t)((bars.w + 1.0f) / 3.0f);
  for (size_t i = 0; i < nb; i++) {
    uint8_t v = nw > 0 ? wv[i * nw / nb] : 0;
    float bh = 3.0f + (float)v / 255.0f * (bars.h - 3.0f);
    bool played = (((float)i + 0.5f) / (float)nb) <= frac;
    ccord_draw_rrect(r, (ccord_rect){bars.x + (float)i * 3.0f, bars.y + (bars.h - bh) * 0.5f, 2.0f, bh}, 1.0f,
                     played ? COL(brand) : COL(text_muted));
  }

  if (url[0] && ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
    ui_media_hit *mh = &ui->media_hits[ui->nmedia_hits++];
    mh->play_rect = play_r;
    mh->scrub_rect = bars;
    mh->open_rect = (ccord_rect){0, 0, 0, 0};
    mh->att_id = att_id;
    mh->duration_ms = a->duration_ms;
    mh->url = strlen(url) < UI_URL_MAX ? ui_hit_cstr(ui, url) : "";
    mh->needs_resolve = false;
  }
}

#define UI_VIDEO_TEX_KEY "__ccord_inline_video__"

// A video the inline player can show in a picture box.
typedef struct video_src {
  const char *url;         // the file (or page URL when needs_resolve) to play; "" = poster-only link
  const char *poster;      // still shown before playback, "" = none
  const char *poster_alt;  // asked for when `poster` fails to load
  ccord_id id;             // player identity (attachment id, or a hash for embeds)
  uint32_t duration_ms;    // 0 = unknown until the file is open
  bool needs_resolve;      // true for third-party video pages (YouTube, X, etc.) resolved on click
} video_src;

static void draw_overlay_pill(ccord_ui *ui, ccord_renderer *r, ccord_rect vp, const char *text)
{
  ccord_text_style os = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, (ccord_color){255, 255, 255, 255});
  size_t tl = strlen(text);
  ccord_vec2 sz = ccord_text_measure(r, text, tl, &os, 0.0f);
  float pw = fminf(sz.x + 20.0f, maxf(vp.w - 8.0f, 16.0f)), ph = sz.y + 10.0f;
  ccord_rect pill = {vp.x + (vp.w - pw) * 0.5f, vp.y + (vp.h - ph) * 0.5f, pw, ph};
  ccord_draw_rrect(r, pill, ph * 0.5f, (ccord_color){0, 0, 0, 190});
  ccord_text_draw_ellipsis(r, text, tl, pill.x + 10.0f, pill.y + 5.0f, maxf(pw - 20.0f, 8.0f), &os);
}

// The picture box of a video: the playing frame (letterboxed, never stretched), else the poster, with a play
// button, a loading or error note, and - with `overlay` - its own scrub bar and click targets along the bottom.
static void draw_video_surface(ccord_ui *ui, ccord_renderer *r, ccord_rect vp, float radius, const video_src *v,
                               bool overlay)
{
  bool can_play = v->url && v->url[0];
  bool active = can_play && ui->media_att_id == v->id && strcmp(ui->media_url, v->url) == 0;
  bool playing = active && ui->media_playing;
  ccord_draw_rrect(r, vp, radius, (ccord_color){0, 0, 0, 255});

  bool drew = false;
  if (active && ui->media_stream) {
    const ccord_av_info *inf = ccord_av_stream_info(ui->media_stream);
    uint32_t serial = 0;
    const uint8_t *px = ccord_av_stream_frame(ui->media_stream, &serial);
    if (px && inf->w > 0 && inf->h > 0) {
      bool have = serial == ui->media_frame_serial && ccord_render_has_texture(r, UI_VIDEO_TEX_KEY, nullptr, nullptr);
      if (!have) {
        ccord_error verr;
        have = ccord_render_upload_rgba(r, UI_VIDEO_TEX_KEY, px, inf->w, inf->h, &verr) == CCORD_OK;
        if (have) ui->media_frame_serial = serial;
        else ccord_error_clear(&verr);
      }
      if (have) {
        ccord_rect fr = fit_rect(vp, (float)inf->w, (float)inf->h);
        bool full = fr.w >= vp.w - 1.0f && fr.h >= vp.h - 1.0f;
        ccord_draw_texture_rounded(r, UI_VIDEO_TEX_KEY, fr, full ? radius : 0.0f, 255);
        ui->dbg_attachment_textures_drawn++;
        drew = true;
      }
    }
  }
  if (!drew) {
    const char *ps = media_pick(ui, r, v->poster, v->poster_alt);
    int tw = 0, th = 0;
    if (ps && ccord_render_has_texture(r, ps, &tw, &th) && tw > 0 && th > 0) {
      ccord_rect fr = fit_rect(vp, (float)tw, (float)th);
      bool full = fr.w >= vp.w - 1.0f && fr.h >= vp.h - 1.0f;
      ccord_draw_texture_rounded_anim(r, ps, fr, full ? radius : 0.0f, 255, false);
      ui->dbg_attachment_textures_drawn++;
      drew = true;
    }
  }

  if (active && ui->media_failed) {
    draw_overlay_pill(ui, r, vp, ui->media_fail_hint[0] ? ui->media_fail_hint : "Couldn't load - click to retry");
  } else if (playing && (ui->media_resolving || ui->media_buffering || !ui->media_loaded)) {
    draw_overlay_pill(ui, r, vp, "Loading...");
  } else if (!playing) {
    float cx = vp.x + vp.w * 0.5f, cy = vp.y + vp.h * 0.5f;
    ccord_draw_circle(r, cx, cy, 22.0f, ui_alpha(drew ? COL(overlay) : COL(brand), 0.86f));
    ccord_draw_triangle(r, (ccord_vec2){cx - 6.0f, cy - 9.0f}, (ccord_vec2){cx - 6.0f, cy + 9.0f},
                        (ccord_vec2){cx + 10.0f, cy}, COL(text_on_brand));
  }
  if (!overlay || !can_play) return;

  ccord_rect play_r = vp, scrub_r = {0, 0, 0, 0};
  if (active && ui->media_loaded && vp.h > 60.0f) {
    uint32_t dur_ms = ui->media_duration_ms, pos_ms = ui->media_pos_ms;
    if (pos_ms > dur_ms) pos_ms = dur_ms;
    float frac = dur_ms > 0 ? clampf((float)pos_ms / (float)dur_ms, 0.0f, 1.0f) : 0.0f;
    const float bar_h = 26.0f;
    ccord_rect bar = {vp.x, vp.y + vp.h - bar_h, vp.w, bar_h};
    play_r.h -= bar_h;
    ccord_draw_rect(r, bar, (ccord_color){0, 0, 0, 150});
    char t_cur[16], t_tot[16], t_lbl[40];
    format_mmss(pos_ms, t_cur, sizeof t_cur);
    format_mmss(dur_ms, t_tot, sizeof t_tot);
    snprintf(t_lbl, sizeof t_lbl, "%s / %s", t_cur, t_tot);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, (ccord_color){255, 255, 255, 230});
    ccord_vec2 tw = ccord_text_measure(r, t_lbl, strlen(t_lbl), &ts, 0.0f);
    float time_x = bar.x + bar.w - tw.x - 8.0f;
    ccord_text_draw(r, t_lbl, strlen(t_lbl), time_x, bar.y + (bar.h - tw.y) * 0.5f, &ts, 0.0f);
    float sx = bar.x + 10.0f, sw = maxf(time_x - 10.0f - sx, 28.0f);
    scrub_r = (ccord_rect){sx, bar.y + 4.0f, sw, bar.h - 8.0f};
    ccord_rect track = {sx, bar.y + bar.h * 0.5f - 2.0f, sw, 4.0f};
    ccord_draw_rrect(r, track, 2.0f, (ccord_color){255, 255, 255, 70});
    if (frac > 0.0f) ccord_draw_rrect(r, (ccord_rect){sx, track.y, maxf(sw * frac, 4.0f), 4.0f}, 2.0f, COL(brand));
    ccord_draw_circle(r, sx + sw * frac, track.y + 2.0f, 5.0f, (ccord_color){255, 255, 255, 255});
  }
  if (ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
    ui_media_hit *mh = &ui->media_hits[ui->nmedia_hits++];
    mh->play_rect = play_r;
    mh->scrub_rect = scrub_r;
    mh->open_rect = (ccord_rect){0, 0, 0, 0};
    mh->att_id = v->id;
    mh->duration_ms = v->duration_ms;
    mh->url = (v->url && strlen(v->url) < UI_URL_MAX) ? ui_hit_cstr(ui, v->url) : "";
    mh->needs_resolve = v->needs_resolve;
  }
}

static float draw_media_player_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  const ccord_attachment *a = p ? p->att : nullptr;
  if (!a) return 0.0f;
  float h = media_player_h(a, w);
  if (decor_is_voice(a)) {
    draw_voice_message_card(ui, r, p, x, y, w);
    return h;
  }
  float cw = media_player_w(a, w);
  ccord_rect card = {x, y, cw, h};
  ccord_id skey = spoiler_att_key(p->msg_id, a, 0);
  if (a->spoiler && !is_spoiler_revealed(ui, skey)) {
    draw_spoiler_tile(ui, r, card, skey);
    return h;
  }

  const char *url = attachment_url(a);
  ccord_id att_id = a->id != CCORD_ID_NONE ? a->id : (p->msg_id ^ ui_name_id(url));
  bool active = (ui->media_att_id == att_id && url[0] && strcmp(ui->media_url, url) == 0);
  bool playing = active && ui->media_playing;
  uint32_t dur_ms = (active && ui->media_duration_ms > 0) ? ui->media_duration_ms
                                                          : (a->duration_ms > 0 ? a->duration_ms : 30000u);
  uint32_t pos_ms = active ? ui->media_pos_ms : 0u;
  if (pos_ms > dur_ms) pos_ms = dur_ms;
  float frac = dur_ms > 0 ? clampf((float)pos_ms / (float)dur_ms, 0.0f, 1.0f) : 0.0f;

  ccord_draw_rrect(r, card, 6.0f, COL(bg_secondary));
  ccord_draw_rect(r, (ccord_rect){x, y, cw, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){x, y + h - 1.0f, cw, 1.0f}, COL(border_subtle));

  bool is_video = ccord_attachment_is_video(a);
  ccord_rect vp = {0, 0, 0, 0};
  if (is_video) {
    vp = (ccord_rect){x + 6.0f, y + 6.0f, cw - 12.0f, h - 6.0f - 2.0f - MEDIA_CONTROLS_H};
    // The media proxy renders a video's first frame when asked for an image: that is the poster.
    char pbuf[UI_URL_MAX];
    const char *poster = "";
    if (a->proxy_url && a->proxy_url[0] &&
        ui_media_sized_url(a->proxy_url, a->width, a->height, 800u, 600u, false, pbuf, sizeof pbuf)) {
      poster = pbuf;
    }
    video_src vs = {.url = url, .poster = poster, .poster_alt = "", .id = att_id, .duration_ms = a->duration_ms};
    draw_video_surface(ui, r, vp, 5.0f, &vs, false);
  }

  float cy_top = is_video ? (vp.y + vp.h + 2.0f) : y;
  size_t nl = 0;
  const char *name = attachment_label(a, &nl);
  char sz_buf[32] = "";
  if (a->size > 0) format_size(a->size, sz_buf, sizeof sz_buf);
  char hdr[224];
  bool failed = active && ui->media_failed;
  snprintf(hdr, sizeof hdr, "%.*s%s%s%s", (int)(nl < 120u ? nl : 120u), name, sz_buf[0] ? "  " : "", sz_buf,
           (failed && !is_video) ? "  -  couldn't load" : "");

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_link));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));

  ccord_vec2 open_sz = ccord_text_measure(r, "Open", 4, &ls, 0.0f);
  ccord_rect open_r = {x + cw - open_sz.x - 18.0f, cy_top + 4.0f, open_sz.x + 12.0f, 18.0f};
  ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x + 10.0f, cy_top + 5.0f, maxf(open_r.x - (x + 16.0f), 32.0f), &ns);
  if (url[0]) {
    ccord_draw_rrect(r, open_r, 4.0f, COL(bg_tertiary));
    ccord_text_draw(r, "Open", 4, open_r.x + 6.0f, open_r.y + (open_r.h - open_sz.y) * 0.5f, &ls, 0.0f);
  }

  ccord_rect play_r = {x + 8.0f, cy_top + 26.0f, 24.0f, 24.0f};
  ccord_draw_circle(r, play_r.x + 12.0f, play_r.y + 12.0f, 12.0f, COL(brand));
  if (playing) {
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 8.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 13.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
  } else {
    ccord_draw_triangle(r, (ccord_vec2){play_r.x + 9.5f, play_r.y + 7.0f},
                        (ccord_vec2){play_r.x + 9.5f, play_r.y + 17.0f},
                        (ccord_vec2){play_r.x + 17.5f, play_r.y + 12.0f}, COL(text_on_brand));
  }

  char t_cur[16], t_tot[16], t_lbl[40];
  format_mmss(pos_ms, t_cur, sizeof t_cur);
  format_mmss(dur_ms, t_tot, sizeof t_tot);
  snprintf(t_lbl, sizeof t_lbl, "%s / %s", t_cur, t_tot);
  ccord_vec2 tw = ccord_text_measure(r, t_lbl, strlen(t_lbl), &ts, 0.0f);
  float time_x = x + cw - tw.x - 10.0f;
  ccord_text_draw(r, t_lbl, strlen(t_lbl), time_x, play_r.y + (play_r.h - tw.y) * 0.5f, &ts, 0.0f);

  float sx = play_r.x + play_r.w + 10.0f;
  float sw = maxf(time_x - 10.0f - sx, 28.0f);
  ccord_rect scrub_r = {sx, play_r.y + 4.0f, sw, 16.0f};
  ccord_rect track_r = {sx, play_r.y + 10.0f, sw, 4.0f};
  ccord_draw_rrect(r, track_r, 2.0f, COL(bg_tertiary));
  if (frac > 0.0f) {
    ccord_draw_rrect(r, (ccord_rect){sx, track_r.y, maxf(sw * frac, 4.0f), 4.0f}, 2.0f, COL(brand));
  }
  ccord_draw_circle(r, sx + sw * frac, track_r.y + 2.0f, 5.0f, COL(text_header));

  if (url[0] && ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
    ui_media_hit *mh = &ui->media_hits[ui->nmedia_hits++];
    mh->play_rect = play_r;
    mh->scrub_rect = scrub_r;
    mh->open_rect = open_r;
    mh->att_id = att_id;
    mh->duration_ms = a->duration_ms;
    mh->url = strlen(url) < UI_URL_MAX ? ui_hit_cstr(ui, url) : "";
    mh->needs_resolve = false;
    if (is_video && ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
      ui_media_hit *vh = &ui->media_hits[ui->nmedia_hits++];
      *vh = *mh;
      vh->play_rect = vp;
      vh->scrub_rect = (ccord_rect){0, 0, 0, 0};
      vh->open_rect = (ccord_rect){0, 0, 0, 0};
    }
  }
  return h;
}

// Streamer Mode: the embed string to draw — a masked copy in buf when invite masking replaced
// an invite code, else the original. *len receives the drawn byte length.
static const char *embed_str_masked(ccord_ui *ui, const char *s, char *buf, size_t cap, bool draw, size_t *len)
{
  size_t n = s ? strlen(s) : 0;
  size_t ml = ui_streamer_mask_invites(ui, s, n, buf, cap, draw);
  *len = ml > 0 ? ml : n;
  return ml > 0 ? buf : (s ? s : "");
}

static float embed_card_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw)
{
  const ccord_embed *e = p ? p->emb : nullptr;
  if (!e) return 0.0f;

  bool has_title = e->title && e->title[0];
  bool has_desc = e->description && e->description[0];
  bool has_prov = e->provider && e->provider[0];
  bool has_auth = e->author_name && e->author_name[0];
  bool has_foot = (e->footer_text && e->footer_text[0]) || e->timestamp_ms > 0;
  const char *img_url = embed_media_url(&e->image);
  const char *thumb_url = embed_media_url(&e->thumbnail);
  const char *vid_url = embed_video_url(e);
  bool has_vid = (e->video.url && e->video.url[0]) || (e->video.proxy_url && e->video.proxy_url[0]);

  // Pure image / GIF / GIFV embed rendered framelessly (no embed border, provider header, or video-card chrome).
  if (embed_is_bare_media(e)) {
    bool is_gif = embed_is_gif_like(e);
    const char *clip_fb = "";
    const char *clip_url = is_gif ? embed_gif_clip_url(e, &clip_fb) : "";
    const char *pic = img_url[0] ? img_url : (thumb_url[0] ? thumb_url : (e->url ? e->url : ""));
    uint32_t mw = 0, mh = 0;
    if (is_gif && clip_url[0] && e->video.width > 0 && e->video.height > 0) {
      mw = e->video.width;
      mh = e->video.height;
    } else if (e->image.width > 0 && e->image.height > 0) {
      mw = e->image.width;
      mh = e->image.height;
    } else if (e->thumbnail.width > 0 && e->thumbnail.height > 0) {
      mw = e->thumbnail.width;
      mh = e->thumbnail.height;
    } else {
      mw = e->video.width;
      mh = e->video.height;
    }
    if ((mw == 0 || mh == 0) && r) {
      int tw_px = 0, th_px = 0;
      const char *probe = (clip_url[0] && ccord_render_has_texture(r, clip_url, &tw_px, &th_px))
                            ? clip_url
                            : (pic[0] && ccord_render_has_texture(r, pic, &tw_px, &th_px) ? pic : nullptr);
      if (probe && tw_px > 4 && th_px > 4) {
        mw = (uint32_t)tw_px;
        mh = (uint32_t)th_px;
      }
    }
    float max_w = clampf(w, 80.0f, 400.0f), max_h = 300.0f;
    float iw = fminf(max_w, 260.0f), ih = 140.0f;
    if (mw > 0 && mh > 0) {
      float sc = fminf(1.0f, fminf(max_w / (float)mw, max_h / (float)mh));
      iw = clampf((float)mw * sc, 96.0f, max_w);
      ih = clampf((float)mh * sc, 72.0f, max_h);
    }
    if (draw) {
      ccord_rect box = {x, y, iw, ih};
      ccord_draw_rrect(r, box, 6.0f, COL(bg_secondary));
      // A "GIF" from Tenor, Klipy, Giphy or Imgur is a short silent clip or animated image: it plays inline as-is.
      bool gifv = is_gif && clip_url[0] != '\0';
      bool animate = ui_media_animate(ui, box);
      char sbuf[UI_URL_MAX];
      const char *still = pic;
      if (ui_media_sized_url(pic, mw, mh, 800u, 600u, (is_gif && !gifv) || url_is_anim_image(pic), sbuf, sizeof sbuf)) {
        still = sbuf;
      }
      const char *shown = nullptr;
      const char *active_clip = "";
      if (gifv && animate) {
        if (ui_ensure_media_ex(ui, r, clip_url, 480, true)) {
          shown = clip_url;
          active_clip = clip_url;
        } else if (clip_fb[0] && ui_media_failed(ui, clip_url) && ui_ensure_media_ex(ui, r, clip_fb, 480, true)) {
          shown = clip_fb;
          active_clip = clip_fb;
        }
      }
      bool moving = shown != nullptr;
      if (!shown) shown = media_pick(ui, r, still, pic);
      if (shown) {
        if (!moving && animate && r && ccord_render_texture_is_animated(r, shown, nullptr, nullptr)) moving = true;
        ccord_draw_texture_cover(r, shown, box, 6.0f, 255, animate);
        ui->dbg_attachment_textures_drawn++;
      }
      if (is_gif && !moving) {
        ccord_text_style gs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_header));
        ccord_vec2 gt = ccord_text_measure(r, "GIF", 3, &gs, 0.0f);
        ccord_rect gp = {box.x + 8.0f, box.y + 8.0f, gt.x + 10.0f, 16.0f};
        ccord_draw_rrect(r, gp, 3.0f, ui_alpha(COL(overlay), 0.75f));
        ccord_text_draw(r, "GIF", 3, gp.x + 5.0f, gp.y + (gp.h - gt.y) * 0.5f, &gs, 0.0f);
      }
      const char *big = active_clip[0] ? active_clip : (gifv ? clip_url : pic);
      if (big[0]) {
        const char *one_url = big;
        record_img_hit(ui, box, &one_url, 1u, 0u);
      }
    }
    return ih;
  }

  float cw = clampf(w, 120.0f, 432.0f);
  float pad_l = 14.0f, pad_r = 12.0f, pad_v = 8.0f;
  float full_w = maxf(cw - pad_l - pad_r, 48.0f);
  const char *resolve_url = vid_url[0] ? "" : embed_resolvable_video_url(e);
  const char *play_url = vid_url[0] ? vid_url : resolve_url;
  bool needs_resolve = !vid_url[0] && resolve_url[0] != '\0';
  // The wide slot under the text holds, in this order of preference: the embed's video (its thumbnail is then the
  // poster), its image, or - for an article - the thumbnail as a lead picture. Only a thumbnail that is none of
  // these sits small in the corner.
  bool video_box = (has_vid || resolve_url[0]) && (play_url[0] || thumb_url[0] || img_url[0]);
  bool lead_thumb = !video_box && !img_url[0] && thumb_url[0] && e->kind == CCORD_EMBED_ARTICLE;
  const char *big_url = video_box ? "" : (img_url[0] ? img_url : (lead_thumb ? thumb_url : ""));
  const ccord_embed_media *big_m = img_url[0] ? &e->image : &e->thumbnail;
  bool has_thumb = thumb_url[0] != '\0' && !video_box && !lead_thumb &&
                   (!img_url[0] || strcmp(thumb_url, img_url) != 0);
  float thumb_w = 80.0f, thumb_h = 80.0f;
  if (has_thumb && e->thumbnail.width > 0 && e->thumbnail.height > 0) {
    float sc = fminf(80.0f / (float)e->thumbnail.width, 80.0f / (float)e->thumbnail.height);
    thumb_w = clampf((float)e->thumbnail.width * sc, 40.0f, 80.0f);
    thumb_h = clampf((float)e->thumbnail.height * sc, 40.0f, 80.0f);
  }
  // Player pages (YouTube and the like) come with a long page description Discord leaves out; X/Twitter posts and
  // tweet fixers keep their text readable above the video.
  if (video_box && !vid_url[0] && has_title && (!needs_resolve || !url_is_x_host(play_url))) has_desc = false;
  float top_w = has_thumb ? maxf(full_w - thumb_w - 12.0f, 48.0f) : full_w;
  float tx = x + pad_l;
  float cy = y + pad_v;
  char emb_mask[CCORD_MESSAGE_MAX_BYTES + 1];   // streamer-mode invite mask scratch (one string at a time)

  struct ui_chat_caches *cc = chat_caches_ensure(ui);
  int emb_wq = (int)lroundf(w * 2.0f);
  float emb_zoom = r ? ccord_render_get_zoom(r) : 1.0f;
  int emb_sq = (int)lroundf(emb_zoom * 100.0f) * 256 + (int)lroundf(M(font_size_base) * 4.0f);
  bool emb_streamer = ui_streamer_invites_active(ui);
  if (cc) rowh_cache_sync(cc, ui, r);

  if (draw) {
    float total_h = 0.0f;
    bool hit_cached = false;
    if (cc && p->msg_id != CCORD_ID_NONE) {
      uintptr_t ep = (uintptr_t)e;
      for (size_t k = 0; k < CHAT_EMBEDH_CACHE_SLOTS; k++) {
        chat_embedh_entry *ce = &cc->embedh[k];
        if (ce->msg_id == p->msg_id && ce->edit_rev == p->edit_rev && ce->emb_ptr == ep && ce->width_q == emb_wq &&
            ce->scale_q == emb_sq && ce->streamer == emb_streamer) {
          ce->lru_tick = ++cc->rh_tick;
          total_h = ce->height;
          hit_cached = true;
          break;
        }
      }
    }
    if (!hit_cached) total_h = embed_card_walk(ui, r, p, x, y, w, false);
    ccord_draw_rrect(r, (ccord_rect){x, y, cw, total_h}, 4.0f, COL(bg_secondary));
    ccord_color stripe = e->has_color ? ui_rgb_color(e->color) : COL(brand);
    ccord_draw_rrect(r, (ccord_rect){x, y, 4.0f, total_h}, 2.0f, stripe);
    if (has_thumb) {
      ccord_rect tr = {x + cw - pad_r - thumb_w, y + pad_v, thumb_w, thumb_h};
      ccord_draw_rrect(r, tr, 4.0f, COL(bg_tertiary));
      char sbuf[UI_URL_MAX];
      const char *small = thumb_url;
      if (ui_media_sized_url(thumb_url, e->thumbnail.width, e->thumbnail.height, 160u, 160u,
                             url_is_anim_image(thumb_url), sbuf, sizeof sbuf)) {
        small = sbuf;
      }
      const char *shown = media_pick(ui, r, small, thumb_url);
      if (shown) {
        ccord_draw_texture_cover(r, shown, tr, 4.0f, 255, ui_media_animate(ui, tr));
        ui->dbg_attachment_textures_drawn++;
      }
      const char *one_thumb = thumb_url;
      record_img_hit(ui, tr, &one_thumb, 1u, 0u);
    }
  }

  if (has_prov) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    float ph = ui_line_h(ui, ps.size);
    if (draw) {
      size_t pl = 0;
      const char *prov = embed_str_masked(ui, e->provider, emb_mask, sizeof emb_mask, draw, &pl);
      ccord_text_draw_ellipsis(r, prov, pl, tx, cy, top_w, &ps);
    }
    cy += ph + 2.0f;
  }

  if (has_auth) {
    ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
    float ah = ui_line_h(ui, as.size);
    bool has_icon = e->author_icon && e->author_icon[0];
    float row_h = maxf(ah, has_icon ? 18.0f : 0.0f);
    if (draw) {
      float ax = tx;
      if (has_icon) {
        ccord_rect ir = {ax, cy + (row_h - 18.0f) * 0.5f, 18.0f, 18.0f};
        ccord_draw_rrect(r, ir, 9.0f, COL(bg_tertiary));
        if (ui_ensure_media(ui, r, e->author_icon)) {
          ccord_draw_texture_rounded(r, e->author_icon, ir, 9.0f, 255);
          ui->dbg_attachment_textures_drawn++;
        }
        ax += 24.0f;
      }
      float max_aw = maxf(top_w - (ax - tx), 24.0f);
      size_t anl = 0;
      const char *aname = embed_str_masked(ui, e->author_name, emb_mask, sizeof emb_mask, draw, &anl);
      float drawn_w = ccord_text_draw_ellipsis(r, aname, anl, ax, cy + (row_h - ah) * 0.5f, max_aw, &as);
      if (e->author_url && e->author_url[0]) {
        record_link_hit(ui, (ccord_rect){ax, cy, maxf(drawn_w, 16.0f), row_h}, e->author_url, strlen(e->author_url));
      }
    }
    cy += row_h + 3.0f;
  }

  const char *shown_title = has_title ? e->title
                            : ((!has_desc && e->field_count == 0 && !img_url[0] && !video_box && e->url && e->url[0])
                                 ? e->url : "");
  if (shown_title[0]) {
    // Invite embeds: the title (or the invite URL shown as title) gets its code masked, and a
    // masked invite is no longer a clickable link.
    size_t title_ml = ui_streamer_mask_invites(ui, shown_title, strlen(shown_title), emb_mask, sizeof emb_mask, draw);
    bool title_masked = title_ml > 0;
    size_t title_len = title_masked ? title_ml : strlen(shown_title);
    if (title_masked) shown_title = emb_mask;
    bool is_link = e->url && e->url[0];
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), is_link ? COL(text_link) : COL(text_header));
    float tw_lim = (has_thumb && cy < y + pad_v + thumb_h) ? top_w : full_w;
    ccord_vec2 tsz = ccord_text_measure(r, shown_title, title_len, &ts, tw_lim);
    float th = maxf(tsz.y, ui_line_h(ui, ts.size));
    if (draw) {
      ccord_text_draw(r, shown_title, title_len, tx, cy, &ts, tw_lim);
      if (is_link && !title_masked) {
        record_link_hit(ui, (ccord_rect){tx, cy, fminf(maxf(tsz.x, 24.0f), tw_lim), th}, e->url, strlen(e->url));
      }
    }
    cy += th + 4.0f;
  }

  if (has_desc) {
    float dw = (has_thumb && cy < y + pad_v + thumb_h) ? top_w : full_w;
    ccord_id prev_id = ui->current_drawing_msg_id;
    ui->current_drawing_msg_id = p->msg_id;
    float dh = md_body_walk(ui, r, e->description, strlen(e->description), tx, cy, dw, COL(text_normal), draw);
    ui->current_drawing_msg_id = prev_id;
    cy += dh + 4.0f;
  }

  if (has_thumb && (e->field_count > 0 || big_url[0] || has_foot) && cy < y + pad_v + thumb_h + 4.0f) {
    cy = y + pad_v + thumb_h + 4.0f;
  }

  size_t fi = 0;
  while (fi < e->field_count) {
    size_t cols = 1u;
    if (e->fields[fi].inline_) {
      while (cols < 3u && fi + cols < e->field_count && e->fields[fi + cols].inline_) cols++;
    }
    float col_gap = 8.0f;
    float col_w = cols > 1u ? maxf((full_w - col_gap * (float)(cols - 1u)) / (float)cols, 32.0f) : full_w;
    float row_h = 0.0f;
    for (size_t c = 0; c < cols; c++) {
      const ccord_embed_field *f = &e->fields[fi + c];
      float fx = tx + (float)c * (col_w + col_gap);
      float fy = cy;
      const char *fname = f->name ? f->name : "";
      const char *fval = f->value ? f->value : "";
      if (fname[0]) {
        float nh = md_inline_walk_sized(ui, r, fname, strlen(fname), fx, fy, col_w, COL(text_header),
                                        M(font_size_small) - 1.0f, true, draw);
        fy += nh + 2.0f;
      }
      if (fval[0]) {
        ccord_id prev_id = ui->current_drawing_msg_id;
        ui->current_drawing_msg_id = p->msg_id;
        float vh = md_body_walk(ui, r, fval, strlen(fval), fx, fy, col_w, COL(text_secondary), draw);
        ui->current_drawing_msg_id = prev_id;
        fy += vh;
      }
      if (draw) ui->dbg_embed_fields_drawn++;
      row_h = maxf(row_h, fy - cy);
    }
    cy += row_h + 6.0f;
    fi += cols;
  }

  if (video_box) {
    const ccord_embed_media *poster_m = thumb_url[0] ? &e->thumbnail : &e->image;
    uint32_t mw = e->video.width ? e->video.width : poster_m->width;
    uint32_t mh = e->video.height ? e->video.height : poster_m->height;
    float bw, bh;
    video_box_size(mw, mh, full_w, 300.0f, &bw, &bh);
    if (draw) {
      ccord_rect vp = {tx, cy + 2.0f, bw, bh};
      const char *poster = thumb_url[0] ? thumb_url : img_url;
      char pbuf[UI_URL_MAX];
      const char *psized = poster;
      if (ui_media_sized_url(poster, poster_m->width, poster_m->height, 800u, 600u, false, pbuf, sizeof pbuf)) {
        psized = pbuf;
      }
      video_src vs = {.url = play_url, .poster = psized, .poster_alt = poster,
                      .id = p->msg_id ^ ui_name_id(play_url), .duration_ms = 0, .needs_resolve = needs_resolve};
      draw_video_surface(ui, r, vp, 4.0f, &vs, true);
      // Not playable or resolvable (e.g. non-HTTPS): the picture opens the page in the browser instead.
      if (!play_url[0] && e->url && e->url[0]) record_link_hit(ui, vp, e->url, strlen(e->url));
    }
    cy += bh + 2.0f + 6.0f;
  } else if (big_url[0]) {
    if (p->count > 1u) {
      ccord_attachment tmp_atts[CCORD_RICH_MAX_ATTACHMENTS];
      memset(tmp_atts, 0, sizeof tmp_atts);
      size_t cnt = p->count < CCORD_RICH_MAX_ATTACHMENTS ? p->count : CCORD_RICH_MAX_ATTACHMENTS;
      for (size_t k = 0; k < cnt; k++) {
        tmp_atts[k].url = embed_media_url(&p->emb[k].image);
        tmp_atts[k].proxy_url = tmp_atts[k].url;   // lets the gallery ask the proxy for a tile-sized copy
        tmp_atts[k].content_type = url_is_anim_image(tmp_atts[k].url) ? "image/gif" : "image/png";
        tmp_atts[k].width = p->emb[k].image.width;
        tmp_atts[k].height = p->emb[k].image.height;
      }
      decor_part gp = {.kind = DECOR_GALLERY, .att = tmp_atts, .count = cnt, .msg_id = p->msg_id};
      cy += 2.0f + gallery_walk(ui, r, &gp, tx, cy + 2.0f, full_w, draw) + 4.0f;
    } else {
      uint32_t mw = big_m->width, mh = big_m->height;
      if ((mw == 0 || mh == 0) && r) {
        int tw_px = 0, th_px = 0;
        if (ccord_render_has_texture(r, big_url, &tw_px, &th_px) && tw_px > 4 && th_px > 4) {
          mw = (uint32_t)tw_px;
          mh = (uint32_t)th_px;
        }
      }
      float iw = full_w, ih = 140.0f;
      if (mw > 0 && mh > 0) {
        float sc = fminf(1.0f, fminf(full_w / (float)mw, 300.0f / (float)mh));
        iw = clampf((float)mw * sc, 96.0f, full_w);
        ih = clampf((float)mh * sc, 72.0f, 300.0f);
      }
      if (draw) {
        ccord_rect ir = {tx, cy + 2.0f, iw, ih};
        ccord_draw_rrect(r, ir, 4.0f, COL(bg_tertiary));
        char sbuf[UI_URL_MAX];
        const char *sized = big_url;
        if (ui_media_sized_url(big_url, big_m->width, big_m->height, 800u, 600u, url_is_anim_image(big_url),
                               sbuf, sizeof sbuf)) {
          sized = sbuf;
        }
        const char *shown = media_pick(ui, r, sized, big_url);
        if (shown) {
          ccord_draw_texture_cover(r, shown, ir, 4.0f, 255, ui_media_animate(ui, ir));
          ui->dbg_attachment_textures_drawn++;
        }
        const char *one_big = big_url;
        record_img_hit(ui, ir, &one_big, 1u, 0u);
      }
      cy += ih + 6.0f;
    }
  }

  if (has_foot) {
    ccord_text_style fs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    float fh = ui_line_h(ui, fs.size);
    bool has_ficon = e->footer_icon && e->footer_icon[0];
    float row_h = maxf(fh, has_ficon ? 16.0f : 0.0f);
    if (draw) {
      float fx = tx;
      if (has_ficon) {
        ccord_rect fir = {fx, cy + (row_h - 16.0f) * 0.5f, 16.0f, 16.0f};
        ccord_draw_rrect(r, fir, 8.0f, COL(bg_tertiary));
        if (ui_ensure_media(ui, r, e->footer_icon)) {
          ccord_draw_texture_rounded(r, e->footer_icon, fir, 8.0f, 255);
          ui->dbg_attachment_textures_drawn++;
        }
        fx += 22.0f;
      }
      float rem_w = maxf(full_w - (fx - tx), 24.0f);
      char tbuf[48] = "";
      size_t tl = e->timestamp_ms > 0
                    ? ccord_timefmt_message(tbuf, sizeof tbuf, e->timestamp_ms, ui->now_unix_ms, ui->use_utc)
                    : 0u;
      float fy_txt = cy + (row_h - fh) * 0.5f;
      if (e->footer_text && e->footer_text[0]) {
        float reserve = tl > 0 ? (ccord_text_measure(r, tbuf, tl, &fs, 0.0f).x + 18.0f) : 0.0f;
        size_t ftl = 0;
        const char *ftext = embed_str_masked(ui, e->footer_text, emb_mask, sizeof emb_mask, draw, &ftl);
        float fw = ccord_text_draw_ellipsis(r, ftext, ftl, fx, fy_txt, maxf(rem_w - reserve, 24.0f), &fs);
        fx += fw;
        if (tl > 0) {
          ccord_vec2 dt = ccord_text_measure(r, "  -  ", 5, &fs, 0.0f);
          ccord_text_draw(r, "  -  ", 5, fx, fy_txt, &fs, 0.0f);
          fx += dt.x;
        }
      }
      if (tl > 0) {
        ccord_vec2 tw = ccord_text_measure(r, tbuf, tl, &fs, 0.0f);
        ccord_text_draw(r, tbuf, tl, fx, fy_txt, &fs, 0.0f);
        record_ts_hit(ui, (ccord_rect){fx, cy, tw.x, row_h}, e->timestamp_ms, false);
      }
    }
    cy += row_h + 2.0f;
  }

  if (has_thumb && cy < y + pad_v + thumb_h) cy = y + pad_v + thumb_h;
  if (cy <= y + pad_v) cy = y + pad_v + 24.0f;
  float final_h = (cy - y) + pad_v;
  if (cc && p->msg_id != CCORD_ID_NONE) {
    uintptr_t ep = (uintptr_t)e;
    size_t slot = 0;
    bool matched = false;
    for (size_t k = 0; k < CHAT_EMBEDH_CACHE_SLOTS; k++) {
      chat_embedh_entry *ce = &cc->embedh[k];
      if (ce->msg_id == p->msg_id && ce->edit_rev == p->edit_rev && ce->emb_ptr == ep && ce->width_q == emb_wq &&
          ce->scale_q == emb_sq && ce->streamer == emb_streamer) {
        slot = k;
        matched = true;
        break;
      }
      if (ce->msg_id == CCORD_ID_NONE) {
        slot = k;
        break;
      }
      if (ce->lru_tick < cc->embedh[slot].lru_tick) slot = k;
    }
    if (!matched) {
      cc->embedh[slot] = (chat_embedh_entry){
        .msg_id = p->msg_id,
        .edit_rev = p->edit_rev,
        .emb_ptr = ep,
        .width_q = emb_wq,
        .scale_q = emb_sq,
        .streamer = emb_streamer,
        .height = final_h,
        .lru_tick = ++cc->rh_tick,
      };
    } else {
      cc->embedh[slot].height = final_h;
      cc->embedh[slot].lru_tick = ++cc->rh_tick;
    }
  }
  return final_h;
}

static void draw_sticker_card(ccord_ui *ui, ccord_renderer *r, const ccord_msg_sticker *st, float x, float y)
{
  const char *name = st->name ? st->name : "";
  ccord_id id = st->id;
  uint8_t format_type = st->format ? st->format : 1u;
  if (id == CCORD_ID_NONE && ui->d.store && name[0]) {
    const ccord_sticker *known = ccord_store_find_sticker_by_name(ui->d.store, name);
    if (known) {
      id = known->id;
      format_type = known->format ? (uint8_t)known->format : 1u;
    }
  }
  ccord_rect box = {x, y + 2.0f, 160.0f, 160.0f};
  ui_expr_draw_sticker_art(ui, r, id, name, strlen(name), format_type, box, true);
}

static void draw_forward_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  float h = 46.0f;
  ccord_draw_rrect(r, (ccord_rect){x, y, w, h}, 4.0f, COL(bg_secondary));
  ccord_draw_rect(r, (ccord_rect){x, y + 4.0f, 2.0f, h - 8.0f}, COL(text_muted));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_normal));
  ccord_text_draw_ellipsis(r, "Forwarded", 9, x + 12.0f, y + 5.0f, w - 20.0f, &ls);
  if (p->text && p->len > 0) {
    ccord_text_draw_ellipsis(r, p->text, p->len, x + 12.0f, y + 22.0f, w - 20.0f, &ts);
  }
}

static void draw_poll_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  const ccord_poll *poll = p->poll;
  if (!poll) return;
  bool expired = (poll->expiry_ms > 0 && ui->now_unix_ms > 0 && ui->now_unix_ms >= poll->expiry_ms);
  bool closed = poll->finalized || expired;
  ccord_id ch_id = ccord_store_selected_channel(ui->d.store);
  float h = 52.0f + (float)poll->answer_count * DECOR_POLL_ROW_H;
  ccord_draw_rrect(r, (ccord_rect){x, y, w, h}, 6.0f, COL(bg_secondary));
  ccord_text_style qs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_style as = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
  ccord_text_style fs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, poll->question, strlen(poll->question), x + 12.0f, y + 8.0f, w - 24.0f, &qs);
  uint32_t total = 0;
  for (size_t i = 0; i < poll->answer_count; i++) total += poll->answers[i].votes;
  float ay = y + 32.0f;
  for (size_t i = 0; i < poll->answer_count; i++) {
    const ccord_poll_answer *a = &poll->answers[i];
    ccord_rect row = {x + 10.0f, ay, w - 20.0f, DECOR_POLL_ROW_H - 3.0f};
    bool row_hov = !closed && ui->mouse_in && !ui->menu.open && !ui->settings_open &&
                   !ui->select_open && ui->modal_kind == UI_MODAL_NONE && ui_in_rect(ui->mx, ui->my, row);
    ccord_draw_rrect(r, row, 4.0f, COL(bg_tertiary));
    if (!closed) {
      ui_state_fill(ui, r, row, 4.0f, "pollopt", p->msg_id ^ ((uint64_t)a->id << 32), row_hov, a->me_voted);
    }
    if (total > 0 && a->votes > 0) {
      ccord_color fill = a->me_voted ? COL(brand) : COL(text_muted);
      fill.a = 70;
      ccord_draw_rrect(r, (ccord_rect){row.x, row.y, row.w * ((float)a->votes / (float)total), row.h}, 4.0f, fill);
    }
    ccord_rect ind = {row.x + 7.0f, row.y + (row.h - 12.0f) * 0.5f, 12.0f, 12.0f};
    float rad = poll->multiselect ? 3.0f : 6.0f;
    if (a->me_voted) {
      ccord_draw_rrect(r, ind, rad, COL(brand));
      ccord_draw_rrect(r, (ccord_rect){ind.x + 3.5f, ind.y + 3.5f, 5.0f, 5.0f}, poll->multiselect ? 1.5f : 2.5f,
                       COL(text_on_brand));
    } else {
      ccord_draw_rrect(r, ind, rad, COL(text_muted));
      ccord_draw_rrect(r, (ccord_rect){ind.x + 1.5f, ind.y + 1.5f, 9.0f, 9.0f}, maxf(rad - 1.0f, 1.0f),
                       COL(bg_secondary));
    }
    char line[320];
    snprintf(line, sizeof line, "%s%s%s", a->emoji ? a->emoji : "", (a->emoji && a->emoji[0]) ? " " : "", a->text ? a->text : "");
    char votes[32];
    snprintf(votes, sizeof votes, "%u", (unsigned)a->votes);
    ccord_vec2 vw = ccord_text_measure(r, votes, strlen(votes), &as, 0.0f);
    ccord_text_draw_ellipsis(r, line, strlen(line), row.x + 25.0f, row.y + 3.0f, maxf(row.w - vw.x - 41.0f, 10.0f), &as);
    ccord_text_draw(r, votes, strlen(votes), row.x + row.w - vw.x - 8.0f, row.y + 3.0f, &as, 0.0f);
    if (ui->npoll_hits < UI_POLL_HIT_MAX) {
      ui->poll_hits[ui->npoll_hits++] = (ui_poll_hit){
        .rect = row, .channel_id = ch_id, .message_id = p->msg_id, .answer_id = a->id, .closed = closed,
      };
    }
    ay += DECOR_POLL_ROW_H;
  }
  char foot[96];
  const char *status_tag = poll->finalized ? "  -  Poll closed"
                           : (expired ? "  -  Poll expired" : (poll->multiselect ? "  -  Select one or more" : ""));
  snprintf(foot, sizeof foot, "%u vote%s%s", (unsigned)total, total == 1u ? "" : "s", status_tag);
  ccord_text_draw_ellipsis(r, foot, strlen(foot), x + 12.0f, y + h - 16.0f, w - 24.0f, &fs);
}

static bool comp_is_select(ccord_component_kind k)
{
  return k == CCORD_COMP_STRING_SELECT || k == CCORD_COMP_USER_SELECT ||
         k == CCORD_COMP_ROLE_SELECT || k == CCORD_COMP_MENTIONABLE_SELECT ||
         k == CCORD_COMP_CHANNEL_SELECT;
}

// The label a button shows (emoji + label, the link arrow); shared by the item walk and the Section accessory layout.
static size_t comp_button_text(const ccord_component *comp, char *out, size_t cap)
{
  const char *lbl = (comp->label && comp->label[0]) ? comp->label
                    : (comp->style == CCORD_BTN_PREMIUM ? "Upgrade" : "");
  const char *emj = (comp->emoji && comp->emoji[0]) ? comp->emoji : "";
  int n;
  if (emj[0] && lbl[0]) {
    n = snprintf(out, cap, "%s %s%s", emj, lbl, comp->style == CCORD_BTN_LINK ? " \xE2\x86\x97" : "");
  } else if (lbl[0] || emj[0]) {
    n = snprintf(out, cap, "%s%s", lbl[0] ? lbl : emj, comp->style == CCORD_BTN_LINK ? " \xE2\x86\x97" : "");
  } else {
    n = snprintf(out, cap, "%s", comp->style == CCORD_BTN_LINK ? "Open \xE2\x86\x97" : "Button");
  }
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap - 1u;
}

static float comp_button_w(ccord_ui *ui, ccord_renderer *r, const ccord_component *comp, float max_w)
{
  char btxt[220];
  size_t bl = comp_button_text(comp, btxt, sizeof btxt);
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
  ccord_vec2 bsz = ccord_text_measure(r, btxt, bl, &bs, 0.0f);
  return clampf(bsz.x + 24.0f, 56.0f, max_w);
}

// One row of buttons and select menus. v1 action rows and the rows / Section accessories of a Components v2 tree share
// it; a v2 item is found again by its custom_id (there is no v1 row index), a v1 item by row + column.
static float comp_items_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p,
                             const ccord_component *items, size_t nitems,
                             float x, float y, float w, bool draw, bool v2)
{
  if (!p || !p->rich || nitems == 0) return 0.0f;
  ccord_id ch_id = ccord_store_selected_channel(ui->d.store);
  ccord_id app_id = p->rich->application_id != CCORD_ID_NONE ? p->rich->application_id : p->author_id;
  float max_w = maxf(w, 60.0f);
  float cx = x;
  float cy = y;
  float row_h = 0.0f;
  ccord_text_style bs_base = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
  for (size_t ci = 0; ci < nitems; ci++) {
    const ccord_component *comp = &items[ci];
    const char *cid = comp->custom_id ? comp->custom_id : "";
    uint64_t state_key = v2 ? (p->msg_id ^ ui_name_id(cid))
                            : (p->msg_id ^ ((uint64_t)p->row_idx << 40) ^ ((uint64_t)ci << 48));
    if (comp->kind == CCORD_COMP_BUTTON) {
      char btxt[220];
      size_t bl = comp_button_text(comp, btxt, sizeof btxt);
      ccord_vec2 bsz = ccord_text_measure(r, btxt, bl, &bs_base, 0.0f);
      float bw = clampf(bsz.x + 24.0f, 56.0f, max_w);
      float bh = 32.0f;
      if (cx > x && cx + bw > x + max_w) {
        cx = x;
        cy += row_h + 6.0f;
        row_h = 0.0f;
      }
      ccord_rect br = {cx, cy, bw, bh};
      if (draw) {
        ui->dbg_component_buttons++;
        ccord_color bg = COL(brand);
        ccord_color fg = COL(text_on_brand);
        switch ((ccord_button_style)comp->style) {
          case CCORD_BTN_PRIMARY: bg = COL(brand); fg = COL(text_on_brand); break;
          case CCORD_BTN_SECONDARY: bg = COL(bg_tertiary); fg = COL(text_normal); break;
          case CCORD_BTN_SUCCESS: bg = COL(status_online); fg = COL(text_on_brand); break;
          case CCORD_BTN_DANGER: bg = COL(danger); fg = COL(text_on_brand); break;
          case CCORD_BTN_LINK: bg = COL(bg_tertiary); fg = COL(text_normal); break;
          case CCORD_BTN_PREMIUM: bg = COL(brand); fg = COL(text_on_brand); break;
        }
        if (comp->disabled) {
          bg = ui_alpha(bg, 0.5f);
          fg = ui_alpha(fg, 0.5f);
        }
        ccord_draw_rrect(r, br, 4.0f, bg);
        if (!comp->disabled) {
          bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && !ui->select_open &&
                     ui->modal_kind == UI_MODAL_NONE && ui_in_rect(ui->mx, ui->my, br);
          ui_state_fill(ui, r, br, 4.0f, "compbtn", state_key, hov, false);
        }
        ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), fg);
        ccord_text_draw_ellipsis(r, btxt, bl, br.x + maxf((br.w - bsz.x) * 0.5f, 8.0f),
                                 br.y + (br.h - bsz.y) * 0.5f, maxf(br.w - 16.0f, 8.0f), &bst);
        if (ui->ncomp_hits < UI_COMP_HIT_MAX) {
          ui_comp_hit *hit = &ui->comp_hits[ui->ncomp_hits++];
          *hit = (ui_comp_hit){
            .rect = br,
            .channel_id = ch_id,
            .message_id = p->msg_id,
            .application_id = app_id,
            .message_flags = p->rich->flags,
            .kind = comp->kind,
            .style = comp->style,
            .min_values = comp->min_values,
            .max_values = comp->max_values,
            .disabled = comp->disabled,
            .custom_id = ui_hit_cstr(ui, comp->custom_id),
            .url = (comp->url && strlen(comp->url) < UI_URL_MAX) ? ui_hit_cstr(ui, comp->url) : "",
            .placeholder = "",
            .row_idx = v2 ? SIZE_MAX : p->row_idx,
            .comp_idx = ci,
          };
        }
      }
      cx += bw + 8.0f;
      if (bh > row_h) row_h = bh;
    } else if (comp_is_select(comp->kind)) {
      if (cx > x) {
        cx = x;
        cy += row_h + 6.0f;
        row_h = 0.0f;
      }
      float sw = fminf(max_w, 400.0f);
      float sh = 36.0f;
      ccord_rect sr = {cx, cy, sw, sh};
      if (draw) {
        ui->dbg_component_selects++;
        char sum[192];
        sum[0] = '\0';
        bool has_sel = false;
        bool target_open = ui->select_open && ui->select_target && ui->select_opts &&
                           ui->select_target->message_id == p->msg_id &&
                           (v2 ? strcmp(ui->select_target->custom_id, cid) == 0
                               : (ui->select_target->row_idx == p->row_idx && ui->select_target->comp_idx == ci));
        if (target_open) {
          size_t slen = 0;
          for (size_t oi = 0; oi < ui->select_opt_count; oi++) {
            if (!ui->select_opts[oi].selected) continue;
            has_sel = true;
            const char *ol = ui->select_opts[oi].label;
            int n = snprintf(sum + slen, sizeof sum - slen, "%s%s", slen > 0 ? ", " : "", ol);
            if (n > 0) slen += (size_t)n < sizeof sum - slen ? (size_t)n : sizeof sum - slen - 1u;
          }
        } else if (comp->kind == CCORD_COMP_STRING_SELECT) {
          size_t slen = 0;
          for (size_t oi = 0; oi < comp->option_count; oi++) {
            if (!comp->options[oi].default_) continue;
            has_sel = true;
            const char *ol = comp->options[oi].label ? comp->options[oi].label : "";
            int n = snprintf(sum + slen, sizeof sum - slen, "%s%s", slen > 0 ? ", " : "", ol);
            if (n > 0) slen += (size_t)n < sizeof sum - slen ? (size_t)n : sizeof sum - slen - 1u;
          }
        }
        const char *ph = (comp->placeholder && comp->placeholder[0]) ? comp->placeholder : "Make a selection";
        const char *shown = has_sel && sum[0] ? sum : ph;
        ccord_draw_rrect(r, (ccord_rect){sr.x - 1.0f, sr.y - 1.0f, sr.w + 2.0f, sr.h + 2.0f}, 5.0f, COL(border_subtle));
        ccord_draw_rrect(r, sr, 4.0f, comp->disabled ? ui_alpha(COL(bg_tertiary), 0.5f) : COL(bg_tertiary));
        if (!comp->disabled) {
          bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && !ui->select_open &&
                     ui->modal_kind == UI_MODAL_NONE && ui_in_rect(ui->mx, ui->my, sr);
          ui_state_fill(ui, r, sr, 4.0f, "compsel", state_key, hov, false);
        }
        ccord_color tc = has_sel ? COL(text_normal) : COL(text_placeholder);
        if (comp->disabled) tc = ui_alpha(tc, 0.5f);
        ccord_text_style sst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), tc);
        ccord_text_style cst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small),
                                        comp->disabled ? ui_alpha(COL(text_muted), 0.5f) : COL(text_muted));
        float lh = ui_line_h(ui, sst.size);
        ccord_text_draw_ellipsis(r, shown, strlen(shown), sr.x + 12.0f, sr.y + (sr.h - lh) * 0.5f,
                                 maxf(sr.w - 38.0f, 12.0f), &sst);
        ccord_text_draw(r, "v", 1, sr.x + sr.w - 20.0f, sr.y + (sr.h - lh) * 0.5f, &cst, 0.0f);
        if (ui->ncomp_hits < UI_COMP_HIT_MAX) {
          ui_comp_hit *hit = &ui->comp_hits[ui->ncomp_hits++];
          *hit = (ui_comp_hit){
            .rect = sr,
            .channel_id = ch_id,
            .message_id = p->msg_id,
            .application_id = app_id,
            .message_flags = p->rich->flags,
            .kind = comp->kind,
            .style = comp->style,
            .min_values = comp->min_values,
            .max_values = comp->max_values > 0 ? comp->max_values : 1u,
            .disabled = comp->disabled,
            .custom_id = ui_hit_cstr(ui, comp->custom_id),
            .url = "",
            .placeholder = ui_hit_cstr(ui, comp->placeholder),
            .row_idx = v2 ? SIZE_MAX : p->row_idx,
            .comp_idx = ci,
          };
        }
      }
      cy += sh + 6.0f;
      cx = x;
      row_h = 0.0f;
    }
  }
  float total_h = (cy - y) + row_h;
  if (cy > y && row_h == 0.0f && total_h >= 6.0f) total_h -= 6.0f;
  return total_h;
}

static float components_row_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p,
                                 float x, float y, float w, bool draw)
{
  if (!p || !p->rich || p->row_idx >= p->rich->component_row_count) return 0.0f;
  const ccord_component_row *row = &p->rich->components[p->row_idx];
  if (row->component_count == 0) return 0.0f;
  if (draw) ui->dbg_component_rows++;
  return comp_items_walk(ui, r, p, row->components, row->component_count, x, y, w, draw, false);
}

// ---------------------------------------------------------------- components v2 (message flag IS_COMPONENTS_V2)

// Spoiler reveal keys of v2 nodes: a space apart from the attachment keys, addressed by the node's tree path (child i
// of a node is parent * 64 + i + 1, so a node never shares its path with its parent; a validated tree is at most 4
// levels of at most 40 nodes, so the path stays unique).
static ccord_id cv2_spoiler_key(ccord_id msg_id, size_t path)
{
  return (msg_id ^ 0xC200000000000000ULL) + (ccord_id)(path + 1u);
}

// A Thumbnail: an aspect-fit image in a square box, with its own spoiler tile. Returns the drawn height.
static float cv2_thumbnail(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_media_ref *media,
                           float x, float y, float box, bool draw, ccord_id skey)
{
  float tw = box, th = box;
  if (media->width > 0 && media->height > 0) {
    float sc = fminf(1.0f, fminf(box / (float)media->width, box / (float)media->height));
    tw = clampf((float)media->width * sc, 24.0f, box);
    th = clampf((float)media->height * sc, 24.0f, box);
  }
  if (!draw) return th;
  ccord_rect tr = {x, y, tw, th};
  if (media->spoiler && !is_spoiler_revealed(ui, skey)) {
    draw_spoiler_tile(ui, r, tr, skey);
    return th;
  }
  const char *url = ccord_rich_media_url(p->rich, media);
  ccord_draw_rrect(r, tr, 4.0f, COL(bg_tertiary));
  if (url[0] && ui_ensure_media(ui, r, url)) {
    ccord_draw_texture_rounded_anim(r, url, tr, 4.0f, 255, ui_media_animate(ui, tr));
    ui->dbg_attachment_textures_drawn++;
  }
  if (url[0]) {
    const char *one_url = url;
    record_img_hit(ui, tr, &one_url, 1u, 0u);
  }
  return th;
}

#define CV2_MEMO_MAX 128u
typedef struct cv2_memo {
  float h[CV2_MEMO_MAX];
  uint16_t end_idx[CV2_MEMO_MAX];
  size_t count;
  size_t cursor;
  bool valid;
} cv2_memo;

static float cv2_node(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_component *n,
                      float x, float y, float w, bool draw, size_t path, unsigned depth, cv2_memo *memo);

// One node of the tree and everything under it, laid out (draw=false) or drawn. `path` addresses the node for spoiler
// keys; children continue it. When `memo` is provided, a single O(N) measure pass populates every container/node height
// in pre-order so drawing nested containers never re-measures subtrees (avoiding O(2^D) walks).
static float cv2_node_layout(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_component *n,
                             float x, float y, float w, bool draw, size_t path, unsigned depth, cv2_memo *memo)
{
  if (!n || depth > 8u) return 0.0f;   // a validated tree never gets this deep
  size_t my_idx = SIZE_MAX;
  if (memo) {
    if (!draw) {
      if (memo->count < CV2_MEMO_MAX) {
        my_idx = memo->count++;
      } else {
        memo->valid = false;
      }
    } else if (memo->valid) {
      my_idx = memo->cursor++;
    }
  }
  float res = 0.0f;
  switch (n->kind) {
    case CCORD_COMP_CONTAINER: {
      float pad_l = 12.0f, pad_r = 10.0f, pad_v = 10.0f, gap = 6.0f;
      float inner_w = maxf(w - pad_l - pad_r, 48.0f);
      float h = 0.0f;
      if (draw && memo && memo->valid && my_idx < memo->count) {
        h = memo->h[my_idx];
      } else {
        float ch = 0.0f;
        for (size_t i = 0; i < n->child_count; i++) {
          ch += cv2_node(ui, r, p, &n->children[i], x + pad_l, y + pad_v + ch, inner_w, false,
                         path * 64u + i + 1u, depth + 1u, draw ? nullptr : memo) + gap;
        }
        if (ch > 0.0f) ch -= gap;
        h = ch + 2.0f * pad_v;
      }
      ccord_id skey = cv2_spoiler_key(p->msg_id, path);
      if (n->spoiler && !is_spoiler_revealed(ui, skey)) {
        if (draw) {
          draw_spoiler_tile(ui, r, (ccord_rect){x, y, w, h}, skey);
          if (memo && memo->valid && my_idx < memo->count) memo->cursor = memo->end_idx[my_idx];
        }
        res = h;
        break;
      }
      if (draw) {
        ccord_draw_rrect(r, (ccord_rect){x, y, w, h}, 6.0f, COL(bg_secondary));
        if (n->has_accent_color) {
          ccord_draw_rrect(r, (ccord_rect){x, y, 4.0f, h}, 2.0f, ui_rgb_color(n->accent_color));
        } else {
          ccord_draw_rect(r, (ccord_rect){x, y, w, 1.0f}, COL(border_subtle));
          ccord_draw_rect(r, (ccord_rect){x, y + h - 1.0f, w, 1.0f}, COL(border_subtle));
        }
        float cy = y + pad_v;
        for (size_t i = 0; i < n->child_count; i++) {
          cy += cv2_node(ui, r, p, &n->children[i], x + pad_l, cy, inner_w, true, path * 64u + i + 1u,
                         depth + 1u, memo) + gap;
        }
      }
      res = h;
      break;
    }
    case CCORD_COMP_SECTION: {
      // 1-3 TextDisplay children beside an accessory (a Thumbnail or a Button) pinned to the right edge.
      const ccord_component *acc = n->accessory;
      size_t acc_path = path * 64u + n->child_count + 1u;
      float gap = 12.0f, acc_w = 0.0f, acc_h = 0.0f;
      if (acc && acc->kind == CCORD_COMP_BUTTON) {
        acc_w = comp_button_w(ui, r, acc, maxf(w * 0.5f, 56.0f));
      } else if (acc) {
        acc_w = 80.0f;
      }
      float text_w = maxf(w - (acc_w > 0.0f ? acc_w + gap : 0.0f), 48.0f);
      float ty = y;
      for (size_t i = 0; i < n->child_count; i++) {
        if (n->children[i].kind != CCORD_COMP_TEXT_DISPLAY) continue;
        ty += cv2_node(ui, r, p, &n->children[i], x, ty, text_w, draw, path * 64u + i + 1u, depth + 1u, memo) + 2.0f;
      }
      float th = ty > y ? ty - y - 2.0f : 0.0f;
      if (acc) {
        acc_h = cv2_node(ui, r, p, acc, x + w - acc_w, y, acc_w, draw, acc_path, depth + 1u, memo);
      }
      res = maxf(th, acc_h);
      break;
    }
    case CCORD_COMP_TEXT_DISPLAY: {
      const char *txt = n->content ? n->content : "";
      ccord_id prev_id = ui->current_drawing_msg_id;
      ui->current_drawing_msg_id = p->msg_id;
      res = md_body_walk(ui, r, txt, strlen(txt), x, y, w, COL(text_normal), draw);
      ui->current_drawing_msg_id = prev_id;
      break;
    }
    case CCORD_COMP_BUTTON:   // a Section accessory
      res = comp_items_walk(ui, r, p, n, 1u, x, y, w, draw, true);
      break;
    case CCORD_COMP_THUMBNAIL:
      res = cv2_thumbnail(ui, r, p, &n->media, x, y, 80.0f, draw, cv2_spoiler_key(p->msg_id, path));
      break;
    case CCORD_COMP_MEDIA_GALLERY: {
      ccord_attachment tmp[CCORD_RICH_MAX_V2_GALLERY_ITEMS];
      memset(tmp, 0, sizeof tmp);
      size_t cnt = n->gallery_count < CCORD_RICH_MAX_ATTACHMENTS ? n->gallery_count : CCORD_RICH_MAX_ATTACHMENTS;
      for (size_t k = 0; k < cnt; k++) {
        const ccord_media_ref *g = &n->gallery[k];
        tmp[k].url = ccord_rich_media_url(p->rich, g);
        tmp[k].content_type = (g->content_type && g->content_type[0]) ? g->content_type : "image/png";
        tmp[k].width = g->width;
        tmp[k].height = g->height;
        tmp[k].spoiler = g->spoiler;
        // A per-item reveal key (spoiler_att_key flips the top bit, so pre-flip it).
        tmp[k].id = cv2_spoiler_key(p->msg_id, path * 64u + k + 1u) ^ (1ULL << 63);
      }
      decor_part gp = {.kind = DECOR_GALLERY, .att = tmp, .count = cnt, .msg_id = p->msg_id};
      res = gallery_walk(ui, r, &gp, x, y, w, draw);
      if (draw) {
        ui->dbg_gallery_cards++;
        ui->dbg_attachment_cards += cnt;
      }
      break;
    }
    case CCORD_COMP_FILE: {
      ccord_attachment fa = {0};
      fa.filename = n->file_name;
      fa.size = n->file_size;
      fa.url = ccord_rich_media_url(p->rich, &n->media);
      fa.content_type = n->media.content_type;
      fa.spoiler = n->spoiler;
      ccord_id skey = cv2_spoiler_key(p->msg_id, path);
      if (fa.spoiler && !is_spoiler_revealed(ui, skey)) {
        if (draw) draw_spoiler_tile(ui, r, (ccord_rect){x, y, w, 46.0f}, skey);
        res = 46.0f;
        break;
      }
      if (draw) {
        draw_attachment_card(ui, r, &fa, x, y, w);
        ui->dbg_attachment_cards++;
      }
      res = 46.0f;
      break;
    }
    case CCORD_COMP_SEPARATOR: {
      float h = n->spacing == 2u ? 16.0f : 8.0f;
      if (draw && n->divider) ccord_draw_rect(r, (ccord_rect){x, y + h * 0.5f, w, 1.0f}, COL(border_subtle));
      res = h;
      break;
    }
    case CCORD_COMP_ACTION_ROW:
      if (draw) ui->dbg_component_rows++;
      res = comp_items_walk(ui, r, p, n->children, n->child_count, x, y, w, draw, true);
      break;
    default:
      res = 0.0f;   // a kind this build does not know draws nothing
      break;
  }
  if (memo && !draw && my_idx < CV2_MEMO_MAX) {
    memo->h[my_idx] = res;
    memo->end_idx[my_idx] = (uint16_t)memo->count;
  }
  return res;
}

// cv2_node_layout plus the debug tally of what was drawn per kind (count and the last height), read by the UI tests.
static float cv2_node(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_component *n,
                      float x, float y, float w, bool draw, size_t path, unsigned depth, cv2_memo *memo)
{
  float h = cv2_node_layout(ui, r, p, n, x, y, w, draw, path, depth, memo);
  if (draw && n && (size_t)n->kind < UI_CV2_KIND_SLOTS) {
    ui->dbg_cv2_drawn[n->kind]++;
    ui->dbg_cv2_height[n->kind] = h;
  }
  return h;
}

// The whole v2 tree of one message: its top-level nodes stacked with a small gap.
static float cv2_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw)
{
  if (!p || !p->rich || p->rich->v2_component_count == 0) return 0.0f;
  const ccord_component *nodes = p->rich->v2_components;
  if (!draw) {
    float cy = y;
    for (size_t i = 0; i < p->rich->v2_component_count; i++) {
      cy += cv2_node(ui, r, p, &nodes[i], x, cy, w, false, i + 1u, 1u, nullptr) + 6.0f;
    }
    return cy - y - 6.0f;
  }
  // Pre-measure once in O(N) so drawing nested containers never re-measures subtrees.
  cv2_memo memo = {.valid = true};
  bool has_container = false;
  for (size_t i = 0; i < p->rich->v2_component_count; i++) {
    if (nodes[i].kind == CCORD_COMP_CONTAINER) {
      has_container = true;
      break;
    }
  }
  if (has_container) {
    float my = y;
    for (size_t i = 0; i < p->rich->v2_component_count; i++) {
      my += cv2_node(ui, r, p, &nodes[i], x, my, w, false, i + 1u, 1u, &memo) + 6.0f;
    }
  }
  float cy = y;
  cv2_memo *mp = (has_container && memo.valid) ? &memo : nullptr;
  for (size_t i = 0; i < p->rich->v2_component_count; i++) {
    cy += cv2_node(ui, r, p, &nodes[i], x, cy, w, true, i + 1u, 1u, mp) + 6.0f;
  }
  return cy - y - 6.0f;
}

static void draw_thread_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, ccord_id channel_id,
                             float x, float y, float w)
{
  if (!p || !p->rich || p->rich->thread.id == CCORD_ID_NONE) return;
  ccord_id tid = p->rich->thread.id;
  const ccord_channel *tc = ccord_store_channel(ui->d.store, tid);
  const ccord_channel *pch = ccord_store_channel(ui->d.store, channel_id);
  const char *tname = (tc && tc->name && tc->name[0]) ? tc->name
                      : ((p->rich->thread.name && p->rich->thread.name[0]) ? p->rich->thread.name : "Thread");
  uint32_t mc = p->rich->thread.message_count;
  if (tc && tc->thread_message_count > mc) mc = tc->thread_message_count;
  bool archived = tc && tc->thread_archived;

  float cw = w < 420.0f ? w : 420.0f;
  ccord_rect card = {x, y, cw, 42.0f};
  bool hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
  ccord_draw_rrect(r, card, 6.0f, hover ? COL(bg_item_hover) : COL(bg_secondary));
  ccord_draw_rrect(r, (ccord_rect){card.x, card.y, 3.0f, card.h}, 2.0f, COL(brand));

  ui_icon_hash(r, card.x + 16.0f, card.y + 14.0f, 14.0f, COL(brand));
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw_ellipsis(r, tname, strlen(tname), card.x + 28.0f, card.y + 6.0f, card.w - 40.0f, &ns);

  char sub[96];
  if (archived) {
    snprintf(sub, sizeof sub, "%u Message%s • Archived  >", (unsigned)mc, mc == 1u ? "" : "s");
  } else {
    snprintf(sub, sizeof sub, "%u Message%s  >", (unsigned)mc, mc == 1u ? "" : "s");
  }
  ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(brand));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), card.x + 28.0f, card.y + 23.0f, card.w - 40.0f, &ss);

  if (ui->nthread_hits < UI_THREAD_HIT_MAX) {
    ui_thread_hit *th = &ui->thread_hits[ui->nthread_hits++];
    th->rect = card;
    th->guild_id = pch ? pch->guild_id : ccord_store_selected_guild(ui->d.store);
    th->parent_id = channel_id;
    th->thread_id = tid;
    snprintf(th->name, sizeof th->name, "%.99s", tname);
  }
}

static void draw_invite_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  if (!p || p->invite_code[0] == '\0') return;
  bool loading = false, failed = false;
  const ccord_invite_preview *prev = ui->d.session
    ? ccord_session_invite_lookup(ui->d.session, p->invite_code, &loading, &failed)
    : nullptr;
  if (!prev && !loading && !failed && ui->d.session) {
    ccord_error err;
    if (ccord_session_invite_fetch(ui->d.session, p->invite_code, &err) == CCORD_OK) loading = true;
  }
  bool joined = prev && prev->guild_id != CCORD_ID_NONE &&
                ui->d.store && ccord_store_guild(ui->d.store, prev->guild_id) != nullptr;
  ccord_id self = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;

  float cw = w < 420.0f ? w : 420.0f;
  ccord_rect card = {x, y, cw, 88.0f};
  uis_plate(ui, r, card, 6.0f, COL(bg_secondary));

  const char *hdr = (self != CCORD_ID_NONE && p->author_id == self)
                      ? "YOU SENT AN INVITE TO JOIN A SERVER"
                      : "YOU'VE BEEN INVITED TO JOIN A SERVER";
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, hdr, strlen(hdr), card.x + 12.0f, card.y + 8.0f, card.w - 24.0f, &hs);

  ccord_rect ic = {card.x + 12.0f, card.y + 28.0f, 46.0f, 46.0f};
  if (prev && prev->guild_icon_url[0] && ui_ensure_media(ui, r, prev->guild_icon_url)) {
    ccord_draw_texture_rounded(r, prev->guild_icon_url, ic, 12.0f, 255);
  } else {
    uis_plate(ui, r, ic, 12.0f, failed ? COL(bg_tertiary) : COL(brand));
    char init[8] = "!";
    if (prev && prev->guild_name[0]) ui_initials(prev->guild_name, init);
    else if (!failed) snprintf(init, sizeof init, "?");
    ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_on_brand));
    ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
    ccord_text_draw(r, init, strlen(init), ic.x + (ic.w - it.x) * 0.5f, ic.y + (ic.h - it.y) * 0.5f, &ist, 0.0f);
  }

  float btn_w = joined ? 78.0f : 72.0f;
  float btn_h = 34.0f;
  ccord_rect btn = {card.x + card.w - btn_w - 12.0f, card.y + 34.0f, btn_w, btn_h};
  float tx = ic.x + ic.w + 12.0f;
  float tw = btn.x - tx - 10.0f;
  if (tw < 60.0f) tw = 60.0f;

  const char *title = prev ? prev->guild_name : (failed ? "Invalid Invite" : "Loading invite...");
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base),
                                 failed ? COL(danger) : COL(text_header));
  ccord_text_draw_ellipsis(r, title, strlen(title), tx, card.y + 31.0f, tw, &ts);

  char sub[96];
  if (prev) {
    snprintf(sub, sizeof sub, "%u Online  •  %u Members", (unsigned)prev->online_count, (unsigned)prev->member_count);
  } else if (failed) {
    snprintf(sub, sizeof sub, "This invite may be expired or invalid");
  } else {
    snprintf(sub, sizeof sub, "Fetching server invite...");
  }
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), tx, card.y + 52.0f, tw, &ss);

  if (!failed) {
    bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui->modal_kind == UI_MODAL_NONE &&
               ui_in_rect(ui->mx, ui->my, btn);
    const char *blbl = joined ? "Joined" : "Join";
    uis_button(ui, r, btn, blbl, joined ? UIS_BTN_SECONDARY : UIS_BTN_PRIMARY, UIS_BTN_S_SM,
               (uis_state){.hover = hov}, "invjoin", p->msg_id);
    if (ui->ninvite_hits < UI_INVITE_HIT_MAX) {
      size_t idx = ui->ninvite_hits++;
      ui->invite_hits[idx].rect = card;
      ui->invite_hits[idx].btn_rect = btn;
      ui->invite_hits[idx].guild_id = prev ? prev->guild_id : CCORD_ID_NONE;
      snprintf(ui->invite_hits[idx].code, sizeof ui->invite_hits[idx].code, "%s", p->invite_code);
      ui->invite_hits[idx].joined = joined;
    }
  } else {
    btn = (ccord_rect){0};
  }
}

static float draw_decor_cards(ccord_ui *ui, ccord_renderer *r, const decor_info *d, float x, float y, float w)
{
  float y0 = y;
  for (size_t i = 0; i < d->nparts; i++) {
    const decor_part *p = &d->parts[i];
    float ph = 0.0f;
    switch (p->kind) {
      case DECOR_REPLY:   // drawn by draw_row as a line above the header
        continue;
      case DECOR_ATTACHMENT:
        draw_attachment_card(ui, r, p->att, x, y, w);
        ui->dbg_attachment_cards++;
        ph = 46.0f;
        break;
      case DECOR_GALLERY:
        ph = gallery_walk(ui, r, p, x, y, w, true);
        ui->dbg_gallery_cards++;
        ui->dbg_attachment_cards += p->count;
        break;
      case DECOR_MEDIA_PLAYER:
        ph = draw_media_player_card(ui, r, p, x, y, w);
        ui->dbg_media_player_cards++;
        ui->dbg_attachment_cards++;
        break;
      case DECOR_EMBED:
        ph = embed_card_walk(ui, r, p, x, y, w, true);
        ui->dbg_embed_cards += p->count;
        break;
      case DECOR_STICKER:
        draw_sticker_card(ui, r, p->stk, x, y);
        ui->dbg_sticker_cards++;
        ph = 164.0f;
        break;
      case DECOR_FORWARD:
        draw_forward_card(ui, r, p, x, y, w);
        ui->dbg_forward_cards++;
        ph = 46.0f;
        break;
      case DECOR_POLL:
        draw_poll_card(ui, r, p, x, y, w);
        ui->dbg_poll_cards++;
        ph = 52.0f + (p->poll ? (float)p->poll->answer_count : 0.0f) * DECOR_POLL_ROW_H;
        break;
      case DECOR_COMPONENTS:
        ph = components_row_walk(ui, r, p, x, y, w, true);
        break;
      case DECOR_COMPONENTS_V2:
        ph = cv2_walk(ui, r, p, x, y, w, true);
        break;
      case DECOR_THREAD:
        draw_thread_card(ui, r, p, d->channel_id, x, y, w);
        ui->dbg_thread_cards++;
        ph = 42.0f;
        break;
      case DECOR_INVITE:
        draw_invite_card(ui, r, p, x, y, w);
        ui->dbg_invite_cards++;
        ph = 88.0f;
        break;
    }
    y += ph + 4.0f;
  }
  return y - y0;
}

static void draw_row(msg_ctx *c, size_t i, float y, float h, const ccord_rect *ma,
                     ccord_rect *out_avatar_rect, ccord_rect *out_author_rect)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  if (out_avatar_rect) *out_avatar_rect = (ccord_rect){0, 0, 0, 0};
  if (out_author_rect) *out_author_rect = (ccord_rect){0, 0, 0, 0};
  size_t store_i = 0;
  const ccord_message *m = view_message_at(ui, c->channel, i, &store_i);
  if (!m) return;
  if (is_dm_intro_row(ui, c->channel, store_i)) {
    draw_dm_intro(ui, r, ccord_store_channel(ui->d.store, c->channel),
                  ma->x + M(message_pad_x), y, ma->w - 2.0f * M(message_pad_x));
    y += UI_DM_INTRO_H;
    h -= UI_DM_INTRO_H;
  }
  bool has_date = store_i != SIZE_MAX && ui_row_has_date_divider(ui, c->channel, store_i, m);
  bool has_new = store_i != SIZE_MAX && ui_row_has_new_divider(ui, c->channel, store_i, m);
  if (has_date || has_new) {
    draw_chat_divider(ui, r, ma->x + M(message_pad_x), y, ma->w - 2.0f * M(message_pad_x),
                      m->timestamp_ms, has_date, has_new);
    if (has_date) ui->dbg_date_dividers++;
    if (has_new) ui->dbg_new_dividers++;
    y += UI_CHAT_DIVIDER_H;
    h -= UI_CHAT_DIVIDER_H;
  }
  blocked_run br = blocked_run_at(ui, c->channel, store_i, m);
  if (br.blocked) {
    if (br.first) {
      draw_blocked_strip(ui, r, ma, y, &br);
      y += UI_BLOCKED_ROW_H;
      h -= UI_BLOCKED_ROW_H;
    }
    if (!br.revealed) return;
    ui->dbg_blocked_text_drawn++;
  }
  bool compact = store_i != SIZE_MAX && ui_is_compact(ui, c->channel, store_i, m);
  decor_info d;
  decor_scan(ui, m, compact, &d);
  bool menu_target = ui->menu.open && ui->menu.ctx.target == CCORD_MENU_MESSAGE && ui->menu.ctx.message_id == m->id;
  bool hover = menu_target || (m->id == ui->hover_message_id && ui->mouse_in && !ui->settings_open && !ui->menu.open);
  float lh = ui_line_h(ui, M(font_size_base));
  bool dense = ui_messages_compact(ui);
  float top = compact ? M(message_line_gap)
                      : (dense ? M(message_line_gap) + (M(message_group_gap) - M(message_line_gap)) * 0.35f
                               : M(message_group_gap));
  float reply_y = y + top;   // a reply's reference line takes the top of the row, the header follows it
  top += decor_reply_h(&d);
  float skip = compact ? 0.0f : M(message_group_gap) * 0.5f;
  float row_h_vis = h - skip - tail_pad(ui, i);
  bool ephem = ccord_message_is_ephemeral(m);
  if (ephem) {
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(brand), 0.08f));
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, 2.5f, row_h_vis}, COL(brand));
  } else if (message_mentions_self(ui, m)) {
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(status_idle), 0.10f));
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, 2.0f, row_h_vis}, COL(status_idle));
  }
  if (ui->jump_highlight_msg_id == m->id && ui->now_ms < ui->jump_highlight_until_ms) {
    float flash = jump_flash_strength(ui);
    ui->dbg_jump_flash = flash;
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(brand), 0.22f * flash));
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, 2.0f, row_h_vis}, ui_alpha(COL(brand), flash));
  }
  float row_hv = ui_fade(ui, "msg", m->id, hover);   // the row's wash and its action bar ease in and out with the pointer
  if (row_hv > 0.004f) {
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(bg_message_hover), row_hv));
  }
  float text_x = ma->x + M(message_text_x);
  float content_y = y + top;
  ccord_id guild = ccord_store_selected_guild(ui->d.store);
  if (decor_reply_h(&d) > 0.0f) {
    // The connector rises from the top of the avatar column; a grouped or dense row has no avatar, so it stays a stub.
    float sx = ma->x + M(message_pad_x) + M(avatar_size) * 0.5f;
    float y1 = (!compact && !dense) ? y + top + 2.0f : reply_y + UI_REPLY_LINE_H;
    draw_reply_line(ui, r, &d, guild, sx, y1, text_x, reply_y,
                    maxf(ma->w - M(message_text_x) - M(message_pad_x), 40.0f));
  }
  if (!compact) {
    const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
    const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, m->author_id);
    const char *name = (u && u->name && u->name[0]) ? u->name
                       : ((prof && prof->display_name && prof->display_name[0]) ? prof->display_name : "Unknown User");
    if (guild != CCORD_ID_NONE && guild != CCORD_DM_GUILD_ID) {
      const ccord_member *gm = ccord_store_find_member(ui->d.store, guild, m->author_id);
      if (gm && gm->nick[0] != '\0') name = gm->nick;
    }
    ccord_user row_u = {
      .id = m->author_id,
      .name = name,
      .handle = (u && u->handle && u->handle[0]) ? u->handle : ((prof && prof->handle) ? prof->handle : ""),
      .status_text = (u && u->status_text) ? u->status_text : "",
      .avatar_url = (u && u->avatar_url && u->avatar_url[0]) ? u->avatar_url
                    : ((prof && prof->avatar_url) ? prof->avatar_url : ""),
      .presence = u ? u->presence : CCORD_PRESENCE_OFFLINE,
      .is_bot = u ? u->is_bot : false,
      .avatar_palette = u ? u->avatar_palette : (uint8_t)(m->author_id % 6u),
    };
    if (!dense) {
      float ax = ma->x + M(message_pad_x);
      float ay = y + top + 2.0f;
      float asz = M(avatar_size);
      ui_draw_avatar(ui, r, &row_u, ax + asz * 0.5f, ay + asz * 0.5f, asz, (ccord_color){0, 0, 0, 0}, false);
      ccord_rect av_rect = {ax, ay, asz, asz};
      if (out_avatar_rect) *out_avatar_rect = av_rect;
      record_user_hit(ui, av_rect, m->author_id);
    }
    bool has_role;
    ccord_color rc = ui_role_color(ui, guild, m->author_id, &has_role);
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), has_role ? rc : COL(text_header));
    float nw = ccord_text_measure(r, name, strlen(name), &ns, 0.0f).x;
    float max_name = maxf(ma->w - M(message_text_x) - M(message_pad_x) - 160.0f, 60.0f);
    chat_sel_process_run(ui, r, name, strlen(name), &ns, text_x, y + top, fminf(nw, max_name), lh, 0.0f);
    nw = ccord_text_draw_ellipsis(r, name, strlen(name), text_x, y + top, max_name, &ns);
    ccord_rect au_rect = {text_x, y + top, maxf(nw, 4.0f), lh};
    if (out_author_rect) *out_author_rect = au_rect;
    record_user_hit(ui, au_rect, m->author_id);
    float x = text_x + nw + 8.0f;
    {   // the blank between the name and the time (badges included) is part of a selected header line
      float x_time = x;
      if (u && u->is_bot) {
        ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
        x_time += ccord_text_measure(r, "BOT", 3, &bs, 0.0f).x + 9.0f + 6.0f;
      }
      if (m->tts) {
        ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_secondary));
        x_time += ccord_text_measure(r, "TTS", 3, &bs, 0.0f).x + 9.0f + 6.0f;
      }
      if (u && u->clan_tag[0] != '\0') {
        x_time += ui_clan_badge_width(ui, r, u->clan_tag,
                                      u->clan_guild_id != CCORD_ID_NONE && u->clan_badge[0] != '\0') +
                  6.0f;
      }
      chat_sel_process_gap(ui, r, text_x + nw, y + top, 1, x_time - (text_x + nw), lh);
    }
    if (u && u->is_bot) {
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
      ccord_vec2 bt = ccord_text_measure(r, "BOT", 3, &bs, 0.0f);
      ccord_rect tag = {x, y + top + (lh - 15.0f) * 0.5f + 0.5f, bt.x + 9.0f, 15.0f};
      ccord_draw_rrect(r, tag, 3.0f, COL(brand));
      ccord_text_draw(r, "BOT", 3, tag.x + 4.5f, tag.y + (tag.h - bt.y) * 0.5f, &bs, 0.0f);
      x += tag.w + 6.0f;
    }
    if (m->tts) {   // text-to-speech messages carry a TTS badge next to the author
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_secondary));
      ccord_vec2 bt = ccord_text_measure(r, "TTS", 3, &bs, 0.0f);
      ccord_rect tag = {x, y + top + (lh - 15.0f) * 0.5f + 0.5f, bt.x + 9.0f, 15.0f};
      ccord_draw_rrect(r, tag, 3.0f, COL(bg_tertiary));
      ccord_text_draw(r, "TTS", 3, tag.x + 4.5f, tag.y + (tag.h - bt.y) * 0.5f, &bs, 0.0f);
      x += tag.w + 6.0f;
      ui->dbg_tts_badges++;
    }
    if (u && u->clan_tag[0] != '\0') {
      x += ui_draw_clan_badge_ex(ui, r, u->clan_guild_id, u->clan_tag, u->clan_badge, x, y + top, lh, true) + 6.0f;
    }
    char when[48];
    size_t wl = ccord_timefmt_message(when, sizeof when, m->timestamp_ms, ui->now_unix_ms, ui->use_utc);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float wy = y + top + (lh - ui_line_h(ui, ts.size)) * 0.5f;
    ccord_vec2 when_sz = ccord_text_measure(r, when, wl, &ts, 0.0f);
    chat_sel_process_run(ui, r, when, wl, &ts, x, y + top, when_sz.x, lh, 0.0f);
    ccord_text_draw(r, when, wl, x, wy, &ts, 0.0f);
    if (m->timestamp_ms > 0) record_ts_hit(ui, (ccord_rect){x, y + top, when_sz.x, lh}, m->timestamp_ms, false);
    float header_end = x + when_sz.x;
    if (message_edited(m)) {
      chat_sel_process_gap(ui, r, header_end, y + top, 1, 6.0f, lh);
      ccord_vec2 et = ccord_text_measure(r, "(edited)", 8, &ts, 0.0f);
      chat_sel_process_run(ui, r, "(edited)", 8, &ts, x + when_sz.x + 6.0f, y + top, et.x, lh, 0.0f);
      ccord_text_draw(r, "(edited)", 8, x + when_sz.x + 6.0f, wy, &ts, 0.0f);
      if (m->edited_timestamp_ms > 0) {
        record_ts_hit(ui, (ccord_rect){x + when_sz.x + 6.0f, y + top, et.x, lh}, m->edited_timestamp_ms, true);
      }
      header_end += 6.0f + et.x;
    }
    chat_sel_line_end(ui, r, header_end, y + top, ts.size * 0.4f, lh, true);
    content_y += lh;
  }
  bool no_body = d.body_len == 0 && decor_has_body_cards(&d);
  ui_jumbo_item jumbo_items[UI_JUMBO_EMOJI_MAX];
  size_t jumbo_n = no_body ? 0u : chat_message_jumbo(ui, m, jumbo_items);
  if (compact && hover) {
    char clock[24];
    size_t cl = ccord_timefmt_clock(clock, sizeof clock, m->timestamp_ms, ui->use_utc);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    ccord_vec2 tw = ccord_text_measure(r, clock, cl, &ts, 0.0f);
    float gutter = M(message_text_x) - M(message_pad_x);
    float clock_x = ma->x + M(message_pad_x) + (gutter - tw.x) * 0.5f - 4.0f;
    float first_h = jumbo_n > 0u ? (M(font_size_base) * 3.0f + 8.0f) : lh;
    ccord_text_draw(r, clock, cl, clock_x, content_y + (first_h - ui_line_h(ui, ts.size)) * 0.5f, &ts, 0.0f);
    if (m->timestamp_ms > 0) record_ts_hit(ui, (ccord_rect){clock_x, content_y, tw.x, first_h}, m->timestamp_ms, false);
  }
  float wrap = content_wrap(ui, ma->w);
  ui->current_drawing_msg_id = m->id;
  ccord_color body_c = COL(text_normal);
  if (m->send_state == CCORD_MSG_SEND_PENDING) body_c = ui_alpha(COL(text_normal), 0.55f);
  else if (m->send_state == CCORD_MSG_SEND_FAILED) body_c = COL(danger);
  if (compact && m->tts) ui->dbg_tts_badges++;   // the "(tts)" body marker is this row's badge
  float body_h = no_body
                     ? 0.0f
                     : (jumbo_n > 0u
                            ? chat_jumbo_emoji_walk(ui, r, jumbo_items, jumbo_n, text_x, content_y, wrap,
                                                    body_c, compact && message_edited(m),
                                                    compact && m->tts, true)
                            : md_body_walk(ui, r, d.body, d.body_len, text_x, content_y, wrap, body_c, true));
  ui->current_drawing_msg_id = CCORD_ID_NONE;
  float card_y = content_y + body_h + M(message_line_gap) * 0.5f;
  float card_w = maxf(ma->w - M(message_text_x) - M(message_pad_x), 40.0f);
  float after_cards_y = card_y + draw_decor_cards(ui, r, &d, text_x, card_y, card_w);
  if (message_has_reactions(m)) {
    float by = after_cards_y + M(message_line_gap) * 0.5f;
    after_cards_y = by + reaction_pills_walk(ui, r, m, text_x, by, wrap, true);
  }
  if (message_send_unconfirmed(m)) {
    float sy = after_cards_y + M(message_line_gap) * 0.5f;
    draw_send_status(ui, r, m, text_x, sy);
    after_cards_y = sy + UI_SEND_STATUS_H;
  }
  if (ephem) {
    float ey = after_cards_y + 3.0f;
    const char *eprefix = "Only you can see this \xE2\x80\x94 ";
    const char *elink = "Dismiss message";
    ccord_text_style eps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_style els = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_link));
    ccord_vec2 ept = ccord_text_measure(r, eprefix, strlen(eprefix), &eps, 0.0f);
    ccord_vec2 elt = ccord_text_measure(r, elink, strlen(elink), &els, 0.0f);
    ccord_text_draw(r, eprefix, strlen(eprefix), text_x, ey, &eps, 0.0f);
    float lx = text_x + ept.x;
    ccord_rect lrect = {lx, ey - 1.0f, elt.x + 4.0f, maxf(elt.y + 2.0f, 16.0f)};
    ccord_text_draw(r, elink, strlen(elink), lx, ey, &els, 0.0f);
    if (ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, lrect)) {
      ccord_draw_rect(r, (ccord_rect){lx, ey + elt.y - 1.0f, elt.x, 1.0f}, COL(text_link));
    }
    if (ui->nephem_hits < UI_EPHEM_HIT_MAX) {
      ui->ephem_hits[ui->nephem_hits].rect = lrect;
      ui->ephem_hits[ui->nephem_hits].channel_id = m->channel_id;
      ui->ephem_hits[ui->nephem_hits].message_id = m->id;
      ui->nephem_hits++;
    }
  }
  if (!ui->touch_mode && row_hv > 0.004f && !message_send_unconfirmed(m)) {
    // Unconfirmed local echoes have no server-side message yet: reactions, reply, pin and the overflow
    // menu would all fail against their synthetic ids, so only Retry is offered.
    ccord_push_alpha(r, row_hv);
    ccord_push_offset(r, 0.0f, (1.0f - row_hv) * 4.0f);
    float bw = UIS_ICON_BTN, bh = UIS_ICON_BTN;
    bool edit_btn = !ephem && ui_message_editable_by_self(ui, m);   // own messages: Edit sits right after react
    float nbtn = ephem ? 1.0f : (edit_btn ? 5.0f : 4.0f);
    float bar_w = nbtn * bw + nbtn + 3.0f, bar_h = bh + 4.0f;   // 1 px between buttons, 2 px inset at each end
    float bar_x = ma->x + ma->w - bar_w - 16.0f;
    float bar_y = maxf(y + skip - 20.0f, ma->y + 2.0f);   // hangs over the row top: 32 px buttons clear the author line
    ccord_rect bar = {bar_x, bar_y, bar_w, bar_h};
    uis_frame(ui, r, bar, uis_tokens_of(ui).r_control, COL(bg_primary), UIS_ELEV_POPOVER);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_secondary));
    if (ephem) {
      ccord_rect b3 = {bar.x + 2.0f, bar.y + 2.0f, bw, bh};
      ps.color = uis_icon_button(ui, r, b3, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b3)}, "barbtn", 3);
      ccord_vec2 mt = ccord_text_measure(r, "...", 3, &ps, 0.0f);
      ccord_text_draw(r, "...", 3, b3.x + (b3.w - mt.x) * 0.5f, b3.y + (b3.h - mt.y) * 0.5f - 2.0f, &ps, 0.0f);
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
      if (hover) {
        ui->hover_bar_msg_id = m->id;
        ui->hover_bar_rect = bar;
        ui->hover_bar_reply_rect = (ccord_rect){0, 0, 0, 0};
        ui->hover_bar_pin_rect = (ccord_rect){0, 0, 0, 0};
        ui->hover_bar_more_rect = b3;
      }
    } else {
      // Slots after react: Edit (own messages only) comes first, so reply, pin and overflow move one slot right.
      float slot = bw + 1.0f, after_react = edit_btn ? 2.0f : 1.0f;
      ccord_rect b0 = {bar.x + 2.0f, bar.y + 2.0f, bw, bh};
      ccord_rect be = {bar.x + 2.0f + slot, bar.y + 2.0f, bw, bh};
      ccord_rect b1 = {bar.x + 2.0f + after_react * slot, bar.y + 2.0f, bw, bh};
      ccord_rect b2 = {bar.x + 2.0f + (after_react + 1.0f) * slot, bar.y + 2.0f, bw, bh};
      ccord_rect b3 = {bar.x + 2.0f + (after_react + 2.0f) * slot, bar.y + 2.0f, bw, bh};
      ccord_color c0 = uis_icon_button(ui, r, b0, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b0)}, "barbtn", 0);
      ccord_color c1 = uis_icon_button(ui, r, b1, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b1)}, "barbtn", 1);
      ccord_color c2 = uis_icon_button(ui, r, b2, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b2)}, "barbtn", 2);
      ccord_color c3 = uis_icon_button(ui, r, b3, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b3)}, "barbtn", 3);
      ui_expr_draw_smiley(r, b0.x + b0.w * 0.5f, b0.y + b0.h * 0.5f, 14.0f, c0);
      ui_icon_reply(r, b1.x + b1.w * 0.5f, b1.y + b1.h * 0.5f, 14.0f, c1);
      ui_icon_pin(r, b2.x + b2.w * 0.5f, b2.y + b2.h * 0.5f, 14.0f, c2);
      ps.color = c3;
      ccord_vec2 mt = ccord_text_measure(r, "...", 3, &ps, 0.0f);
      ccord_text_draw(r, "...", 3, b3.x + (b3.w - mt.x) * 0.5f, b3.y + (b3.h - mt.y) * 0.5f - 2.0f, &ps, 0.0f);
      if (edit_btn) {
        ccord_color ce = uis_icon_button(ui, r, be, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, be)}, "barbtn", 4);
        ui_icon_edit(r, be.x + be.w * 0.5f, be.y + be.h * 0.5f, 14.0f, ce);
      }
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
      if (hover) {   // only the row under the pointer has a live bar; a fading one is for the eye
        ui->hover_bar_msg_id = m->id;
        ui->hover_bar_rect = bar;
        ui->hover_bar_reply_rect = b1;
        ui->hover_bar_pin_rect = b2;
        ui->hover_bar_more_rect = b3;
        ui->hover_bar_edit_rect = edit_btn ? be : (ccord_rect){0, 0, 0, 0};
        if (ui->nreaction_hits < UI_REACTION_HIT_MAX) {
          ui_reaction_hit *hit = &ui->reaction_hits[ui->nreaction_hits++];
          hit->rect = b0;
          hit->channel_id = m->channel_id;
          hit->message_id = m->id;
          hit->emoji[0] = '\0';
          hit->is_add_btn = true;
        }
      }
    }
  }
}

static void draw_empty_state(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch) return;
  if (ui->search_open && ui->search_edit.len > 0) {
    char msg[192];
    snprintf(msg, sizeof msg, "No messages match \"%s\"", ccord_text_edit_cstr(&ui->search_edit));
    ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 t = ccord_text_measure(r, msg, strlen(msg), &st, ma->w - 40.0f);
    ccord_text_draw(r, msg, strlen(msg), ma->x + (ma->w - t.x) * 0.5f, ma->y + ma->h * 0.5f - t.y * 0.5f, &st, ma->w - 40.0f);
    return;
  }
  if (!ccord_store_history_loaded(ui->d.store, ch->id)) {
    ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    const char *msg = "Loading messages...";
    ccord_vec2 t = ccord_text_measure(r, msg, strlen(msg), &st, 0.0f);
    ccord_text_draw(r, msg, strlen(msg), ma->x + (ma->w - t.x) * 0.5f, ma->y + ma->h * 0.5f - t.y * 0.5f, &st, 0.0f);
    return;
  }
  if (ch->guild_id == CCORD_DM_GUILD_ID) {
    float iy = maxf(ma->y + 16.0f, ma->y + ma->h - UI_DM_INTRO_H - 16.0f);
    draw_dm_intro(ui, r, ch, ma->x + M(message_pad_x), iy, ma->w - 2.0f * M(message_pad_x));
    return;
  }
  char title[160], sub[240];
  snprintf(title, sizeof title, "Welcome to #%s!", ch->name);
  snprintf(sub, sizeof sub, "This is the start of the #%s channel.", ch->name);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 12.0f, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_secondary));
  float wrap = ma->w - 2.0f * M(message_pad_x);
  ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, wrap);
  ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, wrap);
  float y = ma->y + ma->h - 16.0f - st.y - 8.0f - tt.y;
  ccord_text_draw(r, title, strlen(title), ma->x + M(message_pad_x), y, &ts, wrap);
  ccord_text_draw(r, sub, strlen(sub), ma->x + M(message_pad_x), y + tt.y + 8.0f, &ss, wrap);
}

static bool friend_matches_tab(const ccord_relationship *rel, const ccord_user *u, int tab)
{
  if (!rel) return false;
  switch (tab) {
    case 0: return rel->kind == CCORD_RELATIONSHIP_FRIEND && u && u->presence != CCORD_PRESENCE_OFFLINE;
    case 1: return rel->kind == CCORD_RELATIONSHIP_FRIEND;
    case 2: return rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING || rel->kind == CCORD_RELATIONSHIP_PENDING_OUTGOING;
    case 3: return rel->kind == CCORD_RELATIONSHIP_BLOCKED;
  }
  return false;
}

// Shared fading scrollbar for the clipped lists outside the timeline (Friends, stage tiles, forum,
// pins, inbox, reactions, message requests, quick switcher). Same look as the timeline thumb: it
// appears while hovered or shortly after scrolling, then fades out. Returns the drawn thumb rect
// (zero size when the list fits or the thumb is invisible) so tests can assert it.
ccord_rect ui_draw_list_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect view, float content_h,
                                  float scroll, const char *ns, uint64_t id)
{
  if (view.w <= 0.0f || view.h <= 0.0f || content_h <= view.h + 0.5f) return (ccord_rect){0, 0, 0, 0};
  bool over = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, view);
  bool recent = ui->last_scroll_ms && ui->now_ms < ui->last_scroll_ms + 1200;
  bool shown = over || recent;
  float vis = ui_fade_ms(ui, ns, id, shown, 160);
  if (vis < 0.01f) return (ccord_rect){0, 0, 0, 0};
  float sbw = M(scrollbar_width);
  float thumb_h = maxf(view.h * (view.h / content_h), M(scrollbar_min_thumb));
  if (thumb_h > view.h) thumb_h = view.h;
  float max_s = content_h - view.h;
  float norm = max_s > 0.0f ? clampf(scroll / max_s, 0.0f, 1.0f) : 0.0f;
  float top = view.y + norm * (view.h - thumb_h);
  float x = view.x + view.w - sbw - 3.0f;
  ccord_draw_rrect(r, (ccord_rect){x, view.y + 2.0f, sbw, view.h - 4.0f}, sbw * 0.5f, ui_alpha(COL(scrollbar_track), vis));
  ccord_draw_rrect(r, (ccord_rect){x, top, sbw, thumb_h}, sbw * 0.5f, ui_alpha(COL(scrollbar_thumb), vis));
  return (ccord_rect){x, top, sbw, thumb_h};
}

static void draw_dm_friends_view(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ui->nfriend_row_hits = 0;
  size_t nrel = ccord_store_relationship_count(ui->d.store);
  size_t matched = 0;
  for (size_t i = 0; i < nrel; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    const ccord_user *u = rel ? ccord_store_user(ui->d.store, rel->user_id) : nullptr;
    if (friend_matches_tab(rel, u, ui->dm_friends_tab)) matched++;
  }
  static const char *const k_tab_titles[4] = {"ONLINE", "ALL FRIENDS", "PENDING", "BLOCKED"};
  int tab = ui->dm_friends_tab >= 0 && ui->dm_friends_tab < 4 ? ui->dm_friends_tab : 0;
  char hdr[96];
  snprintf(hdr, sizeof hdr, "%s \xE2\x80\x94 %zu", k_tab_titles[tab], matched);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
  float x = ma->x + 24.0f;
  float w = maxf(ma->w - 48.0f, 80.0f);
  float y = ma->y + 20.0f;
  ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x, y, w, &hs);
  y += 28.0f;
  // The rows scroll under the fixed header; off-fold friends stay reachable via wheel + thumb.
  ccord_rect view = {ma->x, y, ma->w, ma->y + ma->h - y};
  if (view.h < 0.0f) view.h = 0.0f;
  ui->dm_friends_content_h = (float)matched * 56.0f;
  float max_s = maxf(ui->dm_friends_content_h - view.h, 0.0f);
  ui->dm_friends_scroll = clampf(ui->dm_friends_scroll, 0.0f, max_s);
  ccord_push_clip(r, view);
  float ry = view.y - ui->dm_friends_scroll;
  for (size_t i = 0; i < nrel; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    const ccord_user *u = rel ? ccord_store_user(ui->d.store, rel->user_id) : nullptr;
    if (!friend_matches_tab(rel, u, tab)) continue;
    ccord_rect row_r = {x, ry + 2.0f, w, 52.0f};
    ccord_rect sep_r = {x, ry, w, 1.0f};
    ry += 56.0f;
    if (row_r.y + row_r.h < view.y || row_r.y > view.y + view.h) continue;
    bool fully_visible = row_r.y >= view.y && row_r.y + row_r.h <= view.y + view.h;
    ccord_draw_rect(r, sep_r, COL(border_subtle));
    ui_state_fill(ui, r, row_r, 6.0f, "frow", rel->user_id, ui->mouse_in && ui_in_rect(ui->mx, ui->my, row_r), false);
    ui_draw_avatar(ui, r, u, x + 24.0f, row_r.y + 26.0f, 36.0f, COL(bg_primary), false);
    const char *fnick = ui_friend_nickname(ui, rel->user_id);
    const char *dname = (fnick && fnick[0]) ? fnick
                        : ((u && u->display_name && u->display_name[0]) ? u->display_name
                        : ((u && u->name && u->name[0]) ? u->name : "Unknown User"));
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_draw_ellipsis(r, dname, strlen(dname), x + 52.0f, row_r.y + 8.0f, w - 200.0f, &ns);
    const char *sub = (u && u->status_text && u->status_text[0]) ? u->status_text
                      : (rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING ? "Incoming Friend Request"
                      : (rel->kind == CCORD_RELATIONSHIP_PENDING_OUTGOING ? "Outgoing Friend Request"
                      : (rel->kind == CCORD_RELATIONSHIP_BLOCKED ? "Blocked"
                      : (u && u->presence == CCORD_PRESENCE_ONLINE ? "Online"
                      : (u && u->presence == CCORD_PRESENCE_IDLE ? "Idle"
                      : (u && u->presence == CCORD_PRESENCE_DND ? "Do Not Disturb" : "Offline"))))));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_draw_ellipsis(r, sub, strlen(sub), x + 52.0f, row_r.y + 28.0f, w - 200.0f, &ss);
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_secondary));
    ccord_rect msg_r = {0, 0, 0, 0}, acc_r = {0, 0, 0, 0}, rem_r = {0, 0, 0, 0};
    if (rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING) {
      acc_r = (ccord_rect){x + w - 144.0f, row_r.y + 12.0f, 64.0f, 28.0f};
      rem_r = (ccord_rect){x + w - 72.0f, row_r.y + 12.0f, 64.0f, 28.0f};
      ccord_draw_rrect(r, acc_r, 14.0f, COL(status_online));
      ccord_draw_rrect(r, rem_r, 14.0f, COL(bg_secondary));
      ccord_text_style abs_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
      ccord_vec2 at = ccord_text_measure(r, "Accept", 6, &abs_st, 0.0f);
      ccord_vec2 rt = ccord_text_measure(r, "Ignore", 6, &bs, 0.0f);
      ccord_text_draw(r, "Accept", 6, acc_r.x + (acc_r.w - at.x) * 0.5f, acc_r.y + (acc_r.h - at.y) * 0.5f, &abs_st, 0.0f);
      ccord_text_draw(r, "Ignore", 6, rem_r.x + (rem_r.w - rt.x) * 0.5f, rem_r.y + (rem_r.h - rt.y) * 0.5f, &bs, 0.0f);
    } else if (rel->kind == CCORD_RELATIONSHIP_PENDING_OUTGOING || rel->kind == CCORD_RELATIONSHIP_BLOCKED) {
      rem_r = (ccord_rect){x + w - 76.0f, row_r.y + 12.0f, 68.0f, 28.0f};
      ccord_draw_rrect(r, rem_r, 14.0f, COL(bg_secondary));
      const char *lbl = (rel->kind == CCORD_RELATIONSHIP_BLOCKED) ? "Unblock" : "Cancel";
      ccord_vec2 bt = ccord_text_measure(r, lbl, strlen(lbl), &bs, 0.0f);
      ccord_text_draw(r, lbl, strlen(lbl), rem_r.x + (rem_r.w - bt.x) * 0.5f, rem_r.y + (rem_r.h - bt.y) * 0.5f, &bs, 0.0f);
    } else {
      msg_r = (ccord_rect){x + w - 112.0f, row_r.y + 12.0f, 68.0f, 28.0f};
      rem_r = (ccord_rect){x + w - 36.0f, row_r.y + 12.0f, 28.0f, 28.0f};
      ccord_draw_rrect(r, msg_r, 14.0f, COL(bg_secondary));
      ccord_draw_rrect(r, rem_r, 14.0f, COL(bg_secondary));
      ccord_vec2 bt = ccord_text_measure(r, "Message", 7, &bs, 0.0f);
      ccord_text_draw(r, "Message", 7, msg_r.x + (msg_r.w - bt.x) * 0.5f, msg_r.y + (msg_r.h - bt.y) * 0.5f, &bs, 0.0f);
      ui_icon_close(r, rem_r.x + rem_r.w * 0.5f, rem_r.y + rem_r.h * 0.5f, 11.0f, COL(text_muted));
    }
    // Only fully visible rows are clickable: a half-clipped row must not catch hits outside the fold.
    if (fully_visible && ui->nfriend_row_hits < 32) {
      ui->friend_row_hits[ui->nfriend_row_hits].row_rect = row_r;
      ui->friend_row_hits[ui->nfriend_row_hits].msg_rect = msg_r.w > 0.0f ? msg_r : row_r;
      ui->friend_row_hits[ui->nfriend_row_hits].accept_rect = acc_r;
      ui->friend_row_hits[ui->nfriend_row_hits].remove_rect = rem_r;
      ui->friend_row_hits[ui->nfriend_row_hits].user_id = rel->user_id;
      ui->friend_row_hits[ui->nfriend_row_hits].kind = rel->kind;
      ui->nfriend_row_hits++;
    }
    if (fully_visible) record_user_hit(ui, (ccord_rect){x, row_r.y, 50.0f, row_r.h}, rel->user_id);
  }
  ccord_pop_clip(r);
  ui->dm_friends_thumb = ui_draw_list_scrollbar(ui, r, view, ui->dm_friends_content_h, ui->dm_friends_scroll,
                                                "friendscroll", 0);
}

static void draw_scrollbar(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ui->thumb_h = 0.0f;
  float top01, size01;
  ccord_vlist_thumb(&ui->vlist, ma->h, &top01, &size01);
  if (size01 >= 0.999f) return;
  bool over = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, *ma);
  bool recent = ui->last_scroll_ms && ui->now_ms < ui->last_scroll_ms + 1200;
  bool shown = over || recent || ui->drag == DRAG_SCROLLBAR;
  float vis = ui_fade_ms(ui, "vscroll", 0, shown, 160);   // fades in and out instead of blinking
  if (vis < 0.01f) return;
  float sbw = M(scrollbar_width);
  float thumb_h = maxf(size01 * ma->h, M(scrollbar_min_thumb));
  if (thumb_h > ma->h) thumb_h = ma->h;
  float norm = size01 < 0.999f ? top01 / (1.0f - size01) : 0.0f;
  float top = ma->y + clampf(norm, 0.0f, 1.0f) * (ma->h - thumb_h);
  float x = ma->x + ma->w - sbw - 3.0f;
  ccord_draw_rrect(r, (ccord_rect){x, ma->y + 2.0f, sbw, ma->h - 4.0f}, sbw * 0.5f, ui_alpha(COL(scrollbar_track), vis));
  ccord_draw_rrect(r, (ccord_rect){x, top, sbw, thumb_h}, sbw * 0.5f, ui_alpha(COL(scrollbar_thumb), vis));
  ui->thumb_x = x;
  ui->thumb_w = sbw;
  ui->thumb_top = top;
  ui->thumb_h = shown ? thumb_h : 0.0f;   // a bar that is on its way out cannot be grabbed
}

// Typing expiry is unix ms from live transports but the mock can run on a small virtual clock.
static bool typing_notice_active(const ccord_ui *ui, int64_t expires_ms)
{
  if (expires_ms <= 0) return false;
  int64_t now = expires_ms > 1000000000000LL ? ui->now_unix_ms : (int64_t)ui->now_ms;
  return expires_ms > now;
}

static size_t active_typing_count(const ccord_ui *ui, ccord_id ch)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  size_t total = 0;
  size_t n = ccord_store_typing_count(ui->d.store, ch);
  for (size_t i = 0; i < n; i++) {
    ccord_id user = ccord_store_typing_user_at(ui->d.store, ch, i);
    if (user == CCORD_ID_NONE || user == self) continue;
    if (typing_notice_active(ui, ccord_store_typing_expires_at(ui->d.store, ch, i))) total++;
  }
  return total;
}

static float typing_reserve_h(const ccord_ui *ui)
{
  return ui_line_h(ui, M(font_size_small) - 2.0f) + 14.0f;
}

static void draw_typing(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return;
  ccord_id self = ccord_store_self_id(ui->d.store);
  char names[3][CCORD_NAME_MAX_BYTES + 1];
  size_t shown = 0, total = 0;
  size_t n = ccord_store_typing_count(ui->d.store, ch);
  for (size_t i = 0; i < n; i++) {
    ccord_id user = ccord_store_typing_user_at(ui->d.store, ch, i);
    if (user == CCORD_ID_NONE || user == self) continue;
    if (!typing_notice_active(ui, ccord_store_typing_expires_at(ui->d.store, ch, i))) continue;
    total++;
    if (shown < 3) {
      const ccord_user *u = ccord_store_user(ui->d.store, user);
      snprintf(names[shown], sizeof names[shown], "%s", u ? u->name : "Someone");
      shown++;
    }
  }
  if (total == 0) {
    ui->typing_next_ms = 0;
    ui->typing_anim_only = false;
    return;
  }
  char text[3 * (CCORD_NAME_MAX_BYTES + 1) + 32];
  if (total == 1) snprintf(text, sizeof text, "%s is typing...", names[0]);
  else if (total == 2) snprintf(text, sizeof text, "%s and %s are typing...", names[0], names[1]);
  else if (total == 3) snprintf(text, sizeof text, "%s, %s and %s are typing...", names[0], names[1], names[2]);
  else snprintf(text, sizeof text, "Several people are typing...");
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_secondary));
  ccord_vec2 t = ccord_text_measure(r, text, strlen(text), &st, ma->w - 2.0f * M(message_pad_x));
  float h = ui_line_h(ui, st.size) + 8.0f;
  const float dots_w = 24.0f;   // the three bouncing dots in front of the text
  ccord_rect box = {ma->x + M(message_pad_x), ma->y + ma->h - h - 6.0f, t.x + 20.0f + dots_w, h};
  if (box.w > ma->w - 2.0f * M(message_pad_x)) box.w = ma->w - 2.0f * M(message_pad_x);
  ui_popup_begin(ui, r, "typing", 0, UI_POPUP_MS, 6.0f);
  ccord_draw_rrect(r, box, h * 0.5f, COL(bg_tertiary));
  for (int d = 0; d < 3; d++) {
    float wave = 0.0f;
    if (!ui->anim.reduced_motion) {
      float phase = (float)(ui->now_ms % 1200u) / 1200.0f * 6.2831853f - (float)d * 0.9f;
      wave = fmaxf(0.0f, sinf(phase));   // each dot rises in turn, then rests
    }
    ccord_draw_circle(r, box.x + 12.0f + (float)d * 6.0f, box.y + h * 0.5f - wave * 2.5f, 2.0f,
                      ui_alpha(COL(text_secondary), ui->anim.reduced_motion ? 0.65f : (0.45f + 0.55f * wave)));
  }
  ccord_text_draw(r, text, strlen(text), box.x + 10.0f + dots_w, box.y + (h - t.y) * 0.5f, &st, box.w - 20.0f - dots_w);
  ui_popup_end(r);
  if (ui->anim.reduced_motion) {
    ui->typing_next_ms = 0;
  } else {
    // Throttle typing-dot redraws to ~20 Hz (50 ms) instead of pegging 144 Hz;
    // ui_draw_frame finalizes typing_anim_only & anim.pending after all other animations run.
    ui->typing_next_ms = ui->now_ms + 50u;
  }
}

// Reaching the top while the user scrolls up asks the session for the previous page (before=<id>).
static void maybe_request_older(ccord_ui *ui, size_t first_visible)
{
  // Search keeps the full timeline, so paging older history works while the panel is open too.
  if (!ui->scroll_up_pending || !ui->d.session) return;
  if (first_visible > 5 || ui->vlist.stick_bottom || ui->vlist.count == 0) return;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return;
  if (!ccord_session_history_has_more(ui->d.session, ch)) {
    ui->scroll_up_pending = false;
    return;
  }
  const ccord_message *first = ccord_store_message_at(ui->d.store, ch, 0);
  if (!first) return;
  if (ui->hist_request_ms && ui->now_ms < ui->hist_request_ms + UI_HISTORY_RETRY_MS) return;
  if (ui->hist_channel == ch && ui->hist_before == first->id) {
    ui->scroll_up_pending = false;
    return;
  }
  ccord_error err;
  if (ccord_session_request_history_before(ui->d.session, ch, first->id, &err) == CCORD_OK) {
    ui->hist_channel = ch;
    ui->hist_before = first->id;
    ui->hist_request_ms = ui->now_ms ? ui->now_ms : 1;
    ui->scroll_up_pending = false;
  } else {
    ccord_error_clear(&err);
  }
}

static void draw_dm_requests_view(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ui->dm_requests_thumb = (ccord_rect){0, 0, 0, 0};
  bool want_spam = (ui->dm_requests_tab == 1);
  size_t dmn = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
  size_t matched = 0;
  for (size_t i = 0; i < dmn; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
    if (c && c->is_message_request && c->is_spam == want_spam) matched++;
  }
  float pad = 20.0f, card_h = 74.0f, gap = 10.0f;
  ui->dm_requests_content_h = matched > 0 ? (pad * 2.0f + (float)matched * (card_h + gap)) : 0.0f;
  float max_s = maxf(ui->dm_requests_content_h - ma->h, 0.0f);
  ui->dm_requests_scroll = clampf(ui->dm_requests_scroll, 0.0f, max_s);

  if (matched == 0) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) + 1.0f, COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    const char *empty_t = want_spam ? "No spam message requests" : "No pending message requests";
    const char *empty_s = want_spam
      ? "Filtered message requests from unknown senders will appear here."
      : "Messages from people you don't share a friend connection with appear here before reaching your DMs.";
    ccord_vec2 tt = ccord_text_measure(r, empty_t, strlen(empty_t), &ts, 0.0f);
    ccord_text_draw(r, empty_t, strlen(empty_t), ma->x + (ma->w - tt.x) * 0.5f, ma->y + ma->h * 0.42f, &ts, 0.0f);
    ccord_vec2 st = ccord_text_measure(r, empty_s, strlen(empty_s), &ss, fminf(ma->w - 48.0f, 440.0f));
    ccord_text_draw(r, empty_s, strlen(empty_s), ma->x + (ma->w - st.x) * 0.5f,
                    ma->y + ma->h * 0.42f + tt.y + 8.0f, &ss, fminf(ma->w - 48.0f, 440.0f));
    return;
  }

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
  ccord_text_style abs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_on_brand));
  ccord_text_style rbs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));

  float y = ma->y + pad - ui->dm_requests_scroll;
  for (size_t i = 0; i < dmn; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
    if (!c || !c->is_message_request || c->is_spam != want_spam) continue;
    ccord_rect card = {ma->x + pad, y, maxf(ma->w - pad * 2.0f, 160.0f), card_h};
    y += card_h + gap;
    if (card.y + card.h < ma->y || card.y > ma->y + ma->h) continue;

    bool hover = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, card);
    ccord_draw_rrect(r, (ccord_rect){card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f}, 9.0f, COL(border_subtle));
    ccord_draw_rrect(r, card, 8.0f, hover ? COL(bg_item_hover) : COL(bg_secondary));

    const ccord_user *u = ui_dm_recipient(ui, c);
    const char *dname = ui_dm_display_name(ui, c);
    const char *uname = (u && u->handle && u->handle[0]) ? u->handle : ((u && u->name && u->name[0]) ? u->name : dname);
    ccord_user fallback = {
      .id = u ? u->id : (c->user_id != CCORD_ID_NONE ? c->user_id : c->id),
      .name = dname,
      .avatar_url = (u && u->avatar_url) ? u->avatar_url : "",
      .presence = u ? u->presence : CCORD_PRESENCE_OFFLINE,
      .avatar_palette = u ? u->avatar_palette : (uint8_t)(c->id % 6u),
    };
    ui_draw_avatar(ui, r, &fallback, card.x + 32.0f, card.y + card.h * 0.5f, 40.0f,
                   hover ? COL(bg_item_hover) : COL(bg_secondary), false);

    ccord_rect rej_r = {card.x + card.w - 86.0f, card.y + (card.h - 30.0f) * 0.5f, 72.0f, 30.0f};
    ccord_rect acc_r = {rej_r.x - 80.0f, rej_r.y, 72.0f, 30.0f};
    bool ahv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, acc_r);
    bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rej_r);
    ccord_draw_rrect(r, acc_r, 5.0f, ahv ? ui_mix(COL(brand), COL(text_on_brand), 0.12f) : COL(brand));
    ccord_draw_rrect(r, rej_r, 5.0f, rhv ? COL(danger) : COL(bg_tertiary));
    ccord_vec2 at = ccord_text_measure(r, "Accept", 6, &abs, 0.0f);
    ccord_vec2 rt = ccord_text_measure(r, "Reject", 6, &rbs, 0.0f);
    ccord_text_draw(r, "Accept", 6, acc_r.x + (acc_r.w - at.x) * 0.5f, acc_r.y + (acc_r.h - at.y) * 0.5f, &abs, 0.0f);
    ccord_text_draw(r, "Reject", 6, rej_r.x + (rej_r.w - rt.x) * 0.5f, rej_r.y + (rej_r.h - rt.y) * 0.5f,
                    rhv ? &abs : &rbs, 0.0f);

    float tx = card.x + 64.0f;
    float tw = maxf(acc_r.x - tx - 14.0f, 40.0f);
    float nw = ccord_text_draw_ellipsis(r, dname, strlen(dname), tx, card.y + 14.0f, tw * 0.6f, &ns);
    char hbuf[96];
    snprintf(hbuf, sizeof hbuf, "@%s", uname ? uname : "user");
    ccord_text_draw_ellipsis(r, hbuf, strlen(hbuf), tx + nw + 8.0f, card.y + 16.0f, maxf(tw - nw - 8.0f, 20.0f), &hs);

    size_t mc = ccord_store_message_count(ui->d.store, c->id);
    char pbuf[192] = "Sent you a message request";
    if (mc > 0) {
      const ccord_message *lm = ccord_store_message_at(ui->d.store, c->id, mc - 1u);
      if (lm) (void)ccord_message_preview(lm, pbuf, sizeof pbuf);
    }
    size_t pbl = ui_streamer_mask_inplace(ui, pbuf, strlen(pbuf), true);
    ccord_text_draw_ellipsis(r, pbuf, pbl, tx, card.y + 40.0f, tw, &ps);

    // Only fully visible cards are clickable: clipped accept/reject buttons must not catch hits.
    if (card.y >= ma->y && card.y + card.h <= ma->y + ma->h && ui->ndm_request_hits < 32u) {
      ui->dm_request_hits[ui->ndm_request_hits].card_rect = card;
      ui->dm_request_hits[ui->ndm_request_hits].accept_rect = acc_r;
      ui->dm_request_hits[ui->ndm_request_hits].reject_rect = rej_r;
      ui->dm_request_hits[ui->ndm_request_hits].channel_id = c->id;
      ui->ndm_request_hits++;
    }
  }
  ui->dm_requests_thumb = ui_draw_list_scrollbar(ui, r, *ma, ui->dm_requests_content_h, ui->dm_requests_scroll,
                                                 "reqscroll", 0);
}

// Clears every message-timeline hit / debug counter. The timeline repopulates these as it draws;
// the stage view must clear them too, or stale hits from a previously drawn transcript would
// intercept clicks meant for the stage (e.g. the Text Chat button).
static void reset_message_hits(ccord_ui *ui)
{
  ui->nvis = 0;
  ui->ndm_request_hits = 0;
  ui->nlinks = 0;
  ui->ntext_hits = 0;
  ui->nuser_hits = 0;
  ui->nreaction_hits = 0;
  ui->ndbg_reaction_draws = 0;
  ui->ncode_copy_hits = 0;
  ui->nspoiler_hits = 0;
  ui->nchan_hits = 0;
  ui->nreply_hits = 0;
  ui->nts_hits = 0;
  ui->nimg_hits = 0;
  ui->nmedia_hits = 0;
  ui->npoll_hits = 0;
  ui->ncomp_hits = 0;
  ui->nephem_hits = 0;
  ui->nretry_hits = 0;
  ui->dbg_pending_echoes = ui->dbg_failed_echoes = 0;
  ui->nthread_hits = 0;
  ui->ninvite_hits = 0;
  ui->dbg_invite_cards = 0;
  ui->nfriend_row_hits = 0;
  ui->jump_present_rect = (ccord_rect){0, 0, 0, 0};
  ui->new_bar_rect = (ccord_rect){0, 0, 0, 0};
  ui->new_bar_ack_rect = (ccord_rect){0, 0, 0, 0};
  ui->new_bar_target_row = SIZE_MAX;
  ui->dbg_date_dividers = 0;
  ui->dbg_new_dividers = 0;
  ui->nblocked_hits = 0;
  ui->dbg_blocked_text_drawn = 0;
  ui->ndbg_code_runs = 0;
  ui->ndbg_md_runs = 0;
  ui->dbg_gallery_cards = 0;
  ui->dbg_media_player_cards = 0;
  ui->dbg_embed_fields_drawn = 0;
  ui->dbg_component_rows = 0;
  ui->dbg_component_buttons = 0;
  ui->dbg_component_selects = 0;
  memset(ui->dbg_cv2_drawn, 0, sizeof ui->dbg_cv2_drawn);
  memset(ui->dbg_cv2_height, 0, sizeof ui->dbg_cv2_height);
  ui->dbg_thread_cards = 0;
  ui->dbg_burst_reaction_pills = 0;
  ui->hover_bar_msg_id = CCORD_ID_NONE;
  ui->hover_bar_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_reply_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_pin_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_more_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_edit_rect = (ccord_rect){0, 0, 0, 0};
  ui->dbg_first = ui->dbg_last = ui->dbg_drawn = ui->dbg_measured = 0;
  ui->dbg_reply_cards = ui->dbg_attachment_cards = ui->dbg_embed_cards = 0;
  ui->dbg_sticker_cards = ui->dbg_forward_cards = ui->dbg_poll_cards = 0;
  ui->dbg_streamer_invite_shown[0] = '\0';
  ui->dbg_streamer_invite_masked[0] = '\0';
  ui->thumb_h = 0.0f;
}

// Notes the newest message of the selected channel. A newer one that arrives while the timeline is pinned to the bottom is
// a live arrival. Ids grow with arrival: a local echo has the top bit set, so the confirmation of an echo (a smaller
// snowflake that replaces it) and a deleted newest message are not new arrivals. The first sight of a channel is not one.
// Each live arrival keeps its own fade: a newer arrival does not restart or snap an older one that is still fading in.
static void chat_live_add(ccord_ui *ui, ccord_id msg)
{
  ui_chat_live *slot = &ui->chat_live[0];
  for (size_t i = 0; i < UI_CHAT_LIVE_MAX; i++) {
    if (ui->chat_live[i].until_ms < slot->until_ms) slot = &ui->chat_live[i];
  }
  slot->msg = msg;
  slot->until_ms = ui->now_ms + UI_MSG_ENTER_MS;
}

static bool chat_is_live(const ccord_ui *ui, ccord_id msg)
{
  for (size_t i = 0; i < UI_CHAT_LIVE_MAX; i++) {
    if (ui->chat_live[i].until_ms > ui->now_ms && ui->chat_live[i].msg == msg) return true;
  }
  return false;
}

// Notes the newest message of the selected channel. Messages newer than the old tail that arrive while the timeline is
// pinned to the bottom are live arrivals. Ids grow with arrival: a local echo has the top bit set, so the confirmation of
// an echo (a smaller snowflake that replaces it) and a deleted newest message are not new arrivals. The first sight of a
// channel is not one. After a gap (`resumed`: the timeline was not drawn last frame) the tail is only noted: messages
// that came in while it was hidden are old news when it is shown again.
static void chat_track_tail(ccord_ui *ui, ccord_id ch, bool resumed)
{
  size_t n = ccord_store_message_count(ui->d.store, ch);
  const ccord_message *last = n > 0 ? ccord_store_message_at(ui->d.store, ch, n - 1) : nullptr;
  ccord_id tail = last ? last->id : CCORD_ID_NONE;
  ccord_id prev = ui->chat_tail_channel == ch ? ui->chat_tail_id : CCORD_ID_NONE;
  ui->chat_tail_channel = ch;
  ui->chat_tail_id = tail;
  if (prev == CCORD_ID_NONE || tail <= prev || !ui->vlist.stick_bottom || resumed) return;
  // Several messages can arrive between two draws: the store is in id order, so walk back over the newer ones.
  for (size_t k = n, added = 0; k > 0 && added < UI_CHAT_LIVE_MAX; k--) {
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, k - 1);
    if (!m || m->id <= prev) break;
    chat_live_add(ui, m->id);
    added++;
  }
}

// Fade-in value of the row at view index i and the message it shows: 1 unless that message is a live arrival.
static float chat_row_enter(ccord_ui *ui, ccord_id ch, size_t i, ccord_id *msg_id)
{
  size_t store_i = 0;
  const ccord_message *m = view_message_at(ui, ch, i, &store_i);
  *msg_id = m ? m->id : CCORD_ID_NONE;
  if (!m || !chat_is_live(ui, m->id)) return 1.0f;
  return ui_enter(ui, "msgin", m->id, UI_MSG_ENTER_MS);
}

// A message counts as seen once its row has been drawn; a reaction pill added later to it pops in.
static void chat_seen_note(ccord_ui *ui, ccord_id msg_id)
{
  if (msg_id != CCORD_ID_NONE && !chat_seen_find(ui, msg_id)) chat_seen_put(ui, msg_id);
}

// Messages that must lie below the viewport before the jump-to-present bar shows (settings, default 10).
static size_t jump_present_threshold(void)
{
  int n = ccord_ui_pref_jump_present_messages();
  return n < 1 ? 1u : (size_t)n;
}

void ui_draw_messages(ccord_ui *ui, ccord_renderer *r)
{
  (void)chat_caches_ensure(ui);
  ccord_rect ma = ui_rect(ui->layout.message_area);
  reset_message_hits(ui);
  if (ma.w <= 0.0f || ma.h <= 0.0f) return;
  if (ma.w != ui->vlist_width) {
    ui->vlist_width = ma.w;
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  ccord_push_clip(r, ma);
  if (ui_is_dm_requests_view(ui)) {
    draw_dm_requests_view(ui, r, &ma);
    ccord_pop_clip(r);
    return;
  }
  if (ui_is_dm_friends_view(ui)) {
    draw_dm_friends_view(ui, r, &ma);
    ccord_pop_clip(r);
    return;
  }
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ui->vlist.count == 0) {
    draw_empty_state(ui, r, &ma);
    draw_typing(ui, r, &ma);
    ccord_pop_clip(r);
    return;
  }
  msg_ctx ctx = {ui, r, ch};
  ccord_vlist_view view;
  ccord_error err;
  // Not drawn last frame (voice or stage view, Settings, another channel, a friends or empty view): the rows and pills
  // seen before the gap are stale and the live arrivals from before it are over, so the timeline starts settled.
  // A second draw in the same frame is not a gap.
  bool resumed = ui->chat_seq != ui->anim.frame && ui->chat_seq + 1 != ui->anim.frame;
  ui->chat_seq = ui->anim.frame;
  if (resumed) memset(ui->chat_live, 0, sizeof ui->chat_live);
  chat_track_tail(ui, ch, resumed);
  float layout_h = ma.h;
  if (active_typing_count(ui, ch) > 0) {
    layout_h = maxf(ma.h - typing_reserve_h(ui), 1.0f);
  }
  if (ccord_vlist_layout(&ui->vlist, layout_h, ma.w, measure_row, &ctx, &view, &err) != CCORD_OK) {
    CCORD_LOG_E("ui: message layout failed: %s", err.message);
    ccord_pop_clip(r);
    return;
  }
  ui->dbg_nsel = 0;
  if (ui->chat_sel_active && ui->chat_sel_channel == ch) {
    ui->chat_sel_len = 0;
    ui->chat_sel_buf[0] = '\0';
  }
  float y = ma.y + view.first_top;
  size_t drawn = 0;
  for (size_t i = view.first; i <= view.last && i < ui->vlist.count; i++) {
    float h = ccord_vlist_row_height(&ui->vlist, i);
    ccord_rect av_rect = {0, 0, 0, 0}, au_rect = {0, 0, 0, 0};
    ccord_id row_msg = CCORD_ID_NONE;
    float enter = chat_row_enter(ui, ch, i, &row_msg);
    // A live arrival fades in and rises over its own drawing; the layout (y, h) of every row stays as measured.
    float dy = enter < 1.0f ? (1.0f - enter) * UI_MSG_ENTER_SLIDE : 0.0f;
    if (enter < 1.0f) {
      ccord_push_alpha(r, enter);
      ccord_push_offset(r, 0.0f, dy);
    }
    draw_row(&ctx, i, y, h, &ma, &av_rect, &au_rect);
    if (enter < 1.0f) {
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
    }
    chat_seen_note(ui, row_msg);
    if (ui->nvis < UI_VISIBLE_ROWS_MAX) ui->vis[ui->nvis++] = (vis_row){i, y, h, av_rect, au_rect, row_msg, enter, dy};
    y += h;
    drawn++;
  }
  ui->dbg_first = view.first;
  ui->dbg_last = view.last;
  ui->dbg_drawn = drawn;
  ui->dbg_measured = view.measured;
  maybe_request_older(ui, view.first);
  draw_scrollbar(ui, r, &ma);
  ui_nmb_draw(ui, r, &ma, ch, view.first, view.last);   // new_messages_bar.c: fades, draws and records the click rects

  // The bar waits until N messages are below the viewport. At the bottom of the loaded window there are none below, so it
  // stays hidden whatever the channel's last_message_id says.
  bool jump_on = ccord_vlist_rows_below(&ui->vlist, &view) >= jump_present_threshold();
  float jp_t = ui_fade_ms(ui, "jumpbar", 1u, jump_on, UI_BAR_MS);
  ui->dbg_jump_present_t = jp_t;
  // As with the new-messages bar, the click rect follows the bar's state, so a bar that is on is clickable at once.
  if (jp_t > 0.004f || jump_on) {
    const char *jtxt = "You're viewing older messages  ·  Jump to Present";
    ccord_text_style jst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
    ccord_vec2 jt = ccord_text_measure(r, jtxt, strlen(jtxt), &jst, 0.0f);
    float jw = fminf(jt.x + 28.0f, ma.w - 32.0f);
    float jh = 26.0f;
    ccord_rect jbar = {ma.x + (ma.w - jw) * 0.5f, ma.y + ma.h - jh - 8.0f, jw, jh};
    if (jump_on) ui->jump_present_rect = jbar;
    if (jp_t > 0.004f) {
      ccord_push_alpha(r, jp_t);
      ccord_push_offset(r, 0.0f, (1.0f - jp_t) * UI_BAR_SLIDE);   // it rises from below
      ccord_draw_rrect(r, jbar, 13.0f, COL(brand));
      ccord_text_draw_ellipsis(r, jtxt, strlen(jtxt), jbar.x + 14.0f, jbar.y + (jbar.h - jt.y) * 0.5f, jbar.w - 28.0f, &jst);
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
    }
  }
  draw_typing(ui, r, &ma);
  ccord_pop_clip(r);
}

// ---------------------------------------------------------------- composer

static float composer_wrap(const ccord_ui *ui)
{
  // The text area of ui_draw_composer: the box less its padding, the '+' button (28) and the right-hand buttons
  // (desktop: GIF + Sticker + Emoji + Mic = 128px; mobile: Emoji + Send/Mic = 76px).
  float right_reserve = ui->mobile_layout ? 76.0f : 128.0f;
  return maxf(ui->layout.composer.w - 2.0f * M(composer_margin_x) - 2.0f * M(composer_pad_x) - 28.0f - right_reserve, 20.0f);
}

#define UI_UPLOAD_ROW_H 36.0f

static size_t ui_channel_active_uploads(const ccord_ui *ui, ccord_upload_progress *out, size_t max)
{
  if (!ui || !ui->d.session || !ui->d.store || ui_is_dm_requests_view(ui)) return 0;
  ccord_id cid = ccord_store_selected_channel(ui->d.store);
  if (cid == CCORD_ID_NONE) return 0;
  return ccord_session_active_uploads(ui->d.session, cid, out, max);
}

static float ui_upload_banner_height(const ccord_ui *ui)
{
  size_t n = ui_channel_active_uploads(ui, nullptr, 0);
  if (n == 0) return 0.0f;
  size_t rows = n < UI_UPLOADS_SHOW_MAX ? n : UI_UPLOADS_SHOW_MAX;
  return UI_UPLOAD_ROW_H * (float)rows;
}

void ui_upload_debug_get(const ccord_ui *ui, ui_upload_debug *out)
{
  if (!out) return;
  if (!ui) {
    memset(out, 0, sizeof *out);
    return;
  }
  *out = ui->dbg_uploads;
}

static float ui_staged_banner_height(const ccord_ui *ui)
{
  if (!ui || ui->staged_file_count == 0) return 0.0f;
  size_t cols = ui->staged_file_count <= 2u ? ui->staged_file_count : (ui->staged_file_count <= 6u ? 3u : 5u);
  size_t frows = (ui->staged_file_count + cols - 1u) / cols;
  return 28.0f * (float)frows;
}

float ui_composer_measure(ccord_ui *ui, ccord_renderer *r)
{
  float lh = ui_line_h(ui, M(font_size_base));
  float th = lh;
  if (ui->edit.len > 0) {
    th = ui_composer_format_height(ui, r, ccord_text_edit_cstr(&ui->edit), ui->edit.len, composer_wrap(ui));
  }
  float banner = (ui->reply_message != CCORD_ID_NONE ? UI_REPLY_BAR_H : 0.0f) +
                 ui_upload_banner_height(ui) + ui_staged_banner_height(ui);
  return clampf(th + 2.0f * M(composer_pad_y) + banner, M(composer_min_height), M(composer_max_height));
}

void ui_draw_composer(ccord_ui *ui, ccord_renderer *r)
{
  memset(&ui->dbg_uploads, 0, sizeof ui->dbg_uploads);
  ui->msg_req_banner_visible = false;
  ui->msg_req_banner_accept_rect = (ccord_rect){0, 0, 0, 0};
  ui->msg_req_banner_reject_rect = (ccord_rect){0, 0, 0, 0};
  ui->mv.composer_btn = (ccord_rect){0, 0, 0, 0};
  ui->reply_bar_rect = ui->reply_close_rect = ui->reply_text_rect = ui->reply_input_rect = (ccord_rect){0, 0, 0, 0};
  // Voice message controls are set again below when they are drawn; stale rects must never take a click.
  ui->vm.mic_rect = ui->vm.bar_rect = ui->vm.send_rect = ui->vm.cancel_rect = (ccord_rect){0, 0, 0, 0};
  const ccord_layout_rect *strip = &ui->layout.composer;
  ccord_draw_rect(r, ui_rect(*strip), COL(bg_primary));
  if (ui_is_dm_requests_view(ui)) {
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    return;
  }
  ccord_rect box = ui_composer_box(ui);
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  const ccord_channel *sel_ch = ui_selected_channel(ui);
  if (sel_ch && sel_ch->guild_id == CCORD_DM_GUILD_ID && sel_ch->is_message_request) {
    ui->msg_req_banner_visible = true;
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    ccord_draw_rrect(r, box, M(composer_radius), COL(bg_secondary));
    const char *who = ui_dm_display_name(ui, sel_ch);
    char prompt[192];
    snprintf(prompt, sizeof prompt, "Do you want to let %s message you?", who && who[0] ? who : "this user");
    ccord_rect rej_r = {box.x + box.w - 86.0f, box.y + (box.h - 28.0f) * 0.5f, 74.0f, 28.0f};
    ccord_rect acc_r = {rej_r.x - 82.0f, rej_r.y, 74.0f, 28.0f};
    ui->msg_req_banner_accept_rect = acc_r;
    ui->msg_req_banner_reject_rect = rej_r;
    bool ahv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, acc_r);
    bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rej_r);
    uis_button(ui, r, acc_r, "Accept", UIS_BTN_PRIMARY, UIS_BTN_S_SM, (uis_state){.hover = ahv}, "mrbacc", 0);
    uis_button(ui, r, rej_r, "Reject", UIS_BTN_DANGER_SOFT, UIS_BTN_S_SM, (uis_state){.hover = rhv}, "mrbrej", 0);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    float py = box.y + (box.h - ui_line_h(ui, ps.size)) * 0.5f;
    ccord_text_draw_ellipsis(r, prompt, strlen(prompt), box.x + 14.0f, py, maxf(acc_r.x - (box.x + 24.0f), 20.0f), &ps);
    return;
  }
  if (sel_ch && sel_ch->kind == CCORD_CHANNEL_TEXT && ui_self_pending(ui, sel_ch->guild_id) &&
      !ui_role_preview_active_for(ui, sel_ch->guild_id)) {
    // Membership screening not completed: the box is a locked banner whose button reopens the rules.
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    ccord_draw_rrect(r, box, M(composer_radius), COL(bg_secondary));
    const char *why = ui_composer_lock_reason(ui);
    if (!why) why = "Complete the server rules to send messages.";
    float bw = uis_label_width(ui, r, "Complete the rules");
    ccord_rect btn = {box.x + box.w - bw - 12.0f, box.y + (box.h - 28.0f) * 0.5f, bw, 28.0f};
    ui->mv.composer_btn = btn;
    bool bhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, btn);
    uis_button(ui, r, btn, "Complete the rules", UIS_BTN_PRIMARY, UIS_BTN_S_SM, (uis_state){.hover = bhv}, "mvcomp", 0);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_placeholder));
    float py = box.y + (box.h - ui_line_h(ui, ps.size)) * 0.5f;
    ccord_text_draw_ellipsis(r, why, strlen(why), box.x + 14.0f, py, maxf(btn.x - (box.x + 24.0f), 20.0f), &ps);
    return;
  }
  if (ui_vm_is_active(ui)) {
    // Recording, or a clip waiting to send: the bar takes the place of the text box until it is sent or discarded.
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    ui_vm_draw_bar(ui, r, box);
    return;
  }
  ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
  bool replying = ui->reply_message != CCORD_ID_NONE;
  if (replying) {
    // One unit with the composer box below it: same x and width, no gap, rounded top corners here and square ones there.
    ccord_rect bar = {box.x, box.y, box.w, UI_REPLY_BAR_H};
    float rad = fminf(M(composer_radius), bar.h);
    ccord_draw_rrect(r, bar, rad, COL(bg_secondary));
    ccord_draw_rect(r, (ccord_rect){bar.x, bar.y + rad, bar.w, bar.h - rad}, COL(bg_secondary));
    ui->reply_bar_rect = bar;
    const ccord_message *rm = ccord_store_find_message(ui->d.store, ui->reply_channel, ui->reply_message);
    const ccord_user *ru = rm ? ccord_store_user(ui->d.store, rm->author_id) : nullptr;
    const char *rname = ru && ru->name && ru->name[0] ? ru->name : "a message";
    if (rm && ru) {
      ccord_id rg = ccord_store_selected_guild(ui->d.store);
      const ccord_member *gm = (rg != CCORD_ID_NONE && rg != CCORD_DM_GUILD_ID)
                                   ? ccord_store_find_member(ui->d.store, rg, rm->author_id) : nullptr;
      if (gm && gm->nick[0] != '\0') rname = gm->nick;
    }
    float by = bar.y + (bar.h - UI_REPLY_BTN_H) * 0.5f;
    bool mouse = ui->mouse_in && !ui->menu.open && !ui->settings_open;
    ccord_rect close_r = {bar.x + bar.w - UI_REPLY_BAR_PAD * 0.5f - UI_REPLY_CLOSE_W, by, UI_REPLY_CLOSE_W, UI_REPLY_BTN_H};
    ui->reply_close_rect = close_r;
    bool close_hv = mouse && ui_in_rect(ui->mx, ui->my, close_r);
    ccord_draw_rrect(r, close_r, close_r.h * 0.5f, close_hv ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_close(r, close_r.x + close_r.w * 0.5f, close_r.y + close_r.h * 0.5f, 12.0f,
                  close_hv ? COL(text_header) : COL(text_muted));
    float right = close_r.x - 6.0f;
    if (bar.w >= 110.0f) {
      ccord_rect pr = {close_r.x - 6.0f - UI_REPLY_PING_W, by, UI_REPLY_PING_W, UI_REPLY_BTN_H};
      ui->reply_ping_rect = pr;
      bool on = ui->reply_ping;
      bool pr_hv = mouse && ui_in_rect(ui->mx, ui->my, pr);
      ccord_color pc = on ? COL(brand) : COL(text_muted);
      ccord_draw_rrect(r, pr, pr.h * 0.5f, on ? ui_alpha(COL(brand), pr_hv ? 0.40f : 0.28f)
                                              : (pr_hv ? COL(bg_item_hover) : COL(bg_tertiary)));
      ccord_text_style pst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), pc);
      ccord_vec2 pm = ccord_text_measure(r, "@", 1, &pst, 0.0f);
      float pcx = pr.x + pr.w * 0.5f, pcy = pr.y + pr.h * 0.5f;
      ccord_text_draw(r, "@", 1, pcx - pm.x * 0.5f, pcy - pm.y * 0.5f, &pst, 0.0f);
      if (!on) {   // off is also shown by a strike, not only by the muted colour
        ccord_draw_line(r, (ccord_vec2){pcx - 6.0f, pcy + 5.0f}, (ccord_vec2){pcx + 6.0f, pcy - 5.0f}, 1.5f, pc);
      }
      right = pr.x - 8.0f;
    }
    ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
    ccord_text_style nst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
    float tx = bar.x + UI_REPLY_BAR_PAD;
    float avail = maxf(right - tx, 0.0f);
    float ty = bar.y + (bar.h - ui_line_h(ui, ls.size)) * 0.5f;
    float lw = ccord_text_draw_ellipsis(r, "Replying to ", 12, tx, ty, avail, &ls);
    float nw = 0.0f;
    if (avail - lw > 12.0f) nw = ccord_text_draw_ellipsis(r, rname, strlen(rname), tx + lw, ty, avail - lw, &nst);
    ui->reply_text_rect = (ccord_rect){tx, ty, lw + nw, ui_line_h(ui, ls.size)};
    box.y += bar.h;
    box.h -= bar.h;
    ui->reply_input_rect = box;
    if (box.h < 10.0f) return;
  }
  memset(ui->staged_spoiler_rects, 0, sizeof ui->staged_spoiler_rects);
  memset(ui->staged_remove_rects, 0, sizeof ui->staged_remove_rects);
  {
    ccord_upload_progress ups[UI_UPLOADS_SHOW_MAX];
    size_t nup_total = ui_channel_active_uploads(ui, ups, UI_UPLOADS_SHOW_MAX);
    size_t nup = nup_total < UI_UPLOADS_SHOW_MAX ? nup_total : UI_UPLOADS_SHOW_MAX;
    if (nup > 0) {
      float ubanner_h = UI_UPLOAD_ROW_H * (float)nup;
      ccord_rect ubanner = {box.x, box.y, box.w, ubanner_h};
      uis_tokens utk = uis_tokens_of(ui);
      ccord_draw_rrect(r, ubanner, utk.r_control, COL(bg_secondary));
      ui->dbg_uploads.count = nup_total;
      ui->dbg_uploads.banner_rect = ubanner;
      for (size_t i = 0; i < nup; i++) {
        ccord_rect row_rc = {box.x + 2.0f, box.y + (float)i * UI_UPLOAD_ROW_H + 2.0f,
                             maxf(box.w - 4.0f, 20.0f), UI_UPLOAD_ROW_H - 4.0f};
        ccord_draw_rrect(r, row_rc, utk.r_control, COL(bg_tertiary));
        int pct = 0;
        float target_frac = 0.04f;
        if (ups[i].phase == CCORD_UPLOAD_FINALIZE) {
          pct = 100;
          target_frac = 1.0f;
        } else if (ups[i].phase == CCORD_UPLOAD_PUT) {
          float bfrac = ups[i].total_bytes > 0
                          ? clampf((float)ups[i].uploaded_bytes / (float)ups[i].total_bytes, 0.0f, 1.0f)
                          : 0.5f;
          pct = (int)(bfrac * 100.0f + 0.5f);
          if (pct > 99 && ups[i].uploaded_bytes < ups[i].total_bytes) pct = 99;
          target_frac = clampf(0.05f + 0.91f * bfrac, 0.05f, 0.96f);
        }
        float shown = ui_ease_to(ui, "uprog", ups[i].upload_id ? ups[i].upload_id : (uint64_t)(i + 1u),
                                 target_frac, 90u);
        ccord_color accent = ups[i].phase == CCORD_UPLOAD_FINALIZE ? COL(status_online) : COL(brand);
        ui_icon_attachment(r, row_rc.x + 12.0f, row_rc.y + 11.0f, 13.0f, accent);

        char title[160];
        const char *fname = ups[i].filename[0] ? ups[i].filename : "attachment";
        if (ups[i].file_count > 1u) {
          snprintf(title, sizeof title, "Uploading %s (%u/%u)", fname,
                   (unsigned)(ups[i].file_index + 1u), (unsigned)ups[i].file_count);
        } else {
          snprintf(title, sizeof title, "Uploading %s", fname);
        }
        if (i + 1u == nup && nup_total > nup) {
          size_t tl = strlen(title);
          if (tl + 16u < sizeof title) {
            snprintf(title + tl, sizeof title - tl, " (+%zu more)", nup_total - nup);
          }
        }

        char done_s[32], tot_s[32], stat[96];
        format_size(ups[i].uploaded_bytes, done_s, sizeof done_s);
        format_size(ups[i].total_bytes, tot_s, sizeof tot_s);
        if (ups[i].phase == CCORD_UPLOAD_SLOT) {
          snprintf(stat, sizeof stat, "Preparing\xE2\x80\xA6 \xC2\xB7 %s (0%%)", tot_s);
        } else if (ups[i].phase == CCORD_UPLOAD_FINALIZE) {
          snprintf(stat, sizeof stat, "Finishing\xE2\x80\xA6 \xC2\xB7 %s (100%%)", tot_s);
        } else {
          snprintf(stat, sizeof stat, "%s / %s (%d%%)", done_s, tot_s, pct);
        }

        ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, utk.fs_caption, COL(text_secondary));
        ccord_text_style tts = ui_style(ui, CCORD_FONT_BOLD, utk.fs_caption, COL(text_header));
        ccord_vec2 sm = ccord_text_measure(r, stat, strlen(stat), &sts, 0.0f);
        float text_y = row_rc.y + 3.0f;
        float stat_x = maxf(row_rc.x + row_rc.w - sm.x - 8.0f, row_rc.x + 24.0f);
        if (row_rc.w >= 140.0f) {
          ccord_text_draw(r, stat, strlen(stat), stat_x, text_y, &sts, 0.0f);
        }
        float title_avail = maxf((row_rc.w >= 140.0f ? stat_x - 8.0f : row_rc.x + row_rc.w - 8.0f) - (row_rc.x + 22.0f), 10.0f);
        ccord_text_draw_ellipsis(r, title, strlen(title), row_rc.x + 22.0f, text_y, title_avail, &tts);

        ccord_rect bar = {row_rc.x + 8.0f, row_rc.y + row_rc.h - 6.5f, maxf(row_rc.w - 16.0f, 8.0f), 4.0f};
        ccord_draw_rrect(r, bar, bar.h * 0.5f, COL(bg_primary));
        float fw = maxf(bar.w * clampf(shown, 0.0f, 1.0f), bar.h);
        ccord_draw_rrect(r, (ccord_rect){bar.x, bar.y, fminf(fw, bar.w), bar.h}, bar.h * 0.5f, accent);

        ui->dbg_uploads.bar_rects[i] = bar;
        ui->dbg_uploads.shown_fraction[i] = shown;
        ui->dbg_uploads.pct[i] = pct;
        snprintf(ui->dbg_uploads.label[i], sizeof ui->dbg_uploads.label[i], "%.96s | %.48s", title, stat);
      }
      box.y += ubanner_h;
      box.h -= ubanner_h;
      if (box.h < 10.0f) return;
    }
  }
  if (ui->staged_file_count > 0) {
    size_t cols = ui->staged_file_count <= 2u ? ui->staged_file_count : (ui->staged_file_count <= 6u ? 3u : 5u);
    size_t frows = (ui->staged_file_count + cols - 1u) / cols;
    float fbanner_h = 28.0f * (float)frows;
    ccord_rect fbanner = {box.x, box.y, box.w, fbanner_h};
    ccord_draw_rrect(r, fbanner, 4.0f, COL(bg_secondary));
    float cell_w = box.w / (float)cols;
    for (size_t i = 0; i < ui->staged_file_count; i++) {
      size_t col = i % cols;
      size_t row = i / cols;
      ccord_rect cell = {box.x + (float)col * cell_w + 2.0f, box.y + (float)row * 28.0f + 2.0f,
                         maxf(cell_w - 4.0f, 20.0f), 24.0f};
      bool sp = ui->staged_files[i].spoiler;
      ccord_draw_rrect(r, cell, 4.0f, COL(bg_tertiary));
      ccord_draw_rect(r, (ccord_rect){cell.x, cell.y + 3.0f, 2.0f, cell.h - 6.0f},
                      sp ? COL(status_idle) : COL(status_online));
      ccord_rect rem_rc = {cell.x + cell.w - 22.0f, cell.y + 1.0f, 20.0f, 22.0f};
      ui->staged_remove_rects[i] = rem_rc;
      float sp_w = cols <= 2u ? 54.0f : 30.0f;
      ccord_rect sp_rc = {rem_rc.x - sp_w - 3.0f, cell.y + 2.0f, sp_w, 20.0f};
      ui->staged_spoiler_rects[i] = sp_rc;
      ccord_draw_rrect(r, sp_rc, 4.0f, sp ? ui_alpha(COL(status_idle), 0.30f) : COL(bg_secondary));
      const char *slbl = cols <= 2u ? "SPOILER" : "SP";
      ccord_text_style sst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f,
                                      sp ? COL(text_header) : COL(text_muted));
      ccord_vec2 sm = ccord_text_measure(r, slbl, strlen(slbl), &sst, 0.0f);
      ccord_text_draw(r, slbl, strlen(slbl), sp_rc.x + (sp_rc.w - sm.x) * 0.5f,
                      sp_rc.y + (sp_rc.h - sm.y) * 0.5f, &sst, 0.0f);
      ui_icon_close(r, rem_rc.x + rem_rc.w * 0.5f, rem_rc.y + rem_rc.h * 0.5f, 11.0f, COL(text_muted));
      char flabel[192];
      snprintf(flabel, sizeof flabel, "%s %s (%.1f KB)", sp ? "[SPOILER]" : "[File]",
               ui->staged_files[i].filename, (double)ui->staged_files[i].size / 1024.0);
      ccord_text_style fls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
      float avail = maxf(sp_rc.x - (cell.x + 8.0f), 6.0f);
      ccord_text_draw_ellipsis(r, flabel, strlen(flabel), cell.x + 7.0f,
                               cell.y + (cell.h - ui_line_h(ui, fls.size)) * 0.5f, avail, &fls);
    }
    box.y += fbanner_h;
    box.h -= fbanner_h;
    if (box.h < 10.0f) return;
  }
  bool editing = ui->editing_message != CCORD_ID_NONE;
  if (editing) ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, M(composer_radius) + 1.0f, COL(brand));
  uis_surface(ui, r, box, COL(bg_input));
  if (replying) {   // the reply bar sits on the box: its top edge is square
    ccord_draw_rect(r, (ccord_rect){box.x, box.y, box.w, fminf(M(composer_radius), box.h)}, COL(bg_input));
  }
  float attach_w = UIS_ICON_BTN;
  ui->composer_attach_rect = (ccord_rect){box.x + 4.0f, box.y + (box.h - attach_w) * 0.5f, attach_w, attach_w};
  bool attach_hv = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, ui->composer_attach_rect);
  ccord_text_style pls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base),
                                  uis_icon_button(ui, r, ui->composer_attach_rect, (uis_state){.hover = attach_hv, .selected = ui->plus_menu_open}, "compattach", 0));
  ccord_vec2 plt = ccord_text_measure(r, "+", 1, &pls, 0.0f);
  ccord_text_draw(r, "+", 1, ui->composer_attach_rect.x + (attach_w - plt.x) * 0.5f,
                  ui->composer_attach_rect.y + (attach_w - plt.y) * 0.5f, &pls, 0.0f);
  float right_reserve = ui->mobile_layout ? 76.0f : 128.0f;
  ccord_rect area = {box.x + M(composer_pad_x) + 28.0f, box.y + M(composer_pad_y),
                     box.w - 2.0f * M(composer_pad_x) - 28.0f - right_reserve, box.h - 2.0f * M(composer_pad_y)};
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  float wrap = composer_wrap(ui);   // = max(area.w, 20); the styled layout wraps, measures and draws at this width
  const char *text = ccord_text_edit_cstr(&ui->edit);
  size_t len = ui->edit.len;
  float lh = ui_line_h(ui, st.size);

  if (ui->pending_click) {
    ui->pending_click = false;
    size_t pos = ui_composer_format_hit(ui, r, text, len, wrap, ui->pending_x - area.x, ui->pending_y - area.y + ui->composer_scroll);
    if (ui->pending_select_mode == 1) {
      ccord_text_edit_select_word_at(&ui->edit, pos);
    } else if (ui->pending_select_mode == 2) {
      ccord_text_edit_select_line_at(&ui->edit, pos);
    } else {
      ccord_text_edit_set_caret(&ui->edit, pos, ui->pending_extend);
    }
    ui->pending_select_mode = 0;
    ui->caret_epoch_ms = ui->now_ms;
    ui->caret_on = true;
    ui_update_autocomplete(ui);
  }
  float total = len ? ui_composer_format_height(ui, r, text, len, wrap) : lh;
  if (ui->pending_vert_move != 0) {
    int dir = ui->pending_vert_move;
    ui->pending_vert_move = 0;
    ccord_rect cur = ui_composer_format_caret(ui, r, text, len, wrap, ui->edit.caret);
    if (dir < 0 && cur.y < lh * 0.5f) {
      ccord_text_edit_set_caret(&ui->edit, 0, ui->pending_extend);
    } else if (dir > 0 && cur.y + lh * 1.5f >= total) {
      ccord_text_edit_set_caret(&ui->edit, len, ui->pending_extend);
    } else {
      float ty = cur.y + (float)dir * lh + lh * 0.5f;
      size_t pos = ui_composer_format_hit(ui, r, text, len, wrap, cur.x, ty);
      ccord_text_edit_set_caret(&ui->edit, pos, ui->pending_extend);
    }
    ui->caret_epoch_ms = ui->now_ms;
    ui->caret_on = true;
  }
  ccord_rect caret = ui_composer_format_caret(ui, r, text, len, wrap, ui->edit.caret);
  if (caret.y < ui->composer_scroll) ui->composer_scroll = caret.y;
  if (caret.y + lh > ui->composer_scroll + area.h) ui->composer_scroll = caret.y + lh - area.h;
  ui->composer_scroll = clampf(ui->composer_scroll, 0.0f, maxf(total - area.h, 0.0f));
  // The platform text-input / IME candidate window follows the CARET, not the whole box.
  ui->text_area = (ccord_rect){area.x + caret.x, area.y - ui->composer_scroll + caret.y,
                               maxf(caret.w, 1.0f), caret.h};

  ccord_push_clip(r, (ccord_rect){box.x, box.y, box.w, box.h});
  const ccord_channel *ch = ui_selected_channel(ui);
  const char *locked = ui_composer_lock_reason(ui);
  if (len == 0) {
    char ph[160];
    if (locked) {
      snprintf(ph, sizeof ph, "%s", locked);
    } else if (ui->d.session && ccord_session_is_stale(ui->d.session)) {
      snprintf(ph, sizeof ph, "Reconnecting\xe2\x80\xa6");   // saved data on screen: sending is refused until READY
    } else if (ch && ch->guild_id == CCORD_DM_GUILD_ID) {
      const ccord_user *u = ch->user_id != CCORD_ID_NONE ? ccord_store_user(ui->d.store, ch->user_id) : nullptr;
      const char *dname = (u && u->name && u->name[0]) ? u->name : ch->name;
      snprintf(ph, sizeof ph, "Message @%s", dname ? dname : "");
    } else {
      snprintf(ph, sizeof ph, "Message #%s", ch ? ch->name : "");
    }
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_placeholder));
    ccord_text_draw_ellipsis(r, ph, strlen(ph), area.x, area.y, area.w, &ps);
  } else {
    size_t sel_lo = ui->edit.caret < ui->edit.anchor ? ui->edit.caret : ui->edit.anchor;
    size_t sel_hi = ui->edit.caret < ui->edit.anchor ? ui->edit.anchor : ui->edit.caret;
    ui_composer_format_draw(ui, r, text, len, wrap, area.x, area.y - ui->composer_scroll, sel_lo, sel_hi,
                            ui->composer_scroll - lh, ui->composer_scroll + area.h + lh);
  }
  // IME composition preedit: distinct colour with an underline, drawn at the caret.
  size_t plen = strlen(ui->ime_preedit);
  if (plen > 0) {
    ccord_text_style pst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(brand));
    float px = area.x + caret.x;
    float py = area.y - ui->composer_scroll + caret.y;
    ccord_vec2 psz = ccord_text_measure(r, ui->ime_preedit, plen, &pst, 0.0f);
    ccord_text_draw(r, ui->ime_preedit, plen, px, py, &pst, 0.0f);
    ccord_draw_rect(r, (ccord_rect){px, py + caret.h - 1.0f, psz.x, 1.0f}, COL(brand));
  }
  if (ui->caret_on && !ui->settings_open && !ui->menu.open) {
    float cy = area.y - ui->composer_scroll + caret.y;
    ccord_draw_rect(r, (ccord_rect){area.x + caret.x, cy + (lh - caret.h) * 0.5f, 1.5f, caret.h}, COL(caret));
  }
  ccord_pop_clip(r);
  ui_expr_draw_composer_buttons(ui, r, box);
  bool mobile_send = ui->mobile_layout && !locked && (ui->edit.len > 0 || ui->staged_file_count > 0);
  if (mobile_send) {
    float sw = 32.0f, sh = 30.0f;
    ccord_rect sb = {box.x + box.w - sw - 6.0f, box.y + (box.h - sh) * 0.5f, sw, sh};
    ui->mobile_send_btn_rect = sb;
    ccord_draw_rrect(r, sb, sh * 0.5f, COL(brand));
    // Crisp send arrow icon centered in the pill.
    float cx = sb.x + sb.w * 0.5f, cy = sb.y + sb.h * 0.5f;
    ccord_draw_line(r, (ccord_vec2){cx - 5.5f, cy}, (ccord_vec2){cx + 5.0f, cy}, 2.0f, COL(text_on_brand));
    ccord_draw_line(r, (ccord_vec2){cx + 1.0f, cy - 4.5f}, (ccord_vec2){cx + 5.5f, cy}, 2.0f, COL(text_on_brand));
    ccord_draw_line(r, (ccord_vec2){cx + 1.0f, cy + 4.5f}, (ccord_vec2){cx + 5.5f, cy}, 2.0f, COL(text_on_brand));
  } else if (!locked) {
    ui_vm_draw_mic(ui, r, box);   // rightmost; the emoji button(s) sit left of it
  }
  if (editing) {
    const char *hint = "Editing message - Enter saves, Esc cancels";
    ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    ccord_text_draw(r, hint, strlen(hint), box.x + 2.0f, box.y + box.h + 5.0f, &hs, box.w);
  } else if (ch && ch->rate_limit_per_user > 0) {
    uint32_t rem_s = ui_slowmode_remaining_s(ui, ch);
    char shint[96];
    ccord_color scol = COL(text_muted);
    if (rem_s > 0) {
      snprintf(shint, sizeof shint, "Slowmode: %us remaining", (unsigned)rem_s);
      scol = COL(status_idle);
    } else {
      bool immune = false;
      if (ui->d.store && ch->guild_id != CCORD_ID_NONE && ch->guild_id != CCORD_DM_GUILD_ID) {
        ccord_id self = ccord_store_self_id(ui->d.store);
        if (ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) {
          uint64_t p = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
          uint64_t bypass = CCORD_PERM_BYPASS_SLOWMODE | CCORD_PERM_MANAGE_CHANNELS | CCORD_PERM_MANAGE_MESSAGES;
          immune = (p & CCORD_PERM_ADMINISTRATOR) != 0 || (p & bypass) != 0;
        }
      }
      if (immune) {
        snprintf(shint, sizeof shint, "Slowmode is enabled (%us \xE2\x80\x94 immune)",
                 (unsigned)ch->rate_limit_per_user);
      } else {
        snprintf(shint, sizeof shint, "Slowmode is enabled (%us)", (unsigned)ch->rate_limit_per_user);
      }
    }
    ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, scol);
    ccord_text_draw(r, shint, strlen(shint), box.x + 2.0f, box.y + box.h + 4.0f, &hs, box.w);
  }
}

// ---------------------------------------------------------------- mention serialization & slash commands

static void ser_append(char *out, size_t cap, size_t *w, const char *src, size_t n)
{
  if (*w + 1u >= cap || n == 0) return;
  size_t avail = cap - 1u - *w;
  if (n > avail) n = avail;
  memcpy(out + *w, src, n);
  *w += n;
}

static bool is_mention_boundary(char c)
{
  return !isalnum((unsigned char)c) && c != '_';
}

static void check_user_cand(const char *src, size_t rem, const char *cand, ccord_id uid,
                            size_t *best_len, char best_tag[static 32])
{
  if (!cand || cand[0] == '\0' || uid == CCORD_ID_NONE) return;
  size_t clen = strlen(cand);
  if (clen == 0 || clen > rem || clen <= *best_len) return;
  if (strncasecmp(src, cand, clen) != 0) return;
  if (clen < rem && !is_mention_boundary(src[clen])) return;
  *best_len = clen;
  snprintf(best_tag, 32, "<@%llu>", (unsigned long long)uid);
}

static bool can_mention_everyone_in(const ccord_store *store, ccord_id guild_id, ccord_id channel_id,
                                    int64_t now_unix_ms)
{
  if (!store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  ccord_id self = ccord_store_self_id(store);
  if (self == CCORD_ID_NONE || !ccord_store_permissions_exact(store, guild_id, self)) return true;
  uint64_t p = ccord_store_permissions(store, guild_id, channel_id, self, now_unix_ms);
  return ccord_perm_has(p, CCORD_PERM_MENTION_EVERYONE);
}

static bool has_at_mention_boundary_before(const char *in, size_t i)
{
  if (i == 0) return true;
  char prev = in[i - 1u];
  return !isalnum((unsigned char)prev) && prev != '_' && prev != '.' && prev != '/' && prev != ':' &&
         prev != '@' && prev != '<';
}

static void check_user_obj_cand(const char *src, size_t rem, const ccord_user *u,
                                size_t *best_len, char best_tag[static 32])
{
  if (!u) return;
  check_user_cand(src, rem, u->name, u->id, best_len, best_tag);
  check_user_cand(src, rem, u->handle, u->id, best_len, best_tag);
}

// Matches a `@user`, `@role`, `@everyone`, `@here`, or `#channel` mention at `in[i]`.
// Returns the matched raw byte length (including `@`/`#`) or 0 if none. Writes `<@id>` / `<@&id>` / `<#id>` /
// `@everyone` / `@here` into `tag_out` when non-null.
static size_t match_mention_at(const ccord_store *store, ccord_id guild_id, const ccord_channel *active_ch,
                               bool gate_perms, int64_t now_unix_ms,
                               const char *in, size_t in_len, size_t i, char tag_out[32])
{
  if (!store || !in || i >= in_len) return 0;
  if (in[i] == '@' && has_at_mention_boundary_before(in, i)) {
    const char *src = in + i + 1u;
    size_t rem = in_len - (i + 1u);
    bool in_guild = guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID;
    bool can_everyone = !gate_perms || (in_guild && active_ch &&
                                        can_mention_everyone_in(store, guild_id, active_ch->id, now_unix_ms));
    if (can_everyone && (!gate_perms || in_guild)) {
      if (rem >= 8u && strncmp(src, "everyone", 8u) == 0 && (rem == 8u || is_mention_boundary(src[8u]))) {
        if (tag_out) snprintf(tag_out, 32, "@everyone");
        return 9u;
      }
      if (rem >= 4u && strncmp(src, "here", 4u) == 0 && (rem == 4u || is_mention_boundary(src[4u]))) {
        if (tag_out) snprintf(tag_out, 32, "@here");
        return 5u;
      }
    }
    size_t best_len = 0;
    char best_tag[32] = {0};
    if (in_guild) {
      size_t mc = ccord_store_member_count(store, guild_id);
      for (size_t k = 0; k < mc; k++) {
        const ccord_member *m = ccord_store_member_at(store, guild_id, k);
        if (!m) continue;
        const ccord_user *u = ccord_store_user(store, m->user_id);
        if (m->nick[0] != '\0') check_user_cand(src, rem, m->nick, m->user_id, &best_len, best_tag);
        check_user_obj_cand(src, rem, u, &best_len, best_tag);
      }
    }
    if (guild_id == CCORD_DM_GUILD_ID) {
      ccord_id self_id = ccord_store_self_id(store);
      check_user_obj_cand(src, rem, ccord_store_user(store, self_id), &best_len, best_tag);
      if (active_ch) {
        if (active_ch->user_id != CCORD_ID_NONE) {
          check_user_obj_cand(src, rem, ccord_store_user(store, active_ch->user_id), &best_len, best_tag);
        }
        for (uint16_t r = 0; r < active_ch->recipient_count && r < CCORD_MAX_DM_RECIPIENTS; r++) {
          check_user_obj_cand(src, rem, ccord_store_user(store, active_ch->recipient_ids[r]), &best_len, best_tag);
        }
      } else {
        size_t dmc = ccord_store_channel_count(store, CCORD_DM_GUILD_ID);
        for (size_t d = 0; d < dmc; d++) {
          const ccord_channel *dc = ccord_store_channel_at(store, CCORD_DM_GUILD_ID, d);
          if (!dc) continue;
          if (dc->user_id != CCORD_ID_NONE) {
            check_user_obj_cand(src, rem, ccord_store_user(store, dc->user_id), &best_len, best_tag);
          }
          for (uint16_t r = 0; r < dc->recipient_count && r < CCORD_MAX_DM_RECIPIENTS; r++) {
            check_user_obj_cand(src, rem, ccord_store_user(store, dc->recipient_ids[r]), &best_len, best_tag);
          }
        }
      }
    }
    if (active_ch) {
      size_t msg_c = ccord_store_message_count(store, active_ch->id);
      for (size_t k = 0; k < msg_c; k++) {
        const ccord_message *msg = ccord_store_message_at(store, active_ch->id, k);
        if (msg && msg->author_id != CCORD_ID_NONE) {
          check_user_obj_cand(src, rem, ccord_store_user(store, msg->author_id), &best_len, best_tag);
        }
      }
    }
    if (in_guild) {
      size_t rc = 0;
      const ccord_role *roles = ccord_store_roles(store, guild_id, &rc);
      for (size_t k = 0; k < rc; k++) {
        const ccord_role *rl = &roles[k];
        if (rl->id == guild_id || !rl->name || rl->name[0] == '\0') continue;
        if (gate_perms && !rl->mentionable && !can_everyone) continue;
        size_t rlen = strlen(rl->name);
        if (rlen == 0 || rlen > rem || rlen <= best_len) continue;
        if (strncasecmp(src, rl->name, rlen) != 0) continue;
        if (rlen < rem && !is_mention_boundary(src[rlen])) continue;
        best_len = rlen;
        snprintf(best_tag, sizeof best_tag, "<@&%llu>", (unsigned long long)rl->id);
      }
    }
    if (best_len > 0) {
      if (tag_out) memcpy(tag_out, best_tag, sizeof best_tag);
      return 1u + best_len;
    }
    return 0;
  }
  if (in[i] == '#' && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID &&
      (i == 0 || (!isalnum((unsigned char)in[i - 1u]) && in[i - 1u] != '_' &&
                  in[i - 1u] != '&' && in[i - 1u] != '<' && in[i - 1u] != '#'))) {
    const char *src = in + i + 1u;
    size_t rem = in_len - (i + 1u);
    size_t best_len = 0;
    char best_tag[32] = {0};
    size_t cc = ccord_store_channel_count(store, guild_id);
    for (size_t k = 0; k < cc; k++) {
      const ccord_channel *c = ccord_store_channel_at(store, guild_id, k);
      if (!c || c->kind == CCORD_CHANNEL_CATEGORY || !c->name || c->name[0] == '\0') continue;
      size_t clen = strlen(c->name);
      if (clen == 0 || clen > rem || clen <= best_len) continue;
      if (strncasecmp(src, c->name, clen) != 0) continue;
      if (clen < rem && (!is_mention_boundary(src[clen]) || src[clen] == '-')) continue;
      best_len = clen;
      snprintf(best_tag, sizeof best_tag, "<#%llu>", (unsigned long long)c->id);
    }
    if (best_len > 0) {
      if (tag_out) memcpy(tag_out, best_tag, sizeof best_tag);
      return 1u + best_len;
    }
  }
  return 0;
}

size_t ui_composer_match_mention(const ccord_ui *ui, const char *line, size_t line_len, size_t pos)
{
  if (!ui || !ui->d.store || !line || pos >= line_len) return 0;
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) return 0;
  ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ccord_store_selected_guild(ui->d.store);
  return match_mention_at(ui->d.store, gid, ch, true, ui->now_unix_ms, line, line_len, pos, nullptr);
}

size_t ccord_ui_serialize_mentions(const ccord_store *store, ccord_id guild_id,
                                   const char *in, size_t in_len, char *out, size_t out_cap)
{
  if (!out || out_cap == 0) return 0;
  if (!in || in_len == 0) {
    out[0] = '\0';
    return 0;
  }
  if (!store) {
    size_t n = in_len < out_cap - 1u ? in_len : out_cap - 1u;
    memcpy(out, in, n);
    out[n] = '\0';
    return n;
  }
  size_t w = 0;
  size_t i = 0;
  while (i < in_len && w + 1u < out_cap) {
    // 1. Fenced code block ```...```
    if (i + 2u < in_len && in[i] == '`' && in[i + 1u] == '`' && in[i + 2u] == '`') {
      size_t j = i + 3u;
      while (j + 2u < in_len && !(in[j] == '`' && in[j + 1u] == '`' && in[j + 2u] == '`')) j++;
      size_t end = (j + 2u < in_len) ? (j + 3u) : in_len;
      ser_append(out, out_cap, &w, in + i, end - i);
      i = end;
      continue;
    }
    // 2. Inline code `...`
    if (in[i] == '`') {
      size_t j = i + 1u;
      while (j < in_len && in[j] != '`') j++;
      if (j < in_len) {
        ser_append(out, out_cap, &w, in + i, j + 1u - i);
        i = j + 1u;
        continue;
      }
      ser_append(out, out_cap, &w, in + i, 1u);
      i++;
      continue;
    }
    // 3. Existing angle-bracket tag <...>
    if (in[i] == '<') {
      size_t j = i + 1u;
      while (j < in_len && in[j] != '>' && in[j] != ' ' && in[j] != '\n' && in[j] != '\t') j++;
      if (j < in_len && in[j] == '>') {
        ser_append(out, out_cap, &w, in + i, j + 1u - i);
        i = j + 1u;
        continue;
      }
    }
    // 4. URLs http:// or https://
    if ((i + 7u <= in_len && strncasecmp(in + i, "http://", 7u) == 0) ||
        (i + 8u <= in_len && strncasecmp(in + i, "https://", 8u) == 0)) {
      size_t j = i;
      while (j < in_len && in[j] != ' ' && in[j] != '\t' && in[j] != '\n' && in[j] != '\r') j++;
      ser_append(out, out_cap, &w, in + i, j - i);
      i = j;
      continue;
    }
    // 5. @user / @role / @everyone / @here & 6. #channel
    if (in[i] == '@' || in[i] == '#') {
      char tag[32] = {0};
      size_t mlen = match_mention_at(store, guild_id, nullptr, false, 0, in, in_len, i, tag);
      if (mlen > 0) {
        ser_append(out, out_cap, &w, tag, strlen(tag));
        i += mlen;
        continue;
      }
    }
    ser_append(out, out_cap, &w, in + i, 1u);
    i++;
  }
  out[w] = '\0';
  return w;
}

static bool slash_cmd_allowed(const ccord_ui *ui, const ccord_channel *ch, const ccord_slash_command *cmd)
{
  if (!ui || !ui->d.store || !ch || !cmd || cmd->name[0] == '\0') return false;
  if (cmd->command_type > 1u) return false;   // USER / MESSAGE commands live in context menus, not the slash picker
  if (ch->guild_id == CCORD_DM_GUILD_ID) return cmd->dm_permission;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) return true;
  uint64_t ch_perms = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
  if (!ccord_perm_has(ch_perms, CCORD_PERM_USE_APPLICATION_COMMANDS)) return false;
  if (cmd->default_member_permissions != UINT64_MAX) {
    if (cmd->default_member_permissions == 0) {
      if ((ch_perms & CCORD_PERM_ADMINISTRATOR) == 0) return false;
    } else if (!ccord_perm_has(ch_perms, cmd->default_member_permissions)) {
      return false;
    }
  }
  return true;
}

static const ccord_slash_command *find_slash_cmd(const ccord_ui *ui, ccord_id guild_id,
                                                 const char *name, size_t nlen)
{
  if (!ui || !ui->d.store || !name || nlen == 0) return nullptr;
  size_t count = 0;
  const ccord_slash_command *cmds = ccord_store_slash_commands(ui->d.store, guild_id, &count);
  for (size_t i = 0; i < count; i++) {
    if (strncasecmp(cmds[i].name, name, nlen) == 0 && cmds[i].name[nlen] == '\0') return &cmds[i];
  }
  return nullptr;
}

typedef struct slash_parsed {
  bool opt_set[CCORD_SLASH_MAX_OPTIONS];
  char opt_vals[CCORD_SLASH_MAX_OPTIONS][256];
  int32_t focused_opt;
  size_t token_start;
  char cur_prefix[128];
  bool completing_opt_name;
  bool has_sub_tree;      // the command has root SUB_COMMAND / SUB_COMMAND_GROUP options
  int sub_group_idx;      // chosen SUB_COMMAND_GROUP option index (-1 = none)
  int sub_cmd_idx;        // chosen SUB_COMMAND option index (-1 = none)
  bool selecting_sub;     // the caret token picks a subcommand / group name
} slash_parsed;

// Options live in a tree: with a subcommand tree only the chosen subcommand's children are fillable;
// without one only root options are. SUB_COMMAND / SUB_COMMAND_GROUP entries never carry values.
static bool slash_opt_in_scope(const ccord_slash_command *cmd, size_t i, const slash_parsed *sp)
{
  if (!sp->has_sub_tree) return cmd->options[i].parent_plus_one == 0;
  if (sp->sub_cmd_idx < 0) return false;
  return cmd->options[i].parent_plus_one == (uint8_t)(sp->sub_cmd_idx + 1);
}

static int find_cmd_opt(const ccord_slash_command *cmd, const char *name, size_t nlen, const slash_parsed *sp)
{
  if (!cmd || !name || nlen == 0) return -1;
  for (size_t i = 0; i < cmd->option_count; i++) {
    if (!slash_opt_in_scope(cmd, i, sp)) continue;
    if (strncasecmp(cmd->options[i].name, name, nlen) == 0 && cmd->options[i].name[nlen] == '\0') {
      return (int)i;
    }
  }
  return -1;
}

static size_t read_slash_val_token(const char *text, size_t limit, size_t pos, char *dst, size_t dst_cap, bool *unclosed)
{
  *unclosed = false;
  size_t w = 0;
  if (pos < limit && text[pos] == '"') {
    pos++;
    while (pos < limit) {
      if (text[pos] == '\\' && pos + 1u < limit) {
        pos++;
        if (w + 1u < dst_cap) dst[w++] = text[pos];
        pos++;
        continue;
      }
      if (text[pos] == '"') {
        pos++;
        dst[w] = '\0';
        return pos;
      }
      if (w + 1u < dst_cap) dst[w++] = text[pos];
      pos++;
    }
    *unclosed = true;
    dst[w] = '\0';
    return pos;
  }
  while (pos < limit && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n') {
    if (w + 1u < dst_cap) dst[w++] = text[pos];
    pos++;
  }
  dst[w] = '\0';
  return pos;
}

static void slash_parse_args(const ccord_slash_command *cmd, const char *text, size_t limit, slash_parsed *out)
{
  memset(out, 0, sizeof *out);
  out->focused_opt = -1;
  out->sub_group_idx = -1;
  out->sub_cmd_idx = -1;
  out->token_start = limit;
  if (!cmd || !text || limit == 0 || text[0] != '/') return;
  size_t nlen = strlen(cmd->name);
  if (limit < 1u + nlen) return;
  size_t pos = 1u + nlen;
  int last_pos_string_opt = -1;
  for (size_t i = 0; i < cmd->option_count; i++) {
    if (cmd->options[i].parent_plus_one == 0 &&
        (cmd->options[i].kind == CCORD_SLASH_OPT_SUB_COMMAND ||
         cmd->options[i].kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP)) {
      out->has_sub_tree = true;
      break;
    }
  }

  while (pos < limit) {
    while (pos < limit && (text[pos] == ' ' || text[pos] == '\t')) pos++;
    if (pos >= limit) {
      out->token_start = limit;
      out->completing_opt_name = true;
      out->focused_opt = -1;
      if (out->has_sub_tree && out->sub_cmd_idx < 0) out->selecting_sub = true;
      return;
    }
    size_t tok_start = pos;
    size_t ilen = 0;
    while (pos + ilen < limit &&
           (isalnum((unsigned char)text[pos + ilen]) || text[pos + ilen] == '_' || text[pos + ilen] == '-')) {
      ilen++;
    }
    int named_idx = -1;
    if (ilen > 0 && pos + ilen < limit && text[pos + ilen] == ':') {
      named_idx = find_cmd_opt(cmd, text + pos, ilen, out);
    }
    if (named_idx >= 0) {
      last_pos_string_opt = -1;
      pos += ilen + 1u;
      while (pos < limit && text[pos] == ' ') pos++;
      size_t val_start = pos;
      bool unclosed = false;
      pos = read_slash_val_token(text, limit, pos, out->opt_vals[named_idx],
                                 sizeof out->opt_vals[named_idx], &unclosed);
      out->opt_set[named_idx] = true;
      if (pos >= limit && (unclosed || val_start == limit || (limit > 0 && text[limit - 1u] != ' '))) {
        out->focused_opt = named_idx;
        out->token_start = val_start;
        out->completing_opt_name = false;
        return;
      }
      continue;
    }
    bool was_quoted = (text[pos] == '"');
    char tok_buf[256];
    bool unclosed = false;
    size_t next_pos = read_slash_val_token(text, limit, pos, tok_buf, sizeof tok_buf, &unclosed);
    bool at_caret_end = (next_pos >= limit && (unclosed || (limit > 0 && text[limit - 1u] != ' ')));

    // Subcommand tree: the first tokens pick the group (if any) and the subcommand before any option.
    if (out->has_sub_tree && out->sub_cmd_idx < 0) {
      int match = -1;
      if (out->sub_group_idx < 0) {
        for (size_t k = 0; k < cmd->option_count; k++) {
          if (cmd->options[k].parent_plus_one != 0) continue;
          if (cmd->options[k].kind != CCORD_SLASH_OPT_SUB_COMMAND &&
              cmd->options[k].kind != CCORD_SLASH_OPT_SUB_COMMAND_GROUP) continue;
          if (strcasecmp(cmd->options[k].name, tok_buf) == 0) { match = (int)k; break; }
        }
        if (match >= 0) {
          if (cmd->options[match].kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP) out->sub_group_idx = match;
          else out->sub_cmd_idx = match;
          pos = next_pos;
          continue;
        }
      } else {
        for (size_t k = 0; k < cmd->option_count; k++) {
          if (cmd->options[k].parent_plus_one != (uint8_t)(out->sub_group_idx + 1)) continue;
          if (cmd->options[k].kind != CCORD_SLASH_OPT_SUB_COMMAND) continue;
          if (strcasecmp(cmd->options[k].name, tok_buf) == 0) { match = (int)k; break; }
        }
        if (match >= 0) {
          out->sub_cmd_idx = match;
          pos = next_pos;
          continue;
        }
      }
      if (at_caret_end) {
        out->selecting_sub = true;
        out->token_start = tok_start;
        out->focused_opt = -1;
        snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
        return;
      }
      pos = next_pos;   // an unknown subcommand token is skipped, not turned into an option value
      continue;
    }

    int pos_idx = -1;
    for (size_t k = 0; k < cmd->option_count; k++) {
      if (!out->opt_set[k] && slash_opt_in_scope(cmd, k, out)) {
        pos_idx = (int)k;
        break;
      }
    }
    // Single-string-option ergonomics: `/echo hello world` appends unquoted words to the string option
    if (!was_quoted && last_pos_string_opt >= 0 &&
        (pos_idx < 0 || (!cmd->options[pos_idx].required &&
                         cmd->options[pos_idx].kind == CCORD_SLASH_OPT_BOOLEAN &&
                         strcasecmp(tok_buf, "true") != 0 && strcasecmp(tok_buf, "false") != 0 &&
                         strcmp(tok_buf, "1") != 0 && strcmp(tok_buf, "0") != 0))) {
      size_t cur_l = strlen(out->opt_vals[last_pos_string_opt]);
      size_t add_l = strlen(tok_buf);
      if (cur_l + 1u + add_l < sizeof out->opt_vals[last_pos_string_opt]) {
        out->opt_vals[last_pos_string_opt][cur_l] = ' ';
        memcpy(out->opt_vals[last_pos_string_opt] + cur_l + 1u, tok_buf, add_l + 1u);
      }
      if (at_caret_end) {
        out->focused_opt = last_pos_string_opt;
        out->token_start = tok_start;
        out->completing_opt_name = !was_quoted;
        snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
        return;
      }
      pos = next_pos;
      continue;
    }
    if (pos_idx >= 0) {
      snprintf(out->opt_vals[pos_idx], sizeof out->opt_vals[pos_idx], "%s", tok_buf);
      out->opt_set[pos_idx] = true;
      last_pos_string_opt = (!was_quoted && cmd->options[pos_idx].kind == CCORD_SLASH_OPT_STRING) ? pos_idx : -1;
      if (at_caret_end) {
        out->focused_opt = pos_idx;
        out->token_start = tok_start;
        out->completing_opt_name = !was_quoted;
        snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
        return;
      }
    } else if (at_caret_end) {
      out->focused_opt = -1;
      out->token_start = tok_start;
      out->completing_opt_name = !was_quoted;
      snprintf(out->cur_prefix, sizeof out->cur_prefix, "%.127s", tok_buf);
      return;
    }
    pos = next_pos;
  }
  out->token_start = limit;
  out->completing_opt_name = true;
  out->focused_opt = -1;
  if (out->has_sub_tree && out->sub_cmd_idx < 0) out->selecting_sub = true;
}

static bool resolve_slash_snowflake(const ccord_store *store, ccord_id guild_id, ccord_slash_opt_kind kind,
                                    const char *raw, char out[static 32])
{
  if (!raw || raw[0] == '\0') return false;
  size_t len = strlen(raw);
  // Strip <@123>, <@!123>, <@&123>, <#123>
  if (len >= 4u && raw[0] == '<' && raw[len - 1u] == '>') {
    size_t off = 1u;
    if (raw[off] == '@' || raw[off] == '#') off++;
    if (raw[off] == '!' || raw[off] == '&') off++;
    ccord_id id = CCORD_ID_NONE;
    if (ccord_id_parse(raw + off, len - 1u - off, &id)) {
      ccord_id_format(id, out);
      return true;
    }
  }
  ccord_id parsed = CCORD_ID_NONE;
  if (ccord_id_parse(raw, len, &parsed)) {
    ccord_id_format(parsed, out);
    return true;
  }
  const char *q = (raw[0] == '@' || raw[0] == '#') ? raw + 1u : raw;
  if (q[0] == '\0' || !store) return false;
  if (kind == CCORD_SLASH_OPT_USER || kind == CCORD_SLASH_OPT_MENTIONABLE) {
    size_t mc = ccord_store_member_count(store, guild_id);
    for (size_t i = 0; i < mc; i++) {
      const ccord_member *m = ccord_store_member_at(store, guild_id, i);
      const ccord_user *u = m ? ccord_store_user(store, m->user_id) : nullptr;
      if ((m && m->nick[0] && strcasecmp(m->nick, q) == 0) ||
          (u && u->name && strcasecmp(u->name, q) == 0) ||
          (u && u->handle && u->handle[0] && strcasecmp(u->handle, q) == 0)) {
        ccord_id_format(m->user_id, out);
        return true;
      }
    }
  }
  if ((kind == CCORD_SLASH_OPT_ROLE || kind == CCORD_SLASH_OPT_MENTIONABLE) &&
      guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    size_t rc = 0;
    const ccord_role *roles = ccord_store_roles(store, guild_id, &rc);
    for (size_t i = 0; i < rc; i++) {
      if (roles[i].name && strcasecmp(roles[i].name, q) == 0) {
        ccord_id_format(roles[i].id, out);
        return true;
      }
    }
  }
  if (kind == CCORD_SLASH_OPT_CHANNEL && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
    size_t cc = ccord_store_channel_count(store, guild_id);
    for (size_t i = 0; i < cc; i++) {
      const ccord_channel *c = ccord_store_channel_at(store, guild_id, i);
      if (c && c->kind != CCORD_CHANNEL_CATEGORY && c->name && strcasecmp(c->name, q) == 0) {
        ccord_id_format(c->id, out);
        return true;
      }
    }
  }
  return false;
}

static bool validate_slash_opt_val(const ccord_store *store, ccord_id guild_id, const ccord_slash_option *opt,
                                   char val[static 256], char err_msg[static 128])
{
  switch (opt->kind) {
    case CCORD_SLASH_OPT_STRING:
      return true;
    case CCORD_SLASH_OPT_INTEGER: {
      if (val[0] == '\0') {
        snprintf(err_msg, 128, "Option '%s' requires an integer", opt->name);
        return false;
      }
      errno = 0;
      char *endp = nullptr;
      (void)strtoll(val, &endp, 10);
      if (errno == ERANGE || !endp || *endp != '\0') {
        snprintf(err_msg, 128, "Option '%s' must be a valid integer", opt->name);
        return false;
      }
      return true;
    }
    case CCORD_SLASH_OPT_NUMBER: {
      if (val[0] == '\0') {
        snprintf(err_msg, 128, "Option '%s' requires a number", opt->name);
        return false;
      }
      errno = 0;
      char *endp = nullptr;
      double d = strtod(val, &endp);
      if (errno == ERANGE || !endp || *endp != '\0' || !isfinite(d)) {
        snprintf(err_msg, 128, "Option '%s' must be a valid number", opt->name);
        return false;
      }
      return true;
    }
    case CCORD_SLASH_OPT_BOOLEAN:
      if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "yes") == 0) {
        snprintf(val, 256, "true");
        return true;
      }
      if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 || strcasecmp(val, "no") == 0) {
        snprintf(val, 256, "false");
        return true;
      }
      snprintf(err_msg, 128, "Option '%s' must be true or false", opt->name);
      return false;
    case CCORD_SLASH_OPT_USER:
    case CCORD_SLASH_OPT_CHANNEL:
    case CCORD_SLASH_OPT_ROLE:
    case CCORD_SLASH_OPT_MENTIONABLE: {
      char resolved[32];
      if (!resolve_slash_snowflake(store, guild_id, opt->kind, val, resolved)) {
        snprintf(err_msg, 128, "Option '%s' could not be resolved", opt->name);
        return false;
      }
      snprintf(val, 256, "%s", resolved);
      return true;
    }
    default: break;  // SUB_COMMAND / SUB_COMMAND_GROUP carry no value of their own
  }
  return true;
}

// ---------------------------------------------------------------- sending

static bool is_blank(const char *s, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    if (s[i] != ' ' && s[i] != '\n' && s[i] != '\t') return false;
  }
  return true;
}

const char *ui_composer_lock_reason(const ccord_ui *ui)
{
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui->d.store || !ui_channel_has_chat(ch)) return nullptr;
  if (ch->guild_id == CCORD_DM_GUILD_ID) {
    if (ch->is_message_request) return "Accept this message request to reply.";
    return nullptr;
  }
  if (ui_role_preview_active_for(ui, ch->guild_id)) {
    uint64_t perms = ui_effective_permissions(ui, ch->guild_id, ch->id);
    bool thread = ccord_discord_type_is_thread(ch->discord_type);
    uint64_t need = CCORD_PERM_VIEW_CHANNEL | (thread ? CCORD_PERM_SEND_MESSAGES_IN_THREADS : CCORD_PERM_SEND_MESSAGES);
    if (ccord_perm_has(perms, need)) return nullptr;
    return "Selected role does not have permission to send messages in this channel.";
  }
  // A pending member still holds the roles' permissions locally, so this must answer before the permission check.
  if (ui_self_pending(ui, ch->guild_id)) return "Complete the server rules to send messages.";
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) return nullptr;
  uint64_t perms = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
  bool thread = ccord_discord_type_is_thread(ch->discord_type);
  uint64_t need = CCORD_PERM_VIEW_CHANNEL | (thread ? CCORD_PERM_SEND_MESSAGES_IN_THREADS : CCORD_PERM_SEND_MESSAGES);
  if (ccord_perm_has(perms, need)) return nullptr;
  const ccord_member *me = ccord_store_find_member(ui->d.store, ch->guild_id, self);
  if (me && me->timeout_until_ms > ui->now_unix_ms) return "You have been timed out and cannot send messages.";
  return "You do not have permission to send messages in this channel.";
}

void ui_send_composer(ccord_ui *ui)
{
  const char *lock = ui_composer_lock_reason(ui);
  if (lock) {
    ui_notice(ui, "%s", lock);
    return;
  }
  const char *t = ccord_text_edit_cstr(&ui->edit);
  size_t len = ui->edit.len;
  bool has_files = ui->staged_file_count > 0;
  if (is_blank(t, len) && !has_files) return;   // Enter on an empty composer sends nothing, like the real client
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) {
    ui_notice(ui, "Select a text channel first");
    return;
  }
  ccord_error err;
  if (ui->editing_message != CCORD_ID_NONE && !has_files) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    char exp_buf[CCORD_SEND_MAX_BYTES + 1];
    char ser_buf[CCORD_SEND_MAX_BYTES + 1];
    if (len > 0 && memchr(t, ':', len) != nullptr) {
      len = ui_expr_replace_shortcodes(ui->d.store, ch->guild_id, t, len, exp_buf, sizeof exp_buf);
      t = exp_buf;
    }
    if (len > 0 && (memchr(t, '@', len) != nullptr || memchr(t, '#', len) != nullptr)) {
      len = ccord_ui_serialize_mentions(ui->d.store, ch->guild_id, t, len, ser_buf, sizeof ser_buf);
      t = ser_buf;
    }
    if (ccord_session_edit_message(ui->d.session, ui->editing_channel, ui->editing_message, t, len, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot edit: %s", err.message);
      return;
    }
    ui->editing_channel = ui->editing_message = CCORD_ID_NONE;
    ccord_text_edit_clear(&ui->edit);
    ui->composer_scroll = 0.0f;
    ui->dirty = true;
    return;
  }

  // Slash commands (`/name ...`) when not uploading files or replying
  if (!has_files && ui->reply_message == CCORD_ID_NONE && len > 0 && t[0] == '/' &&
      memchr(t, '\n', len) == nullptr) {
    size_t n = strcspn(t + 1, " \t\n");
    if (n > 0 && n <= CCORD_SLASH_NAME_MAX_BYTES) {
      char name[64];
      memcpy(name, t + 1, n);
      name[n] = '\0';
      if (strcasecmp(name, "tts") == 0) {
        // /tts sends a text-to-speech message; the session enforces SEND_TTS_MESSAGES.
        const char *body = t + 1 + n;
        while (*body == ' ' || *body == '\t') body++;
        size_t blen = strlen(body);
        if (blen == 0) {
          ui_notice(ui, "/tts needs a message to speak");
          return;
        }
        if (!ui->d.session) {
          ui_notice(ui, "Not connected");
          return;
        }
        uint32_t tts_rem = ui_slowmode_remaining_s(ui, ch);
        if (tts_rem > 0) {
          ui_notice(ui, "Slowmode active: wait %us", (unsigned)tts_rem);
          return;
        }
        char tts_exp[CCORD_SEND_MAX_BYTES + 1];
        char tts_ser[CCORD_SEND_MAX_BYTES + 1];
        if (memchr(body, ':', blen) != nullptr) {
          blen = ui_expr_replace_shortcodes(ui->d.store, ch->guild_id, body, blen, tts_exp, sizeof tts_exp);
          body = tts_exp;
        }
        if (memchr(body, '@', blen) != nullptr || memchr(body, '#', blen) != nullptr) {
          blen = ccord_ui_serialize_mentions(ui->d.store, ch->guild_id, body, blen, tts_ser, sizeof tts_ser);
          body = tts_ser;
        }
        if (ccord_session_send_tts_message(ui->d.session, ch->id, body, blen, &err) != CCORD_OK) {
          ui_notice(ui, "Cannot send TTS: %s", err.message);
          return;
        }
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui_record_slowmode_sent(ui, ch);
        ui_dismiss_new_marker(ui, false);
        ui->msg_scroll_vel = 0.0f;
        ccord_vlist_reset_bottom(&ui->vlist);
        ui->composer_scroll = 0.0f;
        ui->dirty = true;
        return;
      }
      const ccord_slash_command *cmd = find_slash_cmd(ui, ch->guild_id, name, n);
      if (cmd) {
        if (!ui->d.session) {
          ui_notice(ui, "Not connected");
          return;
        }
        if (!slash_cmd_allowed(ui, ch, cmd)) {
          ui_notice(ui, "You do not have permission to use /%s", cmd->name);
          return;
        }
        slash_parsed sp;
        slash_parse_args(cmd, t, len, &sp);
        if (sp.has_sub_tree && sp.sub_cmd_idx < 0) {
          ui_notice(ui, "/%s requires a subcommand", cmd->name);
          return;
        }
        const char *opt_names[CCORD_SLASH_MAX_OPTIONS];
        uint8_t opt_kinds[CCORD_SLASH_MAX_OPTIONS];
        const char *opt_vals[CCORD_SLASH_MAX_OPTIONS];
        size_t nopts = 0;
        for (size_t i = 0; i < cmd->option_count; i++) {
          if (!slash_opt_in_scope(cmd, i, &sp)) continue;
          if (!sp.opt_set[i] || (sp.opt_vals[i][0] == '\0' && cmd->options[i].kind != CCORD_SLASH_OPT_STRING)) {
            if (cmd->options[i].required) {
              ui_notice(ui, "/%s requires option '%s'", cmd->name, cmd->options[i].name);
              return;
            }
            continue;
          }
          if (cmd->options[i].required && sp.opt_vals[i][0] == '\0') {
            ui_notice(ui, "/%s requires option '%s'", cmd->name, cmd->options[i].name);
            return;
          }
          char verr[128] = {0};
          if (!validate_slash_opt_val(ui->d.store, ch->guild_id, &cmd->options[i], sp.opt_vals[i], verr)) {
            ui_notice(ui, "/%s: %s", cmd->name, verr);
            return;
          }
          opt_names[nopts] = cmd->options[i].name;
          opt_kinds[nopts] = (uint8_t)cmd->options[i].kind;
          opt_vals[nopts] = sp.opt_vals[i];
          nopts++;
        }
        ccord_interaction_request ireq = {
          .type = CCORD_INTERACTION_APPLICATION_COMMAND,
          .application_id = cmd->application_id,
          .guild_id = ch->guild_id == CCORD_DM_GUILD_ID ? CCORD_ID_NONE : ch->guild_id,
          .channel_id = ch->id,
          .command_id = cmd->id,
          .command_version = cmd->version != CCORD_ID_NONE ? cmd->version : cmd->id,
          .command_name = cmd->name,
          .option_names = nopts > 0 ? opt_names : nullptr,
          .option_kinds = nopts > 0 ? opt_kinds : nullptr,
          .option_values = nopts > 0 ? opt_vals : nullptr,
          .option_count = nopts,
          .focused_option = -1,
          .command_type = 1u,
          .subcommand_group = sp.sub_group_idx >= 0 ? cmd->options[sp.sub_group_idx].name : nullptr,
          .subcommand = sp.sub_cmd_idx >= 0 ? cmd->options[sp.sub_cmd_idx].name : nullptr,
        };
        if (ccord_session_send_interaction(ui->d.session, &ireq, &err) != CCORD_OK) {
          ui_notice(ui, "/%s: %s", cmd->name, err.message);
          return;
        }
        ui->slash_auto_due_ms = 0;
        ccord_session_clear_slash_choices(ui->d.session);
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui_dismiss_new_marker(ui, false);
        ui->msg_scroll_vel = 0.0f;
        ccord_vlist_reset_bottom(&ui->vlist);
        ui->composer_scroll = 0.0f;
        ui->dirty = true;
        return;
      }
      // Dispatch plugin commands only after built-ins and Discord application commands (prevents hijacking).
      if (ui->d.contrib && ccord_contrib_find_command(ui->d.contrib, name)) {
        const char *args = t + 1 + n;
        while (*args == ' ' || *args == '\n') args++;
        if (ccord_contrib_run_command(ui->d.contrib, name, args, ch->guild_id, ch->id, &err) != CCORD_OK) {
          ui_notice(ui, "/%s: %s", name, err.message);
          return;
        }
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui->dirty = true;
        return;
      }
    }
  }

  uint32_t rem_s = ui_slowmode_remaining_s(ui, ch);
  if (rem_s > 0) {
    ui_notice(ui, "Slowmode active: wait %us", (unsigned)rem_s);
    return;
  }

  char expanded_buf[CCORD_SEND_MAX_BYTES + 1];
  char serial_buf[CCORD_SEND_MAX_BYTES + 1];
  if (len > 0 && t[0] != '/' && memchr(t, ':', len) != nullptr) {
    len = ui_expr_replace_shortcodes(ui->d.store, ch->guild_id, t, len, expanded_buf, sizeof expanded_buf);
    t = expanded_buf;
  }
  if (len > 0 && (memchr(t, '@', len) != nullptr || memchr(t, '#', len) != nullptr)) {
    len = ccord_ui_serialize_mentions(ui->d.store, ch->guild_id, t, len, serial_buf, sizeof serial_buf);
    t = serial_buf;
  }
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  if (has_files) {
    ccord_upload_file ufiles[CCORD_RICH_MAX_ATTACHMENTS];
    for (size_t i = 0; i < ui->staged_file_count; i++) {
      ufiles[i] = (ccord_upload_file){
        .filename = ui->staged_files[i].filename,
        .data = ui->staged_files[i].data,
        .size = ui->staged_files[i].size,
        .spoiler = ui->staged_files[i].spoiler,
      };
    }
    ccord_id rep_id = (ui->reply_message != CCORD_ID_NONE && ui->reply_channel == ch->id)
                        ? ui->reply_message
                        : CCORD_ID_NONE;
    if (ccord_session_send_message_with_files(ui->d.session, ch->id, rep_id, ui->reply_ping,
                                              t, len, ufiles, ui->staged_file_count, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot send attachment: %s", err.message);
      return;
    }
    ccord_ui_clear_attachment(ui);
    ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
    ui->reply_ping = true;
    ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
    ccord_text_edit_clear(&ui->edit);
    ui_clear_channel_draft(ui, ch->id);
    ui->autocomplete_open = false;
    ui->nautocomplete = 0;
    ui_record_slowmode_sent(ui, ch);
    ui_dismiss_new_marker(ui, false);
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
    ui->composer_scroll = 0.0f;
    ui->dirty = true;
    return;
  }
  if (ui->reply_message != CCORD_ID_NONE) {
    if (ccord_session_send_reply_ex(ui->d.session, ui->reply_channel, ui->reply_message, ui->reply_ping,
                                    t, len, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot reply: %s", err.message);
      return;
    }
    ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
    ui->reply_ping = true;
    ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
    ccord_text_edit_clear(&ui->edit);
    ui_clear_channel_draft(ui, ch->id);
    ui->autocomplete_open = false;
    ui->nautocomplete = 0;
    ui_record_slowmode_sent(ui, ch);
    ui_dismiss_new_marker(ui, false);
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
    ui->composer_scroll = 0.0f;
    ui->dirty = true;
    return;
  }
  if (ccord_session_send_message(ui->d.session, ch->id, t, len, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot send: %s", err.message);
    return;
  }
  ccord_text_edit_clear(&ui->edit);
  ui_clear_channel_draft(ui, ch->id);
  ui->autocomplete_open = false;
  ui->nautocomplete = 0;
  ui_record_slowmode_sent(ui, ch);
  ui_dismiss_new_marker(ui, false);
  ui->msg_scroll_vel = 0.0f;
  ccord_vlist_reset_bottom(&ui->vlist);   // follow the echoed message when it arrives
  ui->composer_scroll = 0.0f;
  ui->dirty = true;
}

void ui_send_typing(ccord_ui *ui)
{
  if (!ui->d.session || ui->editing_message != CCORD_ID_NONE) return;
  if (ui->edit.len > 0 && ui->edit.data[0] == '/') return;   // typing a slash command does not broadcast typing
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) return;
  if (ui_slowmode_remaining_s(ui, ch) > 0) return;
  uint64_t now = ui->now_ms;
  if (ui->typing_channel == ch->id && now >= ui->last_typing_ms &&
      now - ui->last_typing_ms < UI_TYPING_THROTTLE_MS) {
    return;
  }
  ccord_error err;
  if (ccord_session_send_typing(ui->d.session, ch->id, &err) == CCORD_OK) {
    ui->typing_channel = ch->id;
    ui->last_typing_ms = now;
  }
}

void ui_begin_edit(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  const ccord_message *m = ccord_store_find_message(ui->d.store, channel_id, message_id);
  if (!m) {
    ui_notice(ui, "Message is no longer available");
    return;
  }
  char clean_buf[CCORD_MESSAGE_MAX_BYTES + 1];
  size_t len = ui_message_clean_body(ui, m, clean_buf, sizeof clean_buf);
  if (len > ui->edit.max_bytes) {
    ui_notice(ui, "Message is too long to edit here");
    return;
  }
  ccord_text_edit_clear(&ui->edit);
  ccord_error err;
  if (ccord_text_edit_insert(&ui->edit, clean_buf, len, &err) != CCORD_OK) {
    ui_notice(ui, "%s", err.message);
    return;
  }
  ccord_text_edit_set_caret(&ui->edit, ui->edit.len, false);
  ui->editing_channel = channel_id;
  ui->editing_message = message_id;
  ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
  ui->composer_scroll = 0.0f;
  ui->caret_epoch_ms = ui->now_ms;
  ui->caret_on = true;
  ui->dirty = true;
}

void ui_cancel_edit(ccord_ui *ui)
{
  if (ui->editing_message == CCORD_ID_NONE) return;
  ui->editing_channel = ui->editing_message = CCORD_ID_NONE;
  ccord_text_edit_clear(&ui->edit);
  ui->composer_scroll = 0.0f;
  ui->dirty = true;
}

void ui_begin_reply(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  const ccord_message *m = ccord_store_find_message(ui->d.store, channel_id, message_id);
  if (!m) {
    ui_notice(ui, "Message is no longer available");
    return;
  }
  ui_cancel_edit(ui);
  ui->reply_channel = channel_id;
  ui->reply_message = message_id;
  ui->reply_ping = true;
  ui->dirty = true;
}

void ui_cancel_reply(ccord_ui *ui)
{
  if (ui->reply_message == CCORD_ID_NONE) return;
  ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
  ui->reply_ping = true;
  ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
  ui->dirty = true;
}

// ---------------------------------------------------------------- voice / stream stage

static const char *encoder_label(int v)
{
  static const char *const names[] = {
    "NVENC", "VAAPI", "AMF", "Vulkan", "OpenH264", "x264", "Auto", "Quick Sync", "Media Found.", "VideoToolbox",
  };
  return v >= 0 && v < 10 ? names[v] : "Auto";
}

static const char *codec_label(int v)
{
  static const char *const names[] = {"H.264", "HEVC", "AV1"};
  return v >= 0 && v < 3 ? names[v] : "?";
}

[[maybe_unused]] static const char *framework_label(int v)
{
  static const char *const names[] = {
    "PipeWire", "DMA-BUF", "NVFBC", "Vulkan", "X11", "Auto", "Win. Capture", "DXGI Dup", "GDI", "ScreenCaptureKit", "CoreGraphics",
  };
  return v >= 0 && v < 11 ? names[v] : "Auto";
}

static const char *resolution_label(int v)
{
  static const char *const names[] = {"720p", "1080p", "1440p", "Source"};
  return v >= 0 && v < 4 ? names[v] : "?";
}

static const char *fps_label(int v)
{
  static const char *const names[] = {"15 FPS", "30 FPS", "60 FPS"};
  return v >= 0 && v < 3 ? names[v] : "?";
}

// ---- M5 stage-view controls (m5-ui-surfaces) ----------------------------------------------------
// Owner-tracked static state: struct ccord_ui is owned by ui.c and cannot grow fields. The rects are
// rebuilt every draw; clicks are matched in ui_voice_stage_handle. Nothing here writes Discord state
// locally — status/topic/speaker actions are session intents, volume/local-mute are client prefs.
typedef struct ui_stage_state {
  const ccord_ui *owner;
  ccord_rect status_edit, topic_edit, request_speak;
  ccord_rect stage_start, stage_end;   // lifecycle chips (zero when not offered)
  bool stage_active;                   // an active stage instance exists
  bool request_speak_cancel;
  size_t speaker_count, audience_count;
  size_t ctl_count;
  struct {
    ccord_id user_id;
    ccord_rect tile, slider, mute, mod;
    bool mod_is_invite;
  } ctl[UI_STAGE_CTL_MAX];
  size_t stream_cards;
  struct {
    ccord_id user_id;
    uint32_t viewers;
    ccord_rect rect, watch;
    bool watching;
  } card[UI_STREAM_CARD_MAX];
  char stream_badge[16];
  char stream_title[192];
  char stream_placeholder[64];
  bool stream_frame_present;
  int stream_frame_w, stream_frame_h;
  int cam_cols;
  size_t cam_tile_count;
  struct {
    ccord_id user_id;
    ccord_rect rect, video;
    char key[40];
    bool textured;
    int tex_w, tex_h;
    bool limit, connecting, self;
  } cam_tile[UI_CAM_TILE_MAX];
  ccord_rect stream_player, stage_view;
  ccord_rect stream_resize_handle, stream_fs_btn, stream_vport;
} ui_stage_state;

static ui_stage_state g_stage;

// What stage_tile needs to draw a camera tile.
typedef struct ui_cam_tile_info {
  ccord_rect video;
  char key[40];
  bool textured, limit, self;
} ui_cam_tile_info;

static ui_stage_state *stage_state(const ccord_ui *ui)
{
  if (g_stage.owner != ui) {
    memset(&g_stage, 0, sizeof g_stage);
    g_stage.owner = ui;
  }
  return &g_stage;
}

void ui_stage_get_debug(const ccord_ui *ui, ui_stage_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  const ui_stage_state *st = stage_state(ui);
  out->status_edit = st->status_edit;
  out->topic_edit = st->topic_edit;
  out->request_speak = st->request_speak;
  out->request_speak_cancel = st->request_speak_cancel;
  out->speaker_count = st->speaker_count;
  out->audience_count = st->audience_count;
  out->ctl_count = st->ctl_count;
  for (size_t i = 0; i < st->ctl_count && i < UI_STAGE_CTL_MAX; i++) {
    out->ctl[i].user_id = st->ctl[i].user_id;
    out->ctl[i].slider = st->ctl[i].slider;
    out->ctl[i].mute = st->ctl[i].mute;
    out->ctl[i].mod = st->ctl[i].mod;
    out->ctl[i].mod_is_invite = st->ctl[i].mod_is_invite;
  }
  out->stream_cards = st->stream_cards;
  for (size_t i = 0; i < st->stream_cards && i < UI_STREAM_CARD_MAX; i++) {
    out->card[i].user_id = st->card[i].user_id;
    out->card[i].viewers = st->card[i].viewers;
    out->card[i].watch = st->card[i].watch;
    out->card[i].watching = st->card[i].watching;
  }
  memcpy(out->stream_badge, st->stream_badge, sizeof out->stream_badge);
  memcpy(out->stream_title, st->stream_title, sizeof out->stream_title);
  memcpy(out->stream_placeholder, st->stream_placeholder, sizeof out->stream_placeholder);
  out->stream_frame_present = st->stream_frame_present;
  out->stream_frame_w = st->stream_frame_w;
  out->stream_frame_h = st->stream_frame_h;
  out->cam_cols = st->cam_cols;
  out->cam_tile_count = st->cam_tile_count;
  for (size_t i = 0; i < st->cam_tile_count && i < UI_CAM_TILE_MAX; i++) {
    out->cam_tile[i].user_id = st->cam_tile[i].user_id;
    out->cam_tile[i].rect = st->cam_tile[i].rect;
    out->cam_tile[i].video = st->cam_tile[i].video;
    memcpy(out->cam_tile[i].key, st->cam_tile[i].key, sizeof out->cam_tile[i].key);
    out->cam_tile[i].textured = st->cam_tile[i].textured;
    out->cam_tile[i].tex_w = st->cam_tile[i].tex_w;
    out->cam_tile[i].tex_h = st->cam_tile[i].tex_h;
    out->cam_tile[i].limit = st->cam_tile[i].limit;
    out->cam_tile[i].connecting = st->cam_tile[i].connecting;
    out->cam_tile[i].self = st->cam_tile[i].self;
  }
  out->stream_player = st->stream_player;
  out->stage_view = st->stage_view;
  out->stream_resize_handle = st->stream_resize_handle;
  out->stream_fs_btn = st->stream_fs_btn;
  out->stream_vport = st->stream_vport;
  out->stage_split_handle = ui_stage_split_handle_rect(ui);
  out->stream_fullscreen = ui->stream_fullscreen;
  out->stream_fs_exit_btn = ui->stream_fs_exit_rect;
  out->stream_fs_video = ui->stream_fs_video_rect;
  out->stream_pip_visible = ui->stream_pip_rect.w > 0.0f && ui->stream_pip_rect.h > 0.0f;
  out->stream_pip_rect = ui->stream_pip_rect;
  out->stream_pip_fs_btn = ui->stream_pip_fs_rect;
  out->stream_pip_stage_btn = ui->stream_pip_stage_rect;
  out->stream_pip_close_btn = ui->stream_pip_close_rect;
}

void ui_stage_lifecycle_get_debug(const ccord_ui *ui, ui_stage_lifecycle_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  const ui_stage_state *st = stage_state(ui);
  out->start_stage = st->stage_start;
  out->end_stage = st->stage_end;
  out->stage_active = st->stage_active;
}

// ---- Embedded Activities (shelf modal + voice-stage instance cards) -----------------------------
// Owner-tracked static state like g_stage: struct ccord_ui is owned by ui.c. The geometry is rebuilt
// every draw; clicks are matched in ui_voice_stage_handle (stage) / ui_activity_shelf_handle (modal).
// Launch / join / leave are session intents — instances only appear once the transport answers.
#define ACT_ROW_H 54.0f        // one shelf entry in the modal
#define ACT_CARD_H 56.0f       // one running instance in the stage view
#define ACT_CARD_GAP 8.0f
#define ACT_BANNER_H 22.0f
typedef struct ui_activity_state {
  const ccord_ui *owner;
  // Persistent across draws (reset when the owner changes or the modal reopens).
  ccord_id guild_id, channel_id;       // the shelf modal's target
  float list_scroll;
  ccord_scroller list_glide;   // wheel glide of the list (scroll.h)
  size_t sel;                          // keyboard selection in the shelf list
  float gutter;                        // right-hand strip of the list the scrollbar owns (0 when it all fits)
  uint64_t opened_ms;                  // when the shelf opened (switches "Loading" to "No activities")
  char load_error[128];                // the shelf request was refused on open; "" otherwise
  uint64_t shelf_retry_ms;             // stage view: earliest time for the next shelf request (app names)
  // Per-draw geometry.
  ccord_rect chip;                     // "Activities" chip in the stage control row
  bool banner_visible;
  size_t instance_count;
  struct {
    ccord_id instance_id, application_id;
    ccord_rect card, join, leave, open_browser;
    bool self_in;
    size_t participant_count;
  } inst[UI_ACTIVITY_INST_MAX];
  ccord_rect box, close_btn, list_view, list_thumb;
  float list_content_h;
  size_t row_count;
  struct {
    ccord_id application_id;
    ccord_rect row;
    char name[CCORD_NAME_MAX_BYTES + 1];
    char description[CCORD_ACTIVITY_DESC_MAX_BYTES + 1];
  } rows[UI_ACTIVITY_SHELF_MAX];
} ui_activity_state;

static ui_activity_state g_activity;

static ui_activity_state *activity_state(const ccord_ui *ui)
{
  if (g_activity.owner != ui) {
    memset(&g_activity, 0, sizeof g_activity);
    g_activity.owner = ui;
  }
  return &g_activity;
}

// The shelf app behind an instance (nullptr until the guild's shelf is cached).
static const ccord_activity_app *activity_app_of(const ccord_ui *ui, ccord_id guild_id, ccord_id application_id)
{
  if (!ui->d.session) return nullptr;
  size_t n = 0;
  const ccord_activity_app *apps = ccord_session_activity_shelf(ui->d.session, guild_id, &n);
  for (size_t i = 0; i < n; i++) {
    if (apps[i].application_id == application_id) return &apps[i];
  }
  return nullptr;
}

static void activity_shelf_close(ccord_ui *ui)
{
  if (ui->modal_kind == UI_MODAL_ACTIVITY_SHELF) ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
}

static const ccord_activity_app *activity_shelf_apps(const ccord_ui *ui, ccord_id guild_id, size_t *n)
{
  *n = 0;
  return ui->d.session ? ccord_session_activity_shelf(ui->d.session, guild_id, n) : nullptr;
}

// Wheel glides of the activity shelf list and the member verification form (scroll.h), ticked by ccord_ui_update.
// A glide is dropped when its surface is shut.
bool ui_activity_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  ui_activity_state *st = activity_state(ui);
  if (st->list_glide.pending == 0.0f) return false;
  if (!ui_activity_shelf_active(ui)) {
    ccord_scroller_cancel(&st->list_glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&st->list_glide, &st->list_scroll, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

bool ui_mv_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  ui_member_verification *mv = &ui->mv;
  if (mv->glide.pending == 0.0f) return false;
  if (ui->modal_kind != UI_MODAL_MEMBER_VERIFICATION) {
    ccord_scroller_cancel(&mv->glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&mv->glide, &mv->scroll, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

void ui_activity_shelf_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  if (!ui || guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE) return;
  if (ui->modal_kind != UI_MODAL_NONE) return;
  ui_activity_state *st = activity_state(ui);
  st->guild_id = guild_id;
  st->channel_id = channel_id;
  st->list_scroll = 0.0f;
  st->sel = 0;
  st->opened_ms = ui->now_ms;
  st->load_error[0] = '\0';
  ui->modal_kind = UI_MODAL_ACTIVITY_SHELF;
  ui->dirty = true;
  // Ask for the shelf; the answer lands in the session cache through pump(). Cached entries (if
  // any) stay visible while it is in flight.
  if (ui->d.session) {
    ccord_error err;
    if (ccord_session_activity_shelf_fetch(ui->d.session, guild_id, &err) != CCORD_OK) {
      snprintf(st->load_error, sizeof st->load_error, "%.127s", err.message);
      ui_notice(ui, "Couldn't load activities: %s", err.message);
    }
  }
}

bool ui_activity_shelf_active(const ccord_ui *ui)
{
  return ui && ui->modal_kind == UI_MODAL_ACTIVITY_SHELF;
}

// Launches (and joins) the app in the voice channel. Shared by the shelf rows and the Join button.
static void activity_launch(ccord_ui *ui, ccord_id channel_id, ccord_id application_id)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_activity_launch(ui->d.session, channel_id, application_id, &err) == CCORD_OK) {
    ui_notice(ui, "Starting activity...");
  } else {
    ui_notice(ui, "Cannot start activity: %s", err.message);
  }
  ui->dirty = true;
}

void ui_activity_get_debug(const ccord_ui *ui, ui_activity_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  const ui_activity_state *st = activity_state(ui);
  out->chip = st->chip;
  out->banner_visible = st->banner_visible;
  out->instance_count = st->instance_count;
  for (size_t i = 0; i < st->instance_count && i < UI_ACTIVITY_INST_MAX; i++) {
    out->instances[i].instance_id = st->inst[i].instance_id;
    out->instances[i].application_id = st->inst[i].application_id;
    out->instances[i].card = st->inst[i].card;
    out->instances[i].join = st->inst[i].join;
    out->instances[i].leave = st->inst[i].leave;
    out->instances[i].open_browser = st->inst[i].open_browser;
    out->instances[i].self_in = st->inst[i].self_in;
    out->instances[i].participant_count = st->inst[i].participant_count;
  }
  out->open = ui_activity_shelf_active(ui);
  out->guild_id = st->guild_id;
  out->channel_id = st->channel_id;
  out->box = st->box;
  out->close_btn = st->close_btn;
  out->list_view = st->list_view;
  out->list_thumb = st->list_thumb;
  out->list_scroll = st->list_scroll;
  out->list_content_h = st->list_content_h;
  out->row_count = st->row_count;
  out->sel = st->sel;
  out->gutter = st->gutter;
  memcpy(out->load_error, st->load_error, sizeof out->load_error);
  for (size_t i = 0; i < st->row_count && i < UI_ACTIVITY_SHELF_MAX; i++) {
    out->rows[i].application_id = st->rows[i].application_id;
    out->rows[i].row = st->rows[i].row;
    memcpy(out->rows[i].name, st->rows[i].name, sizeof out->rows[i].name);
    memcpy(out->rows[i].description, st->rows[i].description, sizeof out->rows[i].description);
  }
}


// Starts the shelf entry `idx` in the modal's voice channel and closes the modal.
static void activity_shelf_pick(ccord_ui *ui, size_t idx)
{
  ui_activity_state *st = activity_state(ui);
  size_t n = 0;
  const ccord_activity_app *apps = activity_shelf_apps(ui, st->guild_id, &n);
  if (idx >= n) return;
  ccord_id channel_id = st->channel_id, app_id = apps[idx].application_id;
  activity_shelf_close(ui);
  activity_launch(ui, channel_id, app_id);
}

void ui_activity_shelf_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_activity_state *st = activity_state(ui);
  st->box = st->close_btn = st->list_view = st->list_thumb = (ccord_rect){0, 0, 0, 0};
  st->list_content_h = 0.0f;
  st->gutter = 0.0f;
  st->row_count = 0;
  for (size_t i = 0; i < UI_ACTIVITY_SHELF_MAX; i++) st->rows[i].row = (ccord_rect){0, 0, 0, 0};
  if (!ui_activity_shelf_active(ui)) return;

  size_t n = 0;
  const ccord_activity_app *apps = activity_shelf_apps(ui, st->guild_id, &n);
  st->row_count = n;
  if (st->sel >= n) st->sel = n > 0 ? n - 1u : 0u;

  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));
  float w = 460.0f, h = 480.0f;
  if (w > ui->win_w - 32.0f && ui->win_w > 64.0f) w = ui->win_w - 32.0f;
  if (h > ui->win_h - 32.0f && ui->win_h > 64.0f) h = ui->win_h - 32.0f;
  ccord_rect box = {(ui->win_w - w) * 0.5f, (ui->win_h - h) * 0.5f, w, h};
  st->box = box;

  ui_popup_begin(ui, r, "actshelf", 0, UI_MODAL_MS, 8.0f);
  ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, 9.0f, COL(border_subtle));
  ccord_draw_rrect(r, box, 8.0f, COL(bg_floating));

  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, "Activities", 10, box.x + 20.0f, box.y + 16.0f, &ts, 0.0f);
  ccord_rect close_r = {box.x + box.w - 40.0f, box.y + 12.0f, 28.0f, 28.0f};
  st->close_btn = close_r;
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, close_r);
  ccord_draw_rrect(r, close_r, 5.0f, chv ? COL(bg_item_hover) : COL(bg_secondary));
  ui_icon_close(r, close_r.x + close_r.w * 0.5f, close_r.y + close_r.h * 0.5f, 13.0f, COL(text_secondary));

  const ccord_channel *ch = ccord_store_channel(ui->d.store, st->channel_id);
  char sub[160];
  snprintf(sub, sizeof sub, "Start an activity in %s", ch ? ch->name : "this voice channel");
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), box.x + 20.0f, box.y + 46.0f, box.w - 70.0f, &ss);

  // Footer note: activities are not embedded.
  const char *note = "Activities run in your browser. C-Cord shows you as a participant.";
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  float note_y = box.y + box.h - 28.0f;
  ccord_text_draw_ellipsis(r, note, strlen(note), box.x + 20.0f, note_y, box.w - 40.0f, &ns);

  float list_x = box.x + 12.0f, list_y = box.y + 72.0f, list_w = box.w - 24.0f;
  float list_h = note_y - 8.0f - list_y;
  if (list_h < 0.0f) list_h = 0.0f;
  ccord_rect view = {list_x, list_y, list_w, list_h};
  st->list_view = view;
  st->list_content_h = (float)n * ACT_ROW_H;
  float max_s = st->list_content_h > list_h ? st->list_content_h - list_h : 0.0f;
  st->list_scroll = clampf(st->list_scroll, 0.0f, max_s);
  // The scrollbar overlays the list's right edge: rows stop short of it and clicks there scroll, never launch.
  st->gutter = st->list_content_h > list_h + 0.5f ? M(scrollbar_width) + 8.0f : 0.0f;

  ccord_push_clip(r, view);
  if (n == 0) {
    bool loading = ui->now_ms < st->opened_ms + 4000u;
    const char *empty = st->load_error[0] ? "Couldn't load activities. Close and reopen to try again."
                        : (loading ? "Loading activities..." : "No activities are available in this server.");
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, 0.0f);
    ccord_text_draw(r, empty, strlen(empty), list_x + (list_w - et.x) * 0.5f, list_y + (list_h - et.y) * 0.45f, &es, 0.0f);
  }
  ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style ds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  for (size_t i = 0; i < n; i++) {
    float cy = list_y + (float)i * ACT_ROW_H - st->list_scroll;
    bool visible = cy + ACT_ROW_H >= list_y && cy <= list_y + list_h;
    if (i < UI_ACTIVITY_SHELF_MAX) {
      st->rows[i].application_id = apps[i].application_id;
      snprintf(st->rows[i].name, sizeof st->rows[i].name, "%s", apps[i].name);
      snprintf(st->rows[i].description, sizeof st->rows[i].description, "%s", apps[i].description);
    }
    if (!visible) continue;
    ccord_rect row = {list_x, cy, list_w - st->gutter, ACT_ROW_H - 4.0f};
    if (i < UI_ACTIVITY_SHELF_MAX) st->rows[i].row = row;
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, row) && ui_in_rect(ui->mx, ui->my, view);
    ccord_draw_rrect(r, row, 5.0f, i == st->sel ? COL(bg_item_selected) : (hov ? COL(bg_item_hover) : COL(bg_secondary)));
    ccord_text_draw_ellipsis(r, apps[i].name, strlen(apps[i].name), row.x + 12.0f, row.y + 8.0f, row.w - 24.0f, &as);
    const char *desc = apps[i].description[0] ? apps[i].description : "No description";
    ccord_text_draw_ellipsis(r, desc, strlen(desc), row.x + 12.0f, row.y + 28.0f, row.w - 24.0f, &ds);
  }
  ccord_pop_clip(r);
  st->list_thumb = ui_draw_list_scrollbar(ui, r, view, st->list_content_h, st->list_scroll, "actshelfscroll", 0);
  ui_popup_end(r);
}

// The shelf entry under (x, y), or SIZE_MAX.
static size_t activity_shelf_row_at(const ui_activity_state *st, float x, float y)
{
  if (st->list_view.w <= 0.0f || !ui_in_rect(x, y, st->list_view) || x >= st->list_view.x + st->list_view.w - st->gutter) {
    return SIZE_MAX;
  }
  size_t idx = (size_t)((y - st->list_view.y + st->list_scroll) / ACT_ROW_H);
  return idx < st->row_count ? idx : SIZE_MAX;
}

bool ui_activity_shelf_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_activity_shelf_active(ui) || !ev) return false;
  ui_activity_state *st = activity_state(ui);
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      activity_shelf_close(ui);
    } else if (ev->key == CCORD_KEY_DOWN && st->row_count > 0) {
      st->sel = (st->sel + 1u) % st->row_count;
      ccord_scroller_cancel(&st->list_glide);   // a key step replaces a wheel glide in flight (scroll.h)
      st->list_scroll = clampf(st->list_scroll, ((float)st->sel + 1.0f) * ACT_ROW_H - st->list_view.h,
                               (float)st->sel * ACT_ROW_H);
      ui->dirty = true;
    } else if (ev->key == CCORD_KEY_UP && st->row_count > 0) {
      st->sel = (st->sel + st->row_count - 1u) % st->row_count;
      ccord_scroller_cancel(&st->list_glide);   // a key step replaces a wheel glide in flight (scroll.h)
      st->list_scroll = clampf(st->list_scroll, ((float)st->sel + 1.0f) * ACT_ROW_H - st->list_view.h,
                               (float)st->sel * ACT_ROW_H);
      ui->dirty = true;
    } else if (ev->key == CCORD_KEY_RETURN) {
      activity_shelf_pick(ui, st->sel);
    }
    return true;
  }
  if (ev->type == CCORD_IN_WHEEL) {
    float notch = 3.0f * ui_line_h(ui, M(font_size_base));
    float max_s = st->list_content_h > st->list_view.h ? st->list_content_h - st->list_view.h : 0.0f;
    ccord_scroller_scroll(&st->list_glide, &st->list_scroll, -ev->wheel * notch, max_s, ui_wheel_direct(ui, ev));
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->close_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, st->close_btn)) {
      activity_shelf_close(ui);
      return true;
    }
    if (st->gutter > 0.0f && ui_in_rect(ev->x, ev->y, st->list_view) &&
        ev->x >= st->list_view.x + st->list_view.w - st->gutter) {
      float max_s = st->list_content_h > st->list_view.h ? st->list_content_h - st->list_view.h : 0.0f;
      ccord_scroller_cancel(&st->list_glide);   // a gutter press replaces any wheel glide (scroll.h)
      st->list_scroll = clampf((ev->y - st->list_view.y) / st->list_view.h * max_s, 0.0f, max_s);
      ui->dirty = true;
      return true;
    }
    size_t idx = activity_shelf_row_at(st, ev->x, ev->y);
    if (idx != SIZE_MAX) {
      activity_shelf_pick(ui, idx);
      return true;
    }
    if (st->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->box)) activity_shelf_close(ui);
    return true;
  }
  return true;
}

ccord_cursor_kind ui_activity_shelf_cursor(const ccord_ui *ui)
{
  if (!ui_activity_shelf_active(ui) || !ui->mouse_in) return CCORD_CURSOR_DEFAULT;
  const ui_activity_state *st = activity_state(ui);
  if (st->close_btn.w > 0.0f && ui_in_rect(ui->mx, ui->my, st->close_btn)) return CCORD_CURSOR_POINTER;
  return activity_shelf_row_at(st, ui->mx, ui->my) != SIZE_MAX ? CCORD_CURSOR_POINTER : CCORD_CURSOR_DEFAULT;
}

// A small labelled chip button used for the status / topic / request-to-speak row.
static ccord_rect stage_chip(ccord_ui *ui, ccord_renderer *r, float x, float cy, const char *label,
                             ccord_color bg, ccord_color fg, float max_w)
{
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, fg);
  ccord_vec2 bt = ccord_text_measure(r, label, strlen(label), &bs, max_w - 18.0f);
  float bw = bt.x + 18.0f;
  if (bw > max_w) bw = max_w;
  ccord_rect br = {x, cy - 12.0f, bw, 24.0f};
  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, br);
  ccord_draw_rrect(r, br, 4.0f, hov ? ui_mix(bg, COL(bg_item_hover), 0.25f) : bg);
  ccord_text_draw_ellipsis(r, label, strlen(label), br.x + 9.0f, br.y + (br.h - bt.y) * 0.5f, br.w - 18.0f, &bs);
  return br;
}

// ---- Embedded Activities: running instances in the stage view -----------------------------------
// One card per running instance of this voice channel, above the participant tiles. The activity
// itself runs in the browser (nothing is embedded): the card's own-instance row offers Leave and
// Open in browser, instances the user is not in offer Join (which launches / joins through the session).

static size_t activity_instance_total(const ccord_ui *ui, const ccord_channel *ch, const ccord_activity_instance **out)
{
  size_t n = 0;
  *out = ui->d.session ? ccord_session_activity_instances(ui->d.session, ch->id, &n) : nullptr;
  if (!*out) return 0;
  return n < UI_ACTIVITY_INST_MAX ? n : UI_ACTIVITY_INST_MAX;
}

// Vertical space the block takes inside the stage scroll area (0 when nothing runs).
static float activity_block_h(size_t n)
{
  return n == 0 ? 0.0f : 18.0f + ACT_BANNER_H + (float)n * (ACT_CARD_H + ACT_CARD_GAP) + 6.0f;
}

// https://discord.com/channels/<guild>/<channel>: where the activity runs ("@me" for a DM call).
static void activity_channel_url(const ccord_channel *ch, char *out, size_t cap)
{
  if (ch->guild_id == CCORD_DM_GUILD_ID) {
    snprintf(out, cap, "https://discord.com/channels/@me/%llu", (unsigned long long)ch->id);
  } else {
    snprintf(out, cap, "https://discord.com/channels/%llu/%llu", (unsigned long long)ch->guild_id,
             (unsigned long long)ch->id);
  }
}

static ccord_rect activity_button(ccord_ui *ui, ccord_renderer *r, float right, float cy, float w, const char *label,
                                  ccord_color bg, ccord_color fg)
{
  ccord_rect b = {right - w, cy - 12.0f, w, 24.0f};
  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b);
  ccord_draw_rrect(r, b, 4.0f, hov ? ui_mix(bg, COL(bg_item_hover), 0.25f) : bg);
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, fg);
  ccord_vec2 bt = ccord_text_measure(r, label, strlen(label), &bs, 0.0f);
  ccord_text_draw(r, label, strlen(label), b.x + (b.w - bt.x) * 0.5f, b.y + (b.h - bt.y) * 0.5f, &bs, 0.0f);
  return b;
}

// The part of `a` inside `c` (zero size when they do not overlap): a half-scrolled card keeps its visible
// buttons clickable, but never outside the viewport (where they would shadow the control row).
static ccord_rect activity_clip_rect(ccord_rect a, ccord_rect c)
{
  float x0 = maxf(a.x, c.x), y0 = maxf(a.y, c.y);
  float x1 = a.x + a.w < c.x + c.w ? a.x + a.w : c.x + c.w;
  float y1 = a.y + a.h < c.y + c.h ? a.y + a.h : c.y + c.h;
  if (x1 <= x0 || y1 <= y0) return (ccord_rect){0, 0, 0, 0};
  return (ccord_rect){x0, y0, x1 - x0, y1 - y0};
}

// Draws the block at (x, y) with width w and records the per-instance geometry, clipped to `clip` (the
// stage scroll viewport).
static void activity_draw_block(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch,
                                const ccord_activity_instance *inst, size_t n, float x, float y, float w,
                                ccord_rect clip)
{
  if (n == 0) return;
  ui_activity_state *st = activity_state(ui);
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  st->banner_visible = true;
  st->instance_count = n;
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  char hdr[48];
  snprintf(hdr, sizeof hdr, "ACTIVITIES  ·  %zu running", n);
  ccord_text_draw(r, hdr, strlen(hdr), x, y, &hs, 0.0f);
  y += 18.0f;
  const char *banner = "Activities run in your browser. C-Cord shows you as a participant.";
  ccord_text_style bns = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, banner, strlen(banner), x, y + 2.0f, w, &bns);
  y += ACT_BANNER_H;

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
  for (size_t i = 0; i < n; i++, y += ACT_CARD_H + ACT_CARD_GAP) {
    const ccord_activity_instance *in = &inst[i];
    size_t pc = in->participant_count < CCORD_ACTIVITY_MAX_PARTICIPANTS ? in->participant_count
                                                                        : CCORD_ACTIVITY_MAX_PARTICIPANTS;
    bool self_in = false;
    for (size_t p = 0; p < pc; p++) self_in = self_in || in->participants[p] == self_id;
    st->inst[i].instance_id = in->instance_id;
    st->inst[i].application_id = in->application_id;
    st->inst[i].self_in = self_in;
    st->inst[i].participant_count = in->participant_count;
    st->inst[i].card = st->inst[i].join = st->inst[i].leave = st->inst[i].open_browser = (ccord_rect){0, 0, 0, 0};
    ccord_rect card = {x, y, w, ACT_CARD_H};
    if (card.y + card.h < clip.y || card.y > clip.y + clip.h) continue;
    ccord_draw_rrect(r, card, 6.0f, COL(bg_secondary));
    st->inst[i].card = activity_clip_rect(card, clip);

    const ccord_activity_app *app = activity_app_of(ui, ch->guild_id, in->application_id);
    if (!app && ui->d.session && ui->now_ms >= st->shelf_retry_ms) {
      // Resolve the app name through the guild's shelf; the answer lands in the session cache through pump().
      // Asked again every few seconds while the name is unknown (a refused or failed request is retried; the
      // session answers repeat calls from its cache or ignores them while one is loading).
      ccord_error serr;
      st->shelf_retry_ms = ui->now_ms + 15000u;
      (void)ccord_session_activity_shelf_fetch(ui->d.session, ch->guild_id, &serr);
    }
    const char *name = app && app->name[0] ? app->name : "Activity";

    // Buttons, right-aligned: [Open in browser][Leave] for the user's own instance, [Join] otherwise.
    float right = card.x + card.w - 12.0f, bcy = card.y + card.h * 0.5f, btn_left = right;
    ccord_rect join = {0, 0, 0, 0}, leave = {0, 0, 0, 0}, open = {0, 0, 0, 0};
    if (self_in) {
      leave = activity_button(ui, r, right, bcy, 70.0f, "Leave", COL(danger), COL(text_on_brand));
      open = activity_button(ui, r, leave.x - 8.0f, bcy, 130.0f, "Open in browser", COL(brand), COL(text_on_brand));
      btn_left = open.x;
    } else {
      join = activity_button(ui, r, right, bcy, 70.0f, "Join", COL(brand), COL(text_on_brand));
      btn_left = join.x;
    }
    st->inst[i].join = activity_clip_rect(join, clip);
    st->inst[i].leave = activity_clip_rect(leave, clip);
    st->inst[i].open_browser = activity_clip_rect(open, clip);

    float text_w = btn_left - 12.0f - (card.x + 12.0f);
    if (text_w < 40.0f) text_w = 40.0f;
    ccord_text_draw_ellipsis(r, name, strlen(name), card.x + 12.0f, card.y + 7.0f, text_w, &ns);
    // Participants: a row of small avatars, then the first names and the overflow count.
    float ax = card.x + 12.0f + 10.0f, ay = card.y + 40.0f;
    size_t shown = pc < 5u ? pc : 5u;
    for (size_t p = 0; p < shown; p++) {
      ui_draw_avatar(ui, r, ccord_store_user(ui->d.store, in->participants[p]), ax + (float)p * 18.0f, ay, 16.0f,
                     COL(bg_secondary), false);
    }
    char who[160];
    size_t off = 0;
    who[0] = '\0';
    for (size_t p = 0; p < pc && p < 3u; p++) {
      const ccord_user *pu = ccord_store_user(ui->d.store, in->participants[p]);
      off += (size_t)snprintf(who + off, sizeof who - off, "%s%s", p ? ", " : "", pu ? pu->name : "Unknown User");
      if (off >= sizeof who) off = sizeof who - 1u;
    }
    if (pc > 3u) snprintf(who + off, sizeof who - off, " +%zu", pc - 3u);
    char line[192];
    snprintf(line, sizeof line, "%zu in activity  ·  %s", pc, who);
    float lx = ax + (float)shown * 18.0f + 6.0f;
    float line_w = card.x + 12.0f + text_w - lx;
    if (line_w > 24.0f) ccord_text_draw_ellipsis(r, line, strlen(line), lx, ay - 7.0f, line_w, &ps);
  }
}

// One participant tile. Registers the per-user controls (stage mod button, local mute, volume slider)
// in g_stage and keeps the legacy stage_watch_* arrays for the existing watch-stream path. Controls
// are only registered when the whole tile is inside `clip`: half-scrolled tiles must not catch hits.
static void stage_tile(ccord_ui *ui, ccord_renderer *r, const ccord_voice_state *vs, ccord_rect tile,
                       bool watching_remote, const ccord_watched_stream *ws, bool is_stage, bool can_mod,
                       ccord_rect clip, const ui_cam_tile_info *cam)
{
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool is_self = vs->user_id == self_id;
  bool record = tile.y >= clip.y && tile.y + tile.h <= clip.y + clip.h;
  bool speaking = ccord_store_user_speaking(ui->d.store, vs->user_id) && !vs->self_mute && !vs->suppress;
  // Camera tile: 6 px padding, a 16:9 video area, then the 82 px footer at dy = tile_h - 150.
  float dy = cam ? tile.h - 150.0f : 0.0f;
  float ty = tile.y + dy;
  if (speaking && !cam) {
    ccord_draw_rrect(r, (ccord_rect){tile.x - 2.0f, tile.y - 2.0f, tile.w + 4.0f, tile.h + 4.0f}, 8.0f,
                     COL(status_online));
  }
  ccord_color bg = is_self ? COL(bg_item_selected) : COL(bg_secondary);
  ccord_draw_rrect(r, tile, 6.0f, bg);
  const ccord_user *u = ccord_store_user(ui->d.store, vs->user_id);
  if (cam) {
    ccord_rect vid = cam->video;
    if (speaking) {
      ccord_draw_rrect(r, (ccord_rect){vid.x - 2.0f, vid.y - 2.0f, vid.w + 4.0f, vid.h + 4.0f}, 8.0f,
                       COL(status_online));
    }
    if (cam->textured) {
      ccord_draw_texture_cover(r, cam->key, vid, 6.0f, 255, false);
    } else {
      ccord_draw_rrect(r, vid, 6.0f, COL(bg_tertiary));
      float acy = vid.y + vid.h * 0.5f - 8.0f;
      ui_draw_avatar(ui, r, u, vid.x + vid.w * 0.5f, acy, 52.0f, (ccord_color){0, 0, 0, 0}, false);
      const char *msg = cam->self ? "Your camera is on" : (cam->limit ? "Video limit (4)" : "Connecting video\xE2\x80\xA6");
      ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_vec2 cm = ccord_text_measure(r, msg, strlen(msg), &cs, 0.0f);
      ccord_text_draw(r, msg, strlen(msg), vid.x + (vid.w - cm.x) * 0.5f, acy + 32.0f, &cs, 0.0f);
    }
  } else if (vs->self_stream && ccord_render_has_texture(r, "__video_stream__", nullptr, nullptr) &&
      (is_self || (watching_remote && ws->user_id == vs->user_id))) {
    ccord_rect preview = {tile.x + 6.0f, tile.y + 6.0f, tile.w - 12.0f, 62.0f};
    ccord_draw_texture_rounded(r, "__video_stream__", preview, 4.0f, 255);
  } else {
    if (speaking) ccord_draw_circle(r, tile.x + tile.w * 0.5f, tile.y + 38.0f, 26.0f + 2.5f, COL(status_online));
    ui_draw_avatar(ui, r, u, tile.x + tile.w * 0.5f, tile.y + 38.0f, 52.0f, (ccord_color){0, 0, 0, 0}, false);
  }
  const char *name = u ? u->name : "Unknown User";
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw_ellipsis(r, name, strlen(name), tile.x + 10.0f, ty + 70.0f, tile.w - 20.0f, &ns);
  if (vs->self_stream) {
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
    ccord_rect lb = {tile.x + tile.w - 46.0f, tile.y + 8.0f, 38.0f, 15.0f};
    ccord_draw_rrect(r, lb, 3.0f, COL(danger));
    ccord_text_draw(r, "LIVE", 4, lb.x + 6.0f, lb.y + 2.0f, &ls, 0.0f);
  }

  ui_stage_state *st = stage_state(ui);
  ccord_rect mod = {0, 0, 0, 0}, mute = {0, 0, 0, 0}, slider = {0, 0, 0, 0};
  bool mod_is_invite = false;
  // Row 3: stage speaker management, else the watch-stream button for a remote broadcaster.
  if (is_stage && can_mod && !is_self) {
    mod_is_invite = vs->suppress;
    const char *mlbl = mod_is_invite ? "Invite to Speak" : "Move to Audience";
    mod = (ccord_rect){tile.x + 10.0f, ty + 92.0f, tile.w - 20.0f, 22.0f};
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mod);
    ccord_draw_rrect(r, mod, 4.0f, hov ? ui_mix(COL(brand), COL(bg_item_hover), 0.25f) : COL(brand));
    ccord_text_style ms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_on_brand));
    ccord_vec2 mt = ccord_text_measure(r, mlbl, strlen(mlbl), &ms, 0.0f);
    ccord_text_draw(r, mlbl, strlen(mlbl), mod.x + (mod.w - mt.x) * 0.5f, mod.y + (mod.h - mt.y) * 0.5f, &ms, 0.0f);
  } else if (vs->self_stream && !is_self) {
    bool is_watched = watching_remote && ws->user_id == vs->user_id;
    mod = (ccord_rect){tile.x + 10.0f, ty + 92.0f, tile.w - 20.0f, 22.0f};
    ccord_draw_rrect(r, mod, 4.0f, is_watched ? COL(bg_tertiary) : COL(brand));
    const char *wlbl = is_watched ? "Watching Stream" : "Watch Stream";
    ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f,
                                    is_watched ? COL(text_secondary) : COL(text_on_brand));
    ccord_vec2 wt = ccord_text_measure(r, wlbl, strlen(wlbl), &wst, 0.0f);
    ccord_text_draw(r, wlbl, strlen(wlbl), mod.x + (mod.w - wt.x) * 0.5f, mod.y + (mod.h - wt.y) * 0.5f, &wst, 0.0f);
    if (record && ui->stage_watch_count < 16) {
      ui->stage_watch_rects[ui->stage_watch_count] = mod;
      ui->stage_watch_users[ui->stage_watch_count] = vs->user_id;
      ui->stage_watch_count++;
    }
  } else {
    ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 4.0f,
                                    speaking ? COL(status_online) : COL(text_muted));
    const char *state = vs->suppress ? "Audience"
                                     : (vs->self_mute ? "Muted" : (speaking ? "Speaking" : "Connected"));
    ccord_text_draw_ellipsis(r, state, strlen(state), tile.x + 10.0f, ty + 94.0f, tile.w - 20.0f, &sts);
  }
  // Row 4: local playback prefs for everyone but self (client-only, never Discord state).
  if (!is_self) {
    bool lm = ccord_store_user_local_mute(ui->d.store, vs->user_id);
    mute = (ccord_rect){tile.x + 10.0f, ty + 120.0f, 24.0f, 22.0f};
    bool mhov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mute);
    ccord_draw_rrect(r, mute, 4.0f, mhov ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_speaker(r, mute.x + mute.w * 0.5f, mute.y + mute.h * 0.5f, 14.0f,
                    lm ? COL(danger) : COL(text_secondary));
    if (lm) {
      ccord_draw_rect(r, (ccord_rect){mute.x + 4.0f, mute.y + mute.h * 0.5f - 1.0f, mute.w - 8.0f, 2.0f}, COL(danger));
    }
    slider = (ccord_rect){tile.x + 40.0f, ty + 126.0f, tile.w - 92.0f, 10.0f};
    uint8_t pct = ccord_store_user_volume(ui->d.store, vs->user_id);
    ccord_draw_rrect(r, slider, 5.0f, COL(bg_tertiary));
    float fill_w = slider.w * ((float)pct / 200.0f);
    if (fill_w > 0.0f) ccord_draw_rrect(r, (ccord_rect){slider.x, slider.y, fill_w, slider.h}, 5.0f, COL(brand));
    char pl[16];
    snprintf(pl, sizeof pl, "%u%%", (unsigned)pct);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_muted));
    ccord_text_draw(r, pl, strlen(pl), tile.x + tile.w - 46.0f, ty + 122.0f, &ps, 0.0f);
  }
  if (record && st->ctl_count < UI_STAGE_CTL_MAX) {
    st->ctl[st->ctl_count].user_id = vs->user_id;
    st->ctl[st->ctl_count].tile = tile;
    st->ctl[st->ctl_count].slider = slider;
    st->ctl[st->ctl_count].mute = mute;
    st->ctl[st->ctl_count].mod = mod;
    st->ctl[st->ctl_count].mod_is_invite = mod_is_invite;
    st->ctl_count++;
  }
}

static bool cam_in_list(const ccord_voice_state *const *list, size_t n, const ccord_voice_state *vs)
{
  for (size_t i = 0; i < n; i++) {
    if (list[i] == vs) return true;
  }
  return false;
}

static ccord_rect stream_fit_aspect(ccord_rect bounds, int frame_w, int frame_h)
{
  if (bounds.w <= 0.0f || bounds.h <= 0.0f) return (ccord_rect){0, 0, 0, 0};
  float aspect = (frame_w > 0 && frame_h > 0) ? ((float)frame_w / (float)frame_h) : (16.0f / 9.0f);
  aspect = clampf(aspect, 0.25f, 4.0f);
  float target_w = bounds.h * aspect;
  if (target_w > bounds.w) target_w = bounds.w;
  float target_h = target_w / aspect;
  if (target_h > bounds.h) {
    target_h = bounds.h;
    target_w = target_h * aspect;
  }
  return (ccord_rect){bounds.x + (bounds.w - target_w) * 0.5f,
                      bounds.y + (bounds.h - target_h) * 0.5f, target_w, target_h};
}

void ui_draw_voice_stage(ccord_ui *ui, ccord_renderer *r)
{
  ccord_rect ma = ui_stage_area(ui);
  if (ma.w <= 0.0f || ma.h <= 0.0f) return;
  // The stage owns the message area (or the split stage pane): drop any stale transcript hits so
  // they cannot intercept stage clicks. In split mode ui_draw_messages repopulates them afterwards.
  reset_message_hits(ui);
  const ccord_voice_state *v = ui_self_voice(ui);
  const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id) : nullptr;
  if (!ch && ui->stage_channel != CCORD_ID_NONE) ch = ccord_store_channel(ui->d.store, ui->stage_channel);
  if (!ch) return;
  ccord_draw_rect(r, ma, COL(bg_primary));
  ccord_draw_rect(r, (ccord_rect){ma.x, ma.y + 56.0f, ma.w, 1.0f}, COL(border_subtle));
  ui_icon_speaker(r, ma.x + 24.0f, ma.y + 28.0f, 22.0f, COL(status_online));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw_ellipsis(r, ch->name, strlen(ch->name), ma.x + 44.0f, ma.y + 12.0f, ma.w - 220.0f, &ts);
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;
  char sub[192];
  if (self_streaming) {
    snprintf(sub, sizeof sub, "Voice / Stream Stage  ·  LIVE · %s · %s · %s · %s", encoder_label(st->config.encoder),
             codec_label(st->config.codec), resolution_label(st->config.resolution), fps_label(st->config.fps));
  } else if (watching_remote) {
    const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
    snprintf(sub, sizeof sub, "Voice / Stream Stage  ·  Watching %s's Stream", wu ? wu->name : "Live");
  } else {
    snprintf(sub, sizeof sub, "Voice / Stream Stage  ·  %zu connected", ccord_store_voice_count(ui->d.store, ch->id));
  }
  ccord_text_draw_ellipsis(r, sub, strlen(sub), ma.x + 44.0f, ma.y + 31.0f, ma.w - 220.0f, &ss);
  ccord_rect back = ui_stage_chat_btn(ui);
  bool bh = ui->hover_voice_back;
  float bhf = ui_fade(ui, "stageback", 0, bh);
  ccord_draw_rrect(r, back, 4.0f, ui_mix(COL(bg_tertiary), COL(bg_item_hover), bhf));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                 ui_mix(COL(text_secondary), COL(text_interactive_hover), bhf));
  const char *chat_lbl = ui_stage_split_active(ui) ? "Close Chat" : "Text Chat";
  ccord_vec2 bt = ccord_text_measure(r, chat_lbl, strlen(chat_lbl), &bs, 0.0f);
  ccord_text_draw(r, chat_lbl, strlen(chat_lbl), back.x + (back.w - bt.x) * 0.5f, back.y + (back.h - bt.y) * 0.5f, &bs, 0.0f);

  ui->stream_stage_btn1_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_btn2_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_pause_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_fs_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_vport_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_stage_resize_rect = (ccord_rect){0, 0, 0, 0};
  ui->stage_watch_count = 0;
  ui_stage_state *sst = stage_state(ui);
  memset(sst, 0, sizeof *sst);
  sst->owner = ui;
  ui_activity_state *ast = activity_state(ui);
  ast->chip = (ccord_rect){0, 0, 0, 0};
  ast->banner_visible = false;
  ast->instance_count = 0;

  float area_x = ma.x + 20.0f, area_y = ma.y + 70.0f;
  float area_w = ma.w - 40.0f;

  // M5 control row: voice status, stage topic, request-to-speak. Edits open the shared text prompt;
  // the buttons only appear when the session would accept the action (permission / stage membership).
  bool is_stage = ch->kind == CCORD_CHANNEL_STAGE;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool self_here = v && v->channel_id == ch->id;
  float ctl_cy = ma.y + 66.0f, cx = area_x;
  bool can_status = ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                               CCORD_PERM_SET_VOICE_CHANNEL_STATUS);
  const char *status = ccord_store_voice_status(ui->d.store, ch->id);
  if (can_status) {
    char sl[160];
    snprintf(sl, sizeof sl, "Status: %s", (status && status[0]) ? status : "set voice status");
    sst->status_edit = stage_chip(ui, r, cx, ctl_cy, sl, COL(bg_tertiary), COL(text_secondary), 260.0f);
    cx = sst->status_edit.x + sst->status_edit.w + 8.0f;
  } else if (status && status[0]) {
    ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    ccord_text_draw_ellipsis(r, status, strlen(status), cx, ctl_cy - 7.0f, 260.0f, &sts);
    cx += 268.0f;
  }
  bool can_mod = is_stage && ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                                        CCORD_PERM_MUTE_MEMBERS);
  if (is_stage && can_mod) {
    const ccord_stage_instance *si = ccord_store_stage_instance(ui->d.store, ch->id);
    sst->stage_active = si != nullptr;
    if (si) {
      // An instance is live: its topic can be edited and the stage can be ended.
      char tl[160];
      snprintf(tl, sizeof tl, "Topic: %s", si->topic[0] ? si->topic : "set topic");
      sst->topic_edit = stage_chip(ui, r, cx, ctl_cy, tl, COL(bg_tertiary), COL(text_secondary), 260.0f);
      cx = sst->topic_edit.x + sst->topic_edit.w + 8.0f;
      sst->stage_end = stage_chip(ui, r, cx, ctl_cy, "End Stage", COL(danger), COL(text_on_brand), 120.0f);
      cx = sst->stage_end.x + sst->stage_end.w + 8.0f;
    } else {
      // No instance yet: Start Stage opens the topic prompt and creates it through the session.
      sst->stage_start = stage_chip(ui, r, cx, ctl_cy, "Start Stage", COL(brand), COL(text_on_brand), 130.0f);
      cx = sst->stage_start.x + sst->stage_start.w + 8.0f;
    }
  }
  // Embedded Activities: the chip opens the shelf. Only for a voice channel the user is connected to
  // (launching also joins) and only with the Use Activities permission.
  if (ch->kind == CCORD_CHANNEL_VOICE && self_here && ui->d.session && ch->guild_id != CCORD_DM_GUILD_ID &&
      ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id, CCORD_PERM_USE_EMBEDDED_ACTIVITIES)) {
    ast->chip = stage_chip(ui, r, cx, ctl_cy, "Activities", COL(brand), COL(text_on_brand), 120.0f);
    cx = ast->chip.x + ast->chip.w + 8.0f;
  }
  if (is_stage && self_here && v->suppress) {
    bool pending = v->request_to_speak_timestamp > 0;
    bool can_ask = pending || ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                                         CCORD_PERM_REQUEST_TO_SPEAK);
    if (can_ask) {
      const char *rl = pending ? "Cancel Request" : "Request to Speak";
      float rw = 150.0f;
      float rx = ma.x + ma.w - 20.0f - rw;
      if (rx > cx) {
        sst->request_speak = stage_chip(ui, r, rx, ctl_cy, rl, pending ? COL(bg_tertiary) : COL(brand),
                                      pending ? COL(text_secondary) : COL(text_on_brand), rw);
        sst->request_speak_cancel = pending;
      }
    }
  }
  area_y = ma.y + 96.0f;

  if (self_streaming || watching_remote) {
    float avail_h = maxf(ma.y + ma.h - area_y, 0.0f);
    float min_h = fminf(160.0f, avail_h);
    float max_h = maxf(avail_h - 14.0f, min_h);
    float stage_h;
    if (ui->stream_stage_height_frac > 0.0f) {
      stage_h = clampf(avail_h * ui->stream_stage_height_frac, min_h, max_h);
    } else {
      float def_hi = maxf(avail_h - 280.0f, min_h);
      stage_h = clampf(avail_h * 0.58f, min_h, def_hi);
    }
    ui->stream_stage_player_y = area_y;
    ui->stream_stage_avail_h = avail_h;

    ccord_rect player = {area_x, area_y, area_w, stage_h};
    sst->stream_player = player;
    ccord_rect rhandle = {player.x, player.y + player.h - 2.0f, player.w, 16.0f};
    ui->stream_stage_resize_rect = rhandle;
    sst->stream_resize_handle = rhandle;
    bool rh_drag = ui->drag == DRAG_STREAM_STAGE_H;
    bool rh_hov = !rh_drag && ui->mouse_in && ui_in_rect(ui->mx, ui->my, rhandle);
    ccord_draw_rrect(r, (ccord_rect){player.x - 1.0f, player.y - 1.0f, player.w + 2.0f, player.h + 2.0f}, 9.0f,
                     rh_drag ? COL(brand) : COL(border_subtle));
    ccord_draw_rrect(r, player, 8.0f, (ccord_color){0x14, 0x15, 0x18, 255});

    // Status is honest: the store only learns that a stream exists (STREAM_CREATE); a LIVE badge
    // needs an actual preview/decoded frame, except when the user hid their own preview on purpose.
    int frame_w = 0, frame_h = 0;
    bool frame_present =
        ccord_render_has_texture(r, "__video_stream__", &frame_w, &frame_h) && frame_w > 0 && frame_h > 0;
    bool paused = (self_streaming && st->paused) || (watching_remote && ws->paused);
    bool live = frame_present || (self_streaming && ui->stream_hide_preview);
    const char *badge = "LIVE";
    ccord_color badge_bg = COL(danger);
    ccord_color badge_fg = COL(text_on_brand);
    const char *placeholder = nullptr;
    if (paused) {
      badge = "PAUSED";
      badge_bg = COL(bg_tertiary);
      badge_fg = COL(text_header);
      placeholder = "Stream Paused";
    } else if (!live) {
      badge = self_streaming ? "STARTING" : "CONNECTING";
      badge_bg = COL(bg_tertiary);
      badge_fg = COL(text_header);
      placeholder = self_streaming ? "Starting capture & encoder..." : "Connecting to stream video...";
    }

    // Header bar inside the stream player
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, badge_fg);
    ccord_vec2 lt = ccord_text_measure(r, badge, strlen(badge), &ls, 0.0f);
    ccord_rect lb = {player.x + 12.0f, player.y + 10.0f, lt.x + 16.0f, 18.0f};
    ccord_draw_rrect(r, lb, 4.0f, badge_bg);
    ccord_text_draw(r, badge, strlen(badge), lb.x + (lb.w - lt.x) * 0.5f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);

    char st_title[192];
    char stream_sub[48];
    if (frame_present) {
      snprintf(stream_sub, sizeof stream_sub, "%dx%d", frame_w, frame_h);
    } else {
      snprintf(stream_sub, sizeof stream_sub, "Screen Share");
    }
    if (self_streaming) {
      const char *src_name = (ui->has_active_capture_source && ui->active_capture_source.title[0])
                               ? ui->active_capture_source.title
                               : "Screen Share";
      snprintf(st_title, sizeof st_title, "Your Stream  ·  %s  (%s · %s)", src_name,
               resolution_label(st->config.resolution), fps_label(st->config.fps));
    } else {
      const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
      snprintf(st_title, sizeof st_title, "%s's Screen Share  ·  %s", wu ? wu->name : "Remote User", stream_sub);
    }
    snprintf(sst->stream_badge, sizeof sst->stream_badge, "%s", badge);
    snprintf(sst->stream_title, sizeof sst->stream_title, "%s", st_title);
    snprintf(sst->stream_placeholder, sizeof sst->stream_placeholder, "%s", placeholder ? placeholder : "");
    sst->stream_frame_present = frame_present;
    sst->stream_frame_w = frame_present ? frame_w : 0;
    sst->stream_frame_h = frame_present ? frame_h : 0;

    // Right-aligned stream controls: [Pause (self)] [Hide Preview / Pause] [Stop] [Fullscreen]
    ccord_text_style b1s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_header));
    ccord_text_style b2s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_on_brand));
    const char *b1_lbl = self_streaming ? (ui->stream_hide_preview ? "Show Preview" : "Hide Preview")
                                        : (ws->paused ? "Resume" : "Pause");
    const char *b2_lbl = self_streaming ? "Stop Streaming" : "Stop Watching";
    ccord_vec2 b1t = ccord_text_measure(r, b1_lbl, strlen(b1_lbl), &b1s, 0.0f);
    ccord_vec2 b2t = ccord_text_measure(r, b2_lbl, strlen(b2_lbl), &b2s, 0.0f);
    float b2_w = player.w < 440.0f ? maxf(b2t.x + 14.0f, 76.0f) : 114.0f;
    float b1_w = player.w < 440.0f ? maxf(b1t.x + 14.0f, 64.0f) : 104.0f;

    ccord_rect fsb = {player.x + player.w - 12.0f - 28.0f, player.y + 7.0f, 28.0f, 24.0f};
    ccord_rect b2 = {fsb.x - 6.0f - b2_w, player.y + 7.0f, b2_w, 24.0f};
    ccord_rect b1 = {b2.x - 6.0f - b1_w, player.y + 7.0f, b1_w, 24.0f};
    ui->stream_stage_fs_rect = fsb;
    sst->stream_fs_btn = fsb;
    ui->stream_stage_btn1_rect = b1;
    ui->stream_stage_btn2_rect = b2;
    bool fs_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, fsb);
    bool b1_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b1);
    bool b2_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b2);
    ccord_draw_rrect(r, fsb, 4.0f, fs_hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_fullscreen(r, fsb.x + fsb.w * 0.5f, fsb.y + fsb.h * 0.5f, 13.0f,
                       fs_hov ? COL(text_interactive_hover) : COL(text_header), false);
    ccord_draw_rrect(r, b1, 4.0f, b1_hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ccord_draw_rrect(r, b2, 4.0f, b2_hov ? ui_mix(COL(danger), COL(bg_item_hover), 0.2f) : COL(danger));
    ccord_text_draw(r, b1_lbl, strlen(b1_lbl), b1.x + (b1.w - b1t.x) * 0.5f, b1.y + (b1.h - b1t.y) * 0.5f, &b1s, 0.0f);
    ccord_text_draw(r, b2_lbl, strlen(b2_lbl), b2.x + (b2.w - b2t.x) * 0.5f, b2.y + (b2.h - b2t.y) * 0.5f, &b2s, 0.0f);
    float left_btn_x = b1.x;
    if (self_streaming) {
      const char *plbl = st->paused ? "Resume" : "Pause";
      ccord_vec2 pt = ccord_text_measure(r, plbl, strlen(plbl), &b1s, 0.0f);
      float pb_w = player.w < 440.0f ? maxf(pt.x + 14.0f, 54.0f) : 78.0f;
      ccord_rect pb = {b1.x - 6.0f - pb_w, player.y + 7.0f, pb_w, 24.0f};
      ui->stream_stage_pause_rect = pb;
      bool pb_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pb);
      ccord_draw_rrect(r, pb, 4.0f, pb_hov ? COL(bg_item_hover) : COL(bg_tertiary));
      ccord_text_draw(r, plbl, strlen(plbl), pb.x + (pb.w - pt.x) * 0.5f, pb.y + (pb.h - pt.y) * 0.5f, &b1s, 0.0f);
      left_btn_x = pb.x;
    }

    ccord_text_style pts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    float title_x = lb.x + lb.w + 10.0f;
    float title_max_w = left_btn_x - title_x - 10.0f;
    if (title_max_w > 24.0f) {
      ccord_text_draw_ellipsis(r, st_title, strlen(st_title), title_x, player.y + 11.0f, title_max_w, &pts);
    }

    // Video frame viewport inside the stream player
    ccord_rect vport = {player.x + 12.0f, player.y + 36.0f, maxf(player.w - 24.0f, 0.0f), maxf(player.h - 46.0f, 0.0f)};
    ui->stream_stage_vport_rect = vport;
    sst->stream_vport = vport;
    if (self_streaming && ui->stream_hide_preview) {
      ccord_draw_rrect(r, vport, 6.0f, COL(bg_secondary));
      const char *hmsg = "Stream preview hidden — your screen share is still live";
      ccord_text_style hms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
      ccord_vec2 hmt = ccord_text_measure(r, hmsg, strlen(hmsg), &hms, 0.0f);
      ccord_text_draw(r, hmsg, strlen(hmsg), vport.x + (vport.w - hmt.x) * 0.5f, vport.y + (vport.h - hmt.y) * 0.5f, &hms, 0.0f);
    } else if (frame_present) {
      ccord_rect vid = stream_fit_aspect(vport, frame_w, frame_h);
      uint8_t alpha = paused ? 110 : 255;
      ccord_draw_texture_rounded(r, "__video_stream__", vid, 6.0f, alpha);
      if (paused) {
        const char *pmsg = "Stream Paused";
        ccord_text_style pms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
        ccord_vec2 pmt = ccord_text_measure(r, pmsg, strlen(pmsg), &pms, 0.0f);
        ccord_draw_rrect(r, (ccord_rect){vid.x + (vid.w - pmt.x - 24.0f) * 0.5f, vid.y + (vid.h - 32.0f) * 0.5f, pmt.x + 24.0f, 32.0f},
                         6.0f, COL(bg_floating));
        ccord_text_draw(r, pmsg, strlen(pmsg), vid.x + (vid.w - pmt.x) * 0.5f, vid.y + (vid.h - pmt.y) * 0.5f, &pms, 0.0f);
      }
    } else {
      ccord_draw_rrect(r, vport, 6.0f, COL(bg_secondary));
      ccord_text_style phs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
      ccord_vec2 pht = ccord_text_measure(r, placeholder, strlen(placeholder), &phs, 0.0f);
      ccord_text_draw(r, placeholder, strlen(placeholder), vport.x + (vport.w - pht.x) * 0.5f,
                      vport.y + (vport.h - pht.y) * 0.5f, &phs, 0.0f);
    }

    if (fs_hov && !ui->menu.open && !ui->settings_open && !ui->go_live_open) {
      const char *tip = "Fullscreen (F)";
      ccord_text_style tips = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_header));
      ccord_vec2 tipt = ccord_text_measure(r, tip, strlen(tip), &tips, 0.0f);
      float tw = tipt.x + 14.0f, th = 22.0f;
      ccord_rect tipr = {fsb.x + fsb.w - tw, fsb.y + fsb.h + 4.0f, tw, th};
      ccord_draw_rrect(r, (ccord_rect){tipr.x - 1.0f, tipr.y - 1.0f, tipr.w + 2.0f, tipr.h + 2.0f}, 5.0f, COL(border_subtle));
      ccord_draw_rrect(r, tipr, 4.0f, COL(bg_floating));
      ccord_text_draw(r, tip, strlen(tip), tipr.x + 7.0f, tipr.y + (tipr.h - tipt.y) * 0.5f, &tips, 0.0f);
    }

    // Bottom vertical resize grip bar (drag to resize stream height; double-click to reset).
    if (rh_drag || rh_hov) {
      ccord_draw_rect(r, (ccord_rect){player.x + 8.0f, player.y + player.h - 1.0f, maxf(player.w - 16.0f, 0.0f), 2.0f},
                      rh_drag ? COL(brand) : ui_mix(COL(border_subtle), COL(brand), 0.65f));
    }
    float grip_w = fminf(48.0f, maxf(player.w - 32.0f, 16.0f));
    ccord_rect grip = {player.x + (player.w - grip_w) * 0.5f, player.y + player.h + 5.0f, grip_w, 4.0f};
    ccord_color grip_col = rh_drag ? COL(brand) : (rh_hov ? COL(text_header) : ui_alpha(COL(text_muted), 0.55f));
    ccord_draw_rrect(r, grip, 2.0f, grip_col);

    area_y += stage_h + 14.0f;
  }

  // Participant tiles (Speakers / Audience sections on a stage channel) and the Go Live cards share
  // one scrollable viewport below the header / controls / stream player, so off-fold tiles wrap and
  // stay reachable via the wheel instead of being dropped at the bottom edge.
  size_t n = ccord_store_voice_count(ui->d.store, ch->id);
  float tile_w = 176.0f, tile_h = 150.0f, gap = 12.0f;
  int cols = (int)((area_w + gap) / (tile_w + gap));
  if (cols < 1) cols = 1;
  int passes = is_stage ? 2 : 1;
  size_t pass_match[2] = {0, 0};
  // Camera participants (self_video; self first) get the 16:9 tile grid on a plain voice channel.
  // The sender list is read once per draw; everyone else stays a legacy tile below the grid.
  const ccord_voice_state *camlist[UI_CAM_TILE_MAX];
  size_t ncam = 0;
  ccord_camera_rx cam_rx[CCORD_CAM_RX_MAX];
  size_t ncam_rx = 0, cam_decoding = 0;
  // Only the call self is in has cameras to show: from a channel self has left (or never joined) no sender is
  // delivered, so its camera users stay audio-only tiles instead of waiting forever on "Connecting video".
  const ccord_voice_state *self_vs = ui_self_voice(ui);
  if (!is_stage && self_vs && self_vs->channel_id == ch->id) {
    ccord_id sid = ccord_store_self_id(ui->d.store);
    for (int selfpass = 1; selfpass >= 0; selfpass--) {
      for (size_t i = 0; i < n && ncam < UI_CAM_TILE_MAX; i++) {
        const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, i);
        if (vs && vs->self_video && (vs->user_id == sid) == (selfpass == 1)) camlist[ncam++] = vs;
      }
    }
    if (ncam > 0 && ui->d.session) ncam_rx = ccord_session_camera_senders(ui->d.session, cam_rx, CCORD_CAM_RX_MAX);
    for (size_t i = 0; i < ncam_rx; i++) {
      if (cam_rx[i].decoding) cam_decoding++;
    }
  }
  ui_cam_layout cam_l = {0};
  float cam_h = 0.0f;
  if (ncam > 0) {
    ui_cam_layout_compute(area_w, ncam, &cam_l);
    cam_h = ui_cam_layout_height(&cam_l, ncam) + gap;
  }
  for (int pass = 0; pass < passes; pass++) {
    for (size_t i = 0; i < n; i++) {
      const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, i);
      if (!vs) continue;
      if (cam_in_list(camlist, ncam, vs)) continue;
      if (is_stage && ((pass == 0) == vs->suppress)) continue;
      pass_match[pass]++;
    }
  }
  size_t nsc = ccord_store_stream_list_count(ui->d.store);
  size_t ncards = 0;
  for (size_t i = 0; i < nsc && ncards < UI_STREAM_CARD_MAX; i++) {
    const ccord_stream_state *bcs = ccord_store_stream_list_at(ui->d.store, i);
    if (bcs && bcs->active && bcs->channel_id == ch->id) ncards++;
  }
  int card_cols = (int)((area_w - 220.0f) / 232.0f) + 1;
  if (card_cols < 1) card_cols = 1;
  // Measure the full content first so the scroll offset clamps before anything is placed.
  float content_h = cam_h;
  for (int pass = 0; pass < passes; pass++) {
    size_t rows = (pass_match[pass] + (size_t)cols - 1u) / (size_t)cols;
    content_h += (is_stage ? 22.0f : 0.0f) + (float)rows * (tile_h + gap) + (is_stage ? 10.0f : 0.0f);
  }
  if (n == 0) content_h += 30.0f;
  sst->cam_cols = cam_l.cols;
  const ccord_activity_instance *act_inst = nullptr;
  size_t act_n = activity_instance_total(ui, ch, &act_inst);
  content_h += activity_block_h(act_n);
  if (ncards > 0) content_h += 18.0f + (float)((ncards + (size_t)card_cols - 1u) / (size_t)card_cols) * 62.0f;

  ccord_rect sview = {ma.x, area_y, ma.w, ma.y + ma.h - area_y};
  if (sview.h < 0.0f) sview.h = 0.0f;
  ui->stage_view_h = sview.h;
  ui->stage_content_h = content_h;
  ui->stage_scroll = clampf(ui->stage_scroll, 0.0f, maxf(content_h - sview.h, 0.0f));
  sst->stage_view = sview;
  if (sview.h <= 1.0f) {
    ui->stage_thumb = (ccord_rect){0, 0, 0, 0};
    return;
  }

  ccord_push_clip(r, sview);
  float section_y = area_y - ui->stage_scroll;
  activity_draw_block(ui, r, ch, act_inst, act_n, area_x, section_y, area_w, sview);
  section_y += activity_block_h(act_n);
  for (size_t i = 0; i < ncam; i++) {
    const ccord_voice_state *vs = camlist[i];
    ccord_rect tile = ui_cam_layout_tile(&cam_l, area_x, area_w, section_y, ncam, i);
    ui_cam_tile_info ci = {0};
    ci.video = (ccord_rect){tile.x + 6.0f, tile.y + 6.0f, tile.w - 12.0f, cam_l.video_h};
    ccord_session_camera_texture_key(vs->user_id, ci.key, sizeof ci.key);
    int tw = 0, th = 0;
    ci.textured = ccord_render_has_texture(r, ci.key, &tw, &th) && tw > 0 && th > 0;
    ci.self = vs->user_id == ccord_store_self_id(ui->d.store);
    bool listed = false, decoding = false;
    for (size_t k = 0; k < ncam_rx; k++) {
      if (cam_rx[k].user_id == vs->user_id) {
        listed = true;
        decoding = cam_rx[k].decoding;
      }
    }
    ci.limit = !ci.self && listed && !decoding && cam_decoding >= CCORD_CAM_DECODE_MAX;
    if (sst->cam_tile_count < UI_CAM_TILE_MAX) {
      size_t ti = sst->cam_tile_count++;
      sst->cam_tile[ti].user_id = vs->user_id;
      sst->cam_tile[ti].rect = tile;
      sst->cam_tile[ti].video = ci.video;
      memcpy(sst->cam_tile[ti].key, ci.key, sizeof sst->cam_tile[ti].key);
      sst->cam_tile[ti].textured = ci.textured;
      sst->cam_tile[ti].tex_w = ci.textured ? tw : 0;
      sst->cam_tile[ti].tex_h = ci.textured ? th : 0;
      sst->cam_tile[ti].limit = ci.limit;
      sst->cam_tile[ti].connecting = !ci.textured && !ci.self && !ci.limit;
      sst->cam_tile[ti].self = ci.self;
    }
    if (tile.y + tile.h < sview.y || tile.y > sview.y + sview.h) continue;
    stage_tile(ui, r, vs, tile, watching_remote, ws, is_stage, can_mod, sview, &ci);
  }
  section_y += cam_h;
  for (int pass = 0; pass < passes; pass++) {
    if (is_stage) {
      if (pass == 0) sst->speaker_count = pass_match[0];
      else sst->audience_count = pass_match[1];
      char hdr[64];
      snprintf(hdr, sizeof hdr, "%s  ·  %zu", pass == 0 ? "SPEAKERS" : "AUDIENCE", pass_match[pass]);
      ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_text_draw(r, hdr, strlen(hdr), area_x, section_y, &hs, 0.0f);
      section_y += 22.0f;
    }
    size_t placed = 0;
    for (size_t i = 0; i < n; i++) {
      const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, i);
      if (!vs) continue;
      if (is_stage && ((pass == 0) == vs->suppress)) continue;
      if (cam_in_list(camlist, ncam, vs)) continue;
      int col = (int)(placed % (size_t)cols), row = (int)(placed / (size_t)cols);
      ccord_rect tile = {area_x + (float)col * (tile_w + gap), section_y + (float)row * (tile_h + gap),
                         tile_w, tile_h};
      placed++;
      if (tile.y + tile.h < sview.y || tile.y > sview.y + sview.h) continue;
      stage_tile(ui, r, vs, tile, watching_remote, ws, is_stage, can_mod, sview, nullptr);
    }
    size_t rows = (pass_match[pass] + (size_t)cols - 1u) / (size_t)cols;
    section_y += (float)rows * (tile_h + gap) + (is_stage ? 10.0f : 0.0f);
  }
  if (n == 0) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    float empty_y = area_y - ui->stage_scroll + activity_block_h(act_n);
    ccord_text_draw(r, "No one is connected yet.", 25, area_x, empty_y, &es, 0.0f);
    section_y = empty_y + 30.0f;
  }

  // Go Live broadcast cards: the server-reported broadcasts in this channel, with viewer counts.
  float card_x = area_x, card_y = section_y + 2.0f;
  bool card_hdr = false;
  for (size_t i = 0; i < nsc && sst->stream_cards < UI_STREAM_CARD_MAX; i++) {
    const ccord_stream_state *bcs = ccord_store_stream_list_at(ui->d.store, i);
    if (!bcs || !bcs->active || bcs->channel_id != ch->id) continue;
    if (!card_hdr) {
      ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
      ccord_text_draw(r, "LIVE NOW", 8, area_x, card_y, &hs, 0.0f);
      card_y += 18.0f;
      card_hdr = true;
    }
    ccord_rect card = {card_x, card_y, 220.0f, 50.0f};
    card_x += 232.0f;
    if (card_x + 220.0f > ma.x + ma.w - 20.0f) {
      card_x = area_x;
      card_y += 62.0f;
    }
    if (card.y + card.h < sview.y || card.y > sview.y + sview.h) continue;
    ccord_draw_rrect(r, card, 6.0f, COL(bg_secondary));
    const ccord_user *bu = ccord_store_user(ui->d.store, bcs->user_id);
    char cl[160];
    snprintf(cl, sizeof cl, "%s", bu ? bu->name : "Live");
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_draw_ellipsis(r, cl, strlen(cl), card.x + 10.0f, card.y + 6.0f, card.w - 100.0f, &cs);
    uint32_t viewers = ccord_store_stream_viewer_count(ui->d.store, ch->id, bcs->user_id);
    char vl[48];
    snprintf(vl, sizeof vl, "%u watching", (unsigned)viewers);
    ccord_text_style vs2 = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    ccord_text_draw(r, vl, strlen(vl), card.x + 10.0f, card.y + 26.0f, &vs2, 0.0f);
    bool is_watched = watching_remote && ws && ws->user_id == bcs->user_id;
    bool is_self_card = bcs->user_id == self_id;
    ccord_rect wb = {card.x + card.w - 82.0f, card.y + (card.h - 22.0f) * 0.5f, 72.0f, 22.0f};
    if (!is_self_card) {
      ccord_draw_rrect(r, wb, 4.0f, is_watched ? COL(bg_tertiary) : COL(brand));
      const char *wl = is_watched ? "Watching" : "Watch";
      ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f,
                                      is_watched ? COL(text_secondary) : COL(text_on_brand));
      ccord_vec2 wt = ccord_text_measure(r, wl, strlen(wl), &wst, 0.0f);
      ccord_text_draw(r, wl, strlen(wl), wb.x + (wb.w - wt.x) * 0.5f, wb.y + (wb.h - wt.y) * 0.5f, &wst, 0.0f);
    } else {
      wb = (ccord_rect){0, 0, 0, 0};
    }
    // Only fully visible cards expose their Watch button.
    if (card.y >= sview.y && card.y + card.h <= sview.y + sview.h) {
      sst->card[sst->stream_cards].user_id = bcs->user_id;
      sst->card[sst->stream_cards].viewers = viewers;
      sst->card[sst->stream_cards].rect = card;
      sst->card[sst->stream_cards].watch = wb;
      sst->card[sst->stream_cards].watching = is_watched;
      sst->stream_cards++;
    }
  }
  ccord_pop_clip(r);
  ui->stage_thumb = ui_draw_list_scrollbar(ui, r, sview, content_h, ui->stage_scroll, "stagescroll", 0);
}

// Activity chip / instance-card clicks inside the stage view. Join launches (and joins) through the
// session, Leave leaves the user's instance, Open in browser hands the channel URL to the host opener.
static bool activity_stage_click(ccord_ui *ui, const ccord_channel *ch, float x, float y)
{
  ui_activity_state *st = activity_state(ui);
  if (st->chip.w > 0.0f && ui_in_rect(x, y, st->chip)) {
    ui_activity_shelf_open(ui, ch->guild_id, ch->id);
    return true;
  }
  for (size_t i = 0; i < st->instance_count && i < UI_ACTIVITY_INST_MAX; i++) {
    if (st->inst[i].join.w > 0.0f && ui_in_rect(x, y, st->inst[i].join)) {
      activity_launch(ui, ch->id, st->inst[i].application_id);
      return true;
    }
    if (st->inst[i].open_browser.w > 0.0f && ui_in_rect(x, y, st->inst[i].open_browser)) {
      char url[160];
      activity_channel_url(ch, url, sizeof url);
      (void)ui_open_url(ui, url, "Activity");
      return true;
    }
    if (st->inst[i].leave.w > 0.0f && ui_in_rect(x, y, st->inst[i].leave)) {
      const ccord_activity_instance *rows = nullptr;
      size_t n = activity_instance_total(ui, ch, &rows);
      for (size_t k = 0; k < n; k++) {
        if (rows[k].instance_id != st->inst[i].instance_id) continue;
        ccord_error err;
        if (ccord_session_activity_leave(ui->d.session, &rows[k], &err) == CCORD_OK) {
          ui_notice(ui, "Leaving activity...");
        } else {
          ui_notice(ui, "Cannot leave activity: %s", err.message);
        }
        break;
      }
      ui->dirty = true;
      return true;
    }
  }
  return false;
}

ccord_id ui_voice_stage_user_at(const ccord_ui *ui, float x, float y)
{
  if (!ui || ui->stage_channel == CCORD_ID_NONE) return CCORD_ID_NONE;
  const ui_stage_state *sst = stage_state(ui);
  if (sst->stage_view.w <= 0.0f || !ui_in_rect(x, y, sst->stage_view)) return CCORD_ID_NONE;
  for (size_t i = 0; i < sst->stream_cards; i++) {
    if (sst->card[i].rect.w > 0.0f && ui_in_rect(x, y, sst->card[i].rect)) return sst->card[i].user_id;
  }
  for (size_t i = 0; i < sst->cam_tile_count; i++) {
    if (sst->cam_tile[i].rect.w > 0.0f && ui_in_rect(x, y, sst->cam_tile[i].rect)) return sst->cam_tile[i].user_id;
  }
  for (size_t i = 0; i < sst->ctl_count; i++) {
    if (sst->ctl[i].tile.w > 0.0f && ui_in_rect(x, y, sst->ctl[i].tile)) return sst->ctl[i].user_id;
  }
  return CCORD_ID_NONE;
}

bool ui_voice_stage_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || ui->stage_channel == CCORD_ID_NONE || ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return false;
  const ccord_voice_state *v = ui_self_voice(ui);
  const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id)
                              : ccord_store_channel(ui->d.store, ui->stage_channel);
  if (!ch) return false;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;

  if ((self_streaming || watching_remote) && ui_in_rect(ev->x, ev->y, ui->stream_stage_fs_rect)) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  if (self_streaming && ui_in_rect(ev->x, ev->y, ui->stream_stage_pause_rect)) {
    if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !st->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui_in_rect(ev->x, ev->y, ui->stream_stage_btn1_rect)) {
    if (self_streaming) {
      ui->stream_hide_preview = !ui->stream_hide_preview;
    } else if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !ws->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui_in_rect(ev->x, ev->y, ui->stream_stage_btn2_rect)) {
    ccord_error err;
    if (self_streaming && ui->d.session) {
      (void)ccord_session_stop_stream(ui->d.session, ch->guild_id, &err);
    } else if (watching_remote && ui->d.session) {
      (void)ccord_session_stop_watching(ui->d.session, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui->stream_stage_resize_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_stage_resize_rect)) {
    if (ev->clicks >= 2) {
      ui->stream_stage_height_frac = 0.0f;
      ui_settings_save(ui);
    } else {
      ui->drag = DRAG_STREAM_STAGE_H;
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ev->clicks >= 2 && ui->stream_stage_vport_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_stage_vport_rect)) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }

  // M5 control row: status / topic open the shared prompt; request-to-speak is a governed PATCH.
  ui_stage_state *sst = stage_state(ui);
  bool is_stage = ch->kind == CCORD_CHANNEL_STAGE;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool can_mod = is_stage && ccord_store_has_permission(ui->d.store, ch->guild_id, ch->id, self_id,
                                                        CCORD_PERM_MUTE_MEMBERS);
  if (sst->status_edit.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->status_edit)) {
    ui_voice_prompt_open(ui, UI_VOICE_PROMPT_STATUS, ch->id);
    return true;
  }
  if (sst->topic_edit.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->topic_edit)) {
    ui_voice_prompt_open(ui, UI_VOICE_PROMPT_TOPIC, ch->id);
    return true;
  }
  if (sst->stage_start.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->stage_start)) {
    ui_stage_start_prompt_open(ui, ch->guild_id, ch->id);
    return true;
  }
  if (sst->stage_end.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->stage_end)) {
    ui_stage_end_confirm_open(ui, ch->guild_id, ch->id);
    return true;
  }
  if (sst->request_speak.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->request_speak)) {
    ccord_admin_request req;
    ccord_error err;
    int64_t ask = sst->request_speak_cancel ? 0 : ui->now_unix_ms;
    if (ccord_admin_stage_request_speak(&req, ch->guild_id, ch->id, ask, &err) == CCORD_OK) {
      ui_admin_submit(ui, &req);
    } else {
      ui_notice(ui, "%s", err.message);
    }
    ui->dirty = true;
    return true;
  }
  if (activity_stage_click(ui, ch, ev->x, ev->y)) return true;
  // Per-user controls: stage speaker management, local mute and the click-to-set volume slider.
  for (size_t i = 0; i < sst->ctl_count; i++) {
    if (sst->ctl[i].mute.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->ctl[i].mute)) {
      bool lm = ccord_store_user_local_mute(ui->d.store, sst->ctl[i].user_id);
      ccord_error err;
      (void)ccord_store_set_user_local_mute(ui->d.store, sst->ctl[i].user_id, !lm, &err);
      ui->dirty = true;
      return true;
    }
    if (sst->ctl[i].slider.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->ctl[i].slider)) {
      float frac = (ev->x - sst->ctl[i].slider.x) / sst->ctl[i].slider.w;
      if (frac < 0.0f) frac = 0.0f;
      if (frac > 1.0f) frac = 1.0f;
      int pct = (int)(frac * 200.0f + 0.5f);
      if (pct > 200) pct = 200;
      ccord_error err;
      if (ui->d.session &&
          ccord_session_set_user_volume(ui->d.session, sst->ctl[i].user_id, (uint8_t)pct, &err) == CCORD_OK) {
        ccord_ui_set_user_volume(ui, sst->ctl[i].user_id, (uint8_t)pct);
      } else if (!ui->d.session) {
        ui_notice(ui, "Not connected");
      }
      ui->dirty = true;
      return true;
    }
    if (is_stage && can_mod && sst->ctl[i].mod.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->ctl[i].mod) &&
        sst->ctl[i].user_id != self_id) {
      ccord_admin_request req;
      ccord_error err;
      if (ccord_admin_stage_set_speaker(&req, ch->guild_id, ch->id, sst->ctl[i].user_id,
                                       sst->ctl[i].mod_is_invite, nullptr, &err) == CCORD_OK) {
        ui_admin_submit(ui, &req);
      } else {
        ui_notice(ui, "%s", err.message);
      }
      ui->dirty = true;
      return true;
    }
  }
  // Go Live broadcast cards.
  for (size_t i = 0; i < sst->stream_cards; i++) {
    if (sst->card[i].watch.w > 0.0f && ui_in_rect(ev->x, ev->y, sst->card[i].watch) && ui->d.session) {
      ccord_error err;
      if (sst->card[i].watching) {
        (void)ccord_session_stop_watching(ui->d.session, &err);
      } else {
        (void)ccord_session_watch_stream(ui->d.session, ch->guild_id, ch->id, sst->card[i].user_id, &err);
      }
      ui->dirty = true;
      return true;
    }
  }
  for (size_t i = 0; i < ui->stage_watch_count; i++) {
    if (ui_in_rect(ev->x, ev->y, ui->stage_watch_rects[i]) && ui->d.session) {
      ccord_id uid = ui->stage_watch_users[i];
      ccord_error err;
      if (watching_remote && ws->user_id == uid) {
        (void)ccord_session_stop_watching(ui->d.session, &err);
      } else {
        (void)ccord_session_watch_stream(ui->d.session, ch->guild_id, ch->id, uid, &err);
      }
      ui->dirty = true;
      return true;
    }
  }
  ccord_id tile_uid = ui_voice_stage_user_at(ui, ev->x, ev->y);
  if (tile_uid != CCORD_ID_NONE) {
    ui_open_member_popover(ui, tile_uid, ev->x, ev->y, false);
    return true;
  }
  return false;
}

// ---- stream resize divider, fullscreen viewer & floating resizable PiP --------------------------

enum {
  PIP_EDGE_L = 1u,
  PIP_EDGE_R = 2u,
  PIP_EDGE_T = 4u,
  PIP_EDGE_B = 8u,
};

bool ui_stream_is_active(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return false;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  if (st && st->active) return true;
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  return ws && ws->active;
}

bool ui_stream_pip_active(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return false;
  if (ui->stream_fullscreen || ui->stage_channel != CCORD_ID_NONE) return false;
  if (ui->settings_open || ui_is_login_screen(ui)) return false;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  bool self_streaming = st && st->active;
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool watching_remote = !self_streaming && ws && ws->active;
  if (self_streaming) return !ui->stream_hide_preview;
  return watching_remote;
}

void ui_draw_stage_split_divider(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui_stage_split_active(ui)) return;
  ccord_rect handle = ui_stage_split_handle_rect(ui);
  if (handle.w <= 0.0f || handle.h <= 0.0f) return;
  float seam_x = ui->layout.stage_pane.x + ui->layout.stage_pane.w;
  bool dragging = ui->drag == DRAG_STAGE_SPLIT;
  bool hovered = !dragging && ui->mouse_in && ui_in_rect(ui->mx, ui->my, handle);
  if (dragging || hovered) {
    ccord_draw_rect(r, (ccord_rect){seam_x - 1.5f, handle.y, 3.0f, handle.h},
                    dragging ? COL(brand) : ui_mix(COL(border_subtle), COL(brand), 0.65f));
  } else {
    ccord_draw_rect(r, (ccord_rect){seam_x - 0.5f, handle.y, 1.0f, handle.h}, COL(border_subtle));
  }
}

static void stream_open_stage_for_active(ccord_ui *ui)
{
  if (!ui || !ui->d.store) return;
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  const ccord_voice_state *v = ui_self_voice(ui);
  ccord_id ch_id = CCORD_ID_NONE;
  if (st && st->active && st->channel_id != CCORD_ID_NONE) ch_id = st->channel_id;
  else if (ws && ws->active && ws->channel_id != CCORD_ID_NONE) ch_id = ws->channel_id;
  else if (v && v->channel_id != CCORD_ID_NONE) ch_id = v->channel_id;
  if (ch_id == CCORD_ID_NONE) return;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, ch_id);
  if (!ch) return;
  ccord_error err;
  if (ch->guild_id != ccord_store_selected_guild(ui->d.store)) {
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, ch->guild_id, &err);
    else (void)ccord_store_select_guild(ui->d.store, ch->guild_id, &err);
  }
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ui->discovery_open = false;
  ui->stage_channel = ch->id;
  ui->stage_armed = (v && v->channel_id == ch->id);
  ui->stage_scroll = 0.0f;
  ui_compute_layout(ui);
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui->dirty = true;
}

static float stream_fs_hud_alpha(const ccord_ui *ui, bool force_visible)
{
  if (force_visible || ui->now_ms == 0) return 1.0f;
  if (ui->mouse_in && ((ui->stream_fs_top_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_top_bar)) ||
                       (ui->stream_fs_bot_bar.w > 0.0f && ui_in_rect(ui->mx, ui->my, ui->stream_fs_bot_bar)))) {
    return 1.0f;
  }
  uint64_t idle = ui->now_ms >= ui->stream_fs_hud_ms ? (ui->now_ms - ui->stream_fs_hud_ms) : 0;
  if (idle < 2500u) return 1.0f;
  if (ui->anim.reduced_motion || idle >= 2800u) return 0.0f;
  return 1.0f - (float)(idle - 2500u) / 300.0f;
}

void ui_draw_stream_fullscreen(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui->stream_fullscreen) return;
  ui->stream_fs_exit_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_bot_exit_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_pause_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_btn1_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_btn2_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_fs_video_rect = (ccord_rect){0, 0, 0, 0};

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;
  if (!self_streaming && !watching_remote) {
    ui_stream_set_fullscreen(ui, false);
    return;
  }

  ccord_rect screen = {0.0f, 0.0f, ui->win_w, ui->win_h};
  ccord_draw_rect(r, screen, (ccord_color){0x0A, 0x0B, 0x0D, 255});

  int frame_w = 0, frame_h = 0;
  bool frame_present =
      ccord_render_has_texture(r, "__video_stream__", &frame_w, &frame_h) && frame_w > 0 && frame_h > 0;
  bool paused = (self_streaming && st->paused) || (watching_remote && ws->paused);
  bool hidden_preview = self_streaming && ui->stream_hide_preview;
  bool live = frame_present || hidden_preview;

  const char *badge = "LIVE";
  ccord_color badge_bg = COL(danger);
  ccord_color badge_fg = COL(text_on_brand);
  const char *placeholder = nullptr;
  if (paused) {
    badge = "PAUSED";
    badge_bg = COL(bg_tertiary);
    badge_fg = COL(text_header);
    placeholder = "Stream Paused";
  } else if (!live) {
    badge = self_streaming ? "STARTING" : "CONNECTING";
    badge_bg = COL(bg_tertiary);
    badge_fg = COL(text_header);
    placeholder = self_streaming ? "Starting capture & encoder..." : "Connecting to stream video...";
  }

  if (hidden_preview) {
    const char *hmsg = "Stream preview hidden — your screen share is still live";
    ccord_text_style hms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 hmt = ccord_text_measure(r, hmsg, strlen(hmsg), &hms, 0.0f);
    ccord_text_draw(r, hmsg, strlen(hmsg), (screen.w - hmt.x) * 0.5f, (screen.h - hmt.y) * 0.5f, &hms, 0.0f);
  } else if (frame_present) {
    ccord_rect vid = stream_fit_aspect(screen, frame_w, frame_h);
    ui->stream_fs_video_rect = vid;
    uint8_t alpha = paused ? 110 : 255;
    ccord_draw_texture(r, "__video_stream__", vid, alpha);
    if (paused) {
      const char *pmsg = "Stream Paused";
      ccord_text_style pms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
      ccord_vec2 pmt = ccord_text_measure(r, pmsg, strlen(pmsg), &pms, 0.0f);
      ccord_draw_rrect(r, (ccord_rect){(screen.w - pmt.x - 32.0f) * 0.5f, (screen.h - 40.0f) * 0.5f, pmt.x + 32.0f, 40.0f},
                       8.0f, COL(bg_floating));
      ccord_text_draw(r, pmsg, strlen(pmsg), (screen.w - pmt.x) * 0.5f, (screen.h - pmt.y) * 0.5f, &pms, 0.0f);
    }
  } else {
    ccord_text_style phs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 pht = ccord_text_measure(r, placeholder, strlen(placeholder), &phs, 0.0f);
    ccord_text_draw(r, placeholder, strlen(placeholder), (screen.w - pht.x) * 0.5f, (screen.h - pht.y) * 0.5f, &phs, 0.0f);
  }

  ui->stream_fs_top_bar = (ccord_rect){0.0f, 0.0f, screen.w, 54.0f};
  float bot_h = 50.0f;
  float bot_w = fminf(screen.w - 32.0f, self_streaming ? 404.0f : 294.0f);
  if (bot_w < 240.0f) bot_w = fminf(screen.w, 240.0f);
  ui->stream_fs_bot_bar = (ccord_rect){(screen.w - bot_w) * 0.5f, screen.h - bot_h - 18.0f, bot_w, bot_h};

  float hud_a = stream_fs_hud_alpha(ui, paused || !frame_present || hidden_preview);
  if (hud_a <= 0.02f) return;

  ccord_push_alpha(r, hud_a);

  // Top translucent HUD bar
  ccord_draw_rect(r, ui->stream_fs_top_bar, (ccord_color){0x0E, 0x10, 0x14, 210});
  ccord_draw_rect(r, (ccord_rect){0.0f, ui->stream_fs_top_bar.h - 1.0f, screen.w, 1.0f},
                  ui_alpha(COL(border_subtle), 0.5f));

  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, badge_fg);
  ccord_vec2 lt = ccord_text_measure(r, badge, strlen(badge), &ls, 0.0f);
  ccord_rect lb = {16.0f, 17.0f, lt.x + 16.0f, 20.0f};
  ccord_draw_rrect(r, lb, 4.0f, badge_bg);
  ccord_text_draw(r, badge, strlen(badge), lb.x + (lb.w - lt.x) * 0.5f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);

  char title[192];
  if (self_streaming) {
    const char *src_name = (ui->has_active_capture_source && ui->active_capture_source.title[0])
                             ? ui->active_capture_source.title
                             : "Screen Share";
    snprintf(title, sizeof title, "Your Stream  ·  %s  (%s · %s)", src_name,
             resolution_label(st->config.resolution), fps_label(st->config.fps));
  } else {
    const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
    if (frame_present) {
      snprintf(title, sizeof title, "%s's Screen Share  ·  %dx%d", wu ? wu->name : "Remote User", frame_w, frame_h);
    } else {
      snprintf(title, sizeof title, "%s's Screen Share", wu ? wu->name : "Remote User");
    }
  }

  ccord_rect exit_btn = {screen.w - 16.0f - 132.0f, 12.0f, 132.0f, 30.0f};
  ui->stream_fs_exit_rect = exit_btn;
  bool exit_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, exit_btn);
  ccord_draw_rrect(r, exit_btn, 5.0f, exit_hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ui_icon_fullscreen(r, exit_btn.x + 16.0f, exit_btn.y + exit_btn.h * 0.5f, 13.0f, COL(text_header), true);
  ccord_text_style exs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  const char *ex_lbl = "Exit Fullscreen";
  ccord_vec2 ext = ccord_text_measure(r, ex_lbl, strlen(ex_lbl), &exs, 0.0f);
  ccord_text_draw(r, ex_lbl, strlen(ex_lbl), exit_btn.x + 28.0f, exit_btn.y + (exit_btn.h - ext.y) * 0.5f, &exs, 0.0f);

  const char *hint = "Esc to exit";
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_vec2 ht = ccord_text_measure(r, hint, strlen(hint), &hs, 0.0f);
  float title_right = exit_btn.x - 14.0f;
  if (screen.w >= 540.0f) {
    float hx = exit_btn.x - 14.0f - ht.x;
    ccord_text_draw(r, hint, strlen(hint), hx, (ui->stream_fs_top_bar.h - ht.y) * 0.5f, &hs, 0.0f);
    title_right = hx - 16.0f;
  }
  float title_x = lb.x + lb.w + 12.0f;
  float title_w = title_right - title_x;
  if (title_w > 32.0f) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_draw_ellipsis(r, title, strlen(title), title_x, (ui->stream_fs_top_bar.h - ui_line_h(ui, ts.size)) * 0.5f,
                             title_w, &ts);
  }

  // Bottom floating control pill
  ccord_rect bar = ui->stream_fs_bot_bar;
  ccord_draw_rrect(r, (ccord_rect){bar.x - 1.0f, bar.y - 1.0f, bar.w + 2.0f, bar.h + 2.0f}, 11.0f,
                   ui_alpha(COL(border_subtle), 0.7f));
  ccord_draw_rrect(r, bar, 10.0f, (ccord_color){0x14, 0x16, 0x1B, 232});

  ccord_text_style b1s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style b2s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
  const char *b1_lbl = self_streaming ? (ui->stream_hide_preview ? "Show Preview" : "Hide Preview")
                                      : (ws->paused ? "Resume" : "Pause");
  const char *b2_lbl = self_streaming ? "Stop Streaming" : "Stop Watching";
  float by = bar.y + (bar.h - 30.0f) * 0.5f;
  float bx = bar.x + 12.0f;
  float gap = 8.0f;

  if (self_streaming) {
    const char *plbl = st->paused ? "Resume" : "Pause";
    ccord_rect pb = {bx, by, 82.0f, 30.0f};
    ui->stream_fs_pause_rect = pb;
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pb);
    ccord_draw_rrect(r, pb, 5.0f, hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ccord_vec2 pt = ccord_text_measure(r, plbl, strlen(plbl), &b1s, 0.0f);
    ccord_text_draw(r, plbl, strlen(plbl), pb.x + (pb.w - pt.x) * 0.5f, pb.y + (pb.h - pt.y) * 0.5f, &b1s, 0.0f);
    bx += pb.w + gap;
  }

  float b1_w = self_streaming ? 116.0f : 96.0f;
  ccord_rect b1 = {bx, by, b1_w, 30.0f};
  ui->stream_fs_btn1_rect = b1;
  bool b1_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b1);
  ccord_draw_rrect(r, b1, 5.0f, b1_hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_vec2 b1t = ccord_text_measure(r, b1_lbl, strlen(b1_lbl), &b1s, 0.0f);
  ccord_text_draw(r, b1_lbl, strlen(b1_lbl), b1.x + (b1.w - b1t.x) * 0.5f, b1.y + (b1.h - b1t.y) * 0.5f, &b1s, 0.0f);
  bx += b1.w + gap;

  float b2_w = 124.0f;
  ccord_rect b2 = {bx, by, b2_w, 30.0f};
  ui->stream_fs_btn2_rect = b2;
  bool b2_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b2);
  ccord_draw_rrect(r, b2, 5.0f, b2_hov ? ui_mix(COL(danger), COL(bg_item_hover), 0.2f) : COL(danger));
  ccord_vec2 b2t = ccord_text_measure(r, b2_lbl, strlen(b2_lbl), &b2s, 0.0f);
  ccord_text_draw(r, b2_lbl, strlen(b2_lbl), b2.x + (b2.w - b2t.x) * 0.5f, b2.y + (b2.h - b2t.y) * 0.5f, &b2s, 0.0f);

  ccord_rect bex = {bar.x + bar.w - 12.0f - 34.0f, by, 34.0f, 30.0f};
  if (bex.x >= b2.x + b2.w + 4.0f) {
    ui->stream_fs_bot_exit_rect = bex;
    bool bex_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, bex);
    ccord_draw_rrect(r, bex, 5.0f, bex_hov ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_fullscreen(r, bex.x + bex.w * 0.5f, bex.y + bex.h * 0.5f, 14.0f, COL(text_header), true);
  }

  ccord_pop_alpha(r);
}

bool ui_stream_fullscreen_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ui->stream_fullscreen || !ev) return false;
  ui->stream_fs_hud_ms = ui->now_ms;
  if (ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return true;

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;

  if ((ui->stream_fs_exit_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_fs_exit_rect)) ||
      (ui->stream_fs_bot_exit_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_fs_bot_exit_rect))) {
    ui_stream_set_fullscreen(ui, false);
    return true;
  }
  if (self_streaming && ui->stream_fs_pause_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_fs_pause_rect)) {
    if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !st->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui->stream_fs_btn1_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_fs_btn1_rect)) {
    if (self_streaming) {
      ui->stream_hide_preview = !ui->stream_hide_preview;
    } else if (ui->d.session) {
      ccord_error err;
      (void)ccord_session_set_stream_paused(ui->d.session, !ws->paused, &err);
    }
    ui->dirty = true;
    return true;
  }
  if ((self_streaming || watching_remote) && ui->stream_fs_btn2_rect.w > 0.0f &&
      ui_in_rect(ev->x, ev->y, ui->stream_fs_btn2_rect)) {
    ccord_error err;
    if (self_streaming && ui->d.session) {
      ccord_id gid = st->guild_id;
      if (gid == CCORD_ID_NONE) {
        const ccord_channel *ch = ccord_store_channel(ui->d.store, st->channel_id);
        if (ch) gid = ch->guild_id;
      }
      (void)ccord_session_stop_stream(ui->d.session, gid, &err);
    } else if (watching_remote && ui->d.session) {
      (void)ccord_session_stop_watching(ui->d.session, &err);
    }
    ui_stream_set_fullscreen(ui, false);
    return true;
  }
  if (ev->clicks >= 2) {
    ui_stream_set_fullscreen(ui, false);
    return true;
  }
  ui->dirty = true;
  return true;
}

static uint8_t pip_edge_at(const ccord_ui *ui, float x, float y)
{
  ccord_rect rc = ui->stream_pip_rect;
  if (rc.w <= 0.0f || rc.h <= 0.0f || !ui_in_rect(x, y, rc)) return 0;
  // Control buttons always win over edge/corner resize handles.
  if ((ui->stream_pip_close_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_close_rect)) ||
      (ui->stream_pip_fs_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_fs_rect)) ||
      (ui->stream_pip_stage_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_stage_rect))) {
    return 0;
  }
  const float edge = 8.0f;
  const float corner = 14.0f;
  bool nl = x <= rc.x + edge;
  bool nr = x >= rc.x + rc.w - edge;
  bool nt = y <= rc.y + edge;
  bool nb = y >= rc.y + rc.h - edge;
  if (nl || nr) {
    if (y <= rc.y + corner) nt = true;
    if (y >= rc.y + rc.h - corner) nb = true;
  }
  if (nt || nb) {
    if (x <= rc.x + corner) nl = true;
    if (x >= rc.x + rc.w - corner) nr = true;
  }
  uint8_t mask = 0;
  if (nl) mask |= PIP_EDGE_L;
  if (nr) mask |= PIP_EDGE_R;
  if (nt) mask |= PIP_EDGE_T;
  if (nb) mask |= PIP_EDGE_B;
  return mask;
}

static ccord_cursor_kind pip_cursor_for_edge(uint8_t edge)
{
  bool h = (edge & (PIP_EDGE_L | PIP_EDGE_R)) != 0;
  bool v = (edge & (PIP_EDGE_T | PIP_EDGE_B)) != 0;
  if (h && v) {
    bool nwse = ((edge & PIP_EDGE_L) && (edge & PIP_EDGE_T)) || ((edge & PIP_EDGE_R) && (edge & PIP_EDGE_B));
    return nwse ? CCORD_CURSOR_NWSE_RESIZE : CCORD_CURSOR_NESW_RESIZE;
  }
  if (h) return CCORD_CURSOR_EW_RESIZE;
  if (v) return CCORD_CURSOR_NS_RESIZE;
  return CCORD_CURSOR_MOVE;
}

ccord_cursor_kind ui_stream_pip_cursor(const ccord_ui *ui, float x, float y)
{
  if (!ui) return CCORD_CURSOR_DEFAULT;
  if (ui->drag == DRAG_STREAM_PIP_RESIZE) return pip_cursor_for_edge(ui->stream_pip_resize_edge);
  if (ui->drag == DRAG_STREAM_PIP_MOVE) return CCORD_CURSOR_MOVE;
  if (ui->stream_pip_rect.w <= 0.0f || !ui_in_rect(x, y, ui->stream_pip_rect)) return CCORD_CURSOR_DEFAULT;
  if ((ui->stream_pip_close_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_close_rect)) ||
      (ui->stream_pip_fs_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_fs_rect)) ||
      (ui->stream_pip_stage_rect.w > 0.0f && ui_in_rect(x, y, ui->stream_pip_stage_rect))) {
    return CCORD_CURSOR_POINTER;
  }
  uint8_t edge = pip_edge_at(ui, x, y);
  if (edge != 0) return pip_cursor_for_edge(edge);
  return CCORD_CURSOR_POINTER;
}

static void pip_bounds(const ccord_ui *ui, float aspect, float *out_min_w, float *out_max_w)
{
  float max_w = clampf(ui->win_w * 0.72f, 200.0f, 960.0f);
  float max_h = clampf(ui->win_h * 0.72f, 120.0f, 640.0f);
  if (max_w / aspect > max_h) max_w = maxf(max_h * aspect, 200.0f);
  if (max_w > ui->win_w - 16.0f) max_w = maxf(ui->win_w - 16.0f, 160.0f);
  float min_w = fminf(200.0f, max_w);
  *out_min_w = min_w;
  *out_max_w = max_w;
}

void ui_draw_stream_pip(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui->stream_pip_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_pip_fs_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_pip_stage_rect = (ccord_rect){0, 0, 0, 0};
  ui->stream_pip_close_rect = (ccord_rect){0, 0, 0, 0};
  if (!ui_stream_pip_active(ui)) return;

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;
  if (!self_streaming && !watching_remote) return;

  int frame_w = 0, frame_h = 0;
  bool frame_present =
      ccord_render_has_texture(r, "__video_stream__", &frame_w, &frame_h) && frame_w > 0 && frame_h > 0;
  float aspect = (frame_w > 0 && frame_h > 0) ? clampf((float)frame_w / (float)frame_h, 0.5f, 2.5f) : (16.0f / 9.0f);
  ui->stream_pip_aspect = aspect;

  float min_w = 200.0f, max_w = 640.0f;
  pip_bounds(ui, aspect, &min_w, &max_w);
  float w = clampf(ui->stream_pip_w > 1.0f ? ui->stream_pip_w : 320.0f, min_w, max_w);
  float h = w / aspect;
  float px, py;
  if (!ui->stream_pip_positioned) {
    ccord_rect ma = ui_rect(ui->layout.message_area);
    px = (ma.w >= w + 24.0f) ? (ma.x + ma.w - w - 16.0f) : (ui->win_w - w - 16.0f);
    py = (ma.h >= h + 24.0f) ? (ma.y + ma.h - h - 16.0f) : (ui->win_h - h - 16.0f);
  } else {
    px = ui->stream_pip_x;
    py = ui->stream_pip_y;
  }
  px = clampf(px, 8.0f, maxf(8.0f, ui->win_w - w - 8.0f));
  py = clampf(py, 8.0f, maxf(8.0f, ui->win_h - h - 8.0f));

  ccord_rect pip = {px, py, w, h};
  ui->stream_pip_rect = pip;
  bool resizing = ui->drag == DRAG_STREAM_PIP_RESIZE;
  bool moving = ui->drag == DRAG_STREAM_PIP_MOVE;
  bool hovered = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pip);

  // Subtle drop shadow + border ring + dark media plate
  ccord_draw_rrect(r, (ccord_rect){pip.x - 2.0f, pip.y + 2.0f, pip.w + 4.0f, pip.h + 4.0f}, 10.0f,
                   (ccord_color){0, 0, 0, 100});
  ccord_draw_rrect(r, (ccord_rect){pip.x - 1.5f, pip.y - 1.5f, pip.w + 3.0f, pip.h + 3.0f}, 9.5f,
                   (resizing || moving) ? COL(brand) : COL(border_subtle));
  ccord_draw_rrect(r, pip, 8.0f, (ccord_color){0x14, 0x15, 0x18, 255});

  bool paused = (self_streaming && st->paused) || (watching_remote && ws->paused);
  ccord_rect vid_bounds = {pip.x + 2.0f, pip.y + 2.0f, maxf(pip.w - 4.0f, 0.0f), maxf(pip.h - 4.0f, 0.0f)};
  if (frame_present) {
    ccord_rect vid = stream_fit_aspect(vid_bounds, frame_w, frame_h);
    ccord_draw_texture_rounded(r, "__video_stream__", vid, 6.0f, paused ? 110 : 255);
    if (paused) {
      const char *pmsg = "Paused";
      ccord_text_style pms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
      ccord_vec2 pmt = ccord_text_measure(r, pmsg, strlen(pmsg), &pms, 0.0f);
      ccord_draw_rrect(r, (ccord_rect){vid.x + (vid.w - pmt.x - 18.0f) * 0.5f, vid.y + (vid.h - 24.0f) * 0.5f,
                                       pmt.x + 18.0f, 24.0f},
                       5.0f, COL(bg_floating));
      ccord_text_draw(r, pmsg, strlen(pmsg), vid.x + (vid.w - pmt.x) * 0.5f, vid.y + (vid.h - pmt.y) * 0.5f, &pms, 0.0f);
    }
  } else {
    const char *pmsg = paused ? "Stream Paused" : (self_streaming ? "Starting stream..." : "Connecting...");
    ccord_text_style phs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    ccord_vec2 pht = ccord_text_measure(r, pmsg, strlen(pmsg), &phs, 0.0f);
    ccord_text_draw(r, pmsg, strlen(pmsg), pip.x + (pip.w - pht.x) * 0.5f, pip.y + (pip.h - pht.y) * 0.5f, &phs, 0.0f);
  }

  // Top translucent overlay bar with LIVE badge, streamer name, Return-to-Stage, Fullscreen, and Close buttons.
  ccord_rect top = {pip.x + 2.0f, pip.y + 2.0f, maxf(pip.w - 4.0f, 0.0f), 30.0f};
  ccord_draw_rrect(r, top, 6.0f, (ccord_color){0x0E, 0x10, 0x14, (uint8_t)(hovered ? 210 : 145)});

  const char *badge = paused ? "PAUSED" : (frame_present ? "LIVE" : "...");
  ccord_color badge_bg = (!paused && frame_present) ? COL(danger) : COL(bg_tertiary);
  ccord_color badge_fg = (!paused && frame_present) ? COL(text_on_brand) : COL(text_header);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, badge_fg);
  ccord_vec2 lt = ccord_text_measure(r, badge, strlen(badge), &ls, 0.0f);
  ccord_rect lb = {top.x + 6.0f, top.y + 6.0f, lt.x + 10.0f, 16.0f};
  ccord_draw_rrect(r, lb, 3.0f, badge_bg);
  ccord_text_draw(r, badge, strlen(badge), lb.x + (lb.w - lt.x) * 0.5f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);

  ccord_rect close_b = {top.x + top.w - 6.0f - 22.0f, top.y + 4.0f, 22.0f, 22.0f};
  ccord_rect fs_b = {close_b.x - 4.0f - 22.0f, top.y + 4.0f, 22.0f, 22.0f};
  ccord_rect stage_b = {fs_b.x - 4.0f - 22.0f, top.y + 4.0f, 22.0f, 22.0f};
  ui->stream_pip_close_rect = close_b;
  ui->stream_pip_fs_rect = fs_b;
  ui->stream_pip_stage_rect = stage_b;

  bool chov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, close_b);
  bool fhov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, fs_b);
  bool shov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, stage_b);
  ccord_draw_rrect(r, stage_b, 4.0f, shov ? COL(bg_item_hover) : (ccord_color){0x26, 0x29, 0x30, 200});
  ui_icon_stage_return(r, stage_b.x + stage_b.w * 0.5f, stage_b.y + stage_b.h * 0.5f, 11.0f, COL(text_header));
  ccord_draw_rrect(r, fs_b, 4.0f, fhov ? COL(bg_item_hover) : (ccord_color){0x26, 0x29, 0x30, 200});
  ui_icon_fullscreen(r, fs_b.x + fs_b.w * 0.5f, fs_b.y + fs_b.h * 0.5f, 11.0f, COL(text_header), false);
  ccord_draw_rrect(r, close_b, 4.0f, chov ? COL(danger) : (ccord_color){0x26, 0x29, 0x30, 200});
  ui_icon_close(r, close_b.x + close_b.w * 0.5f, close_b.y + close_b.h * 0.5f, 11.0f,
                chov ? COL(text_on_brand) : COL(text_header));

  const char *who = "Your Stream";
  if (watching_remote) {
    const ccord_user *wu = ccord_store_user(ui->d.store, ws->user_id);
    who = wu ? wu->name : "Live Stream";
  }
  float name_x = lb.x + lb.w + 6.0f;
  float name_w = stage_b.x - name_x - 6.0f;
  if (name_w > 20.0f) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
    ccord_text_draw_ellipsis(r, who, strlen(who), name_x, top.y + (top.h - ui_line_h(ui, ns.size)) * 0.5f, name_w, &ns);
  }

  // Subtle corner resize grip ticks on hover or during resize.
  if (hovered || resizing) {
    ccord_color gc = resizing ? COL(brand) : ui_alpha(COL(text_header), 0.75f);
    float rx = pip.x + pip.w - 4.0f, ry = pip.y + pip.h - 4.0f;
    ccord_draw_line(r, (ccord_vec2){rx - 8.0f, ry}, (ccord_vec2){rx, ry - 8.0f}, 1.5f, gc);
    ccord_draw_line(r, (ccord_vec2){rx - 4.0f, ry}, (ccord_vec2){rx, ry - 4.0f}, 1.5f, gc);
  }
}

bool ui_stream_pip_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return false;
  if (!ui_stream_pip_active(ui) || ui->stream_pip_rect.w <= 0.0f) return false;
  if (!ui_in_rect(ev->x, ev->y, ui->stream_pip_rect)) return false;

  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  const ccord_watched_stream *ws = ui->d.session ? ccord_session_watched_stream(ui->d.session) : nullptr;
  bool self_streaming = st && st->active;
  bool watching_remote = !self_streaming && ws && ws->active;

  if (ui->stream_pip_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_close_rect)) {
    if (self_streaming) {
      ui->stream_hide_preview = true;
    } else if (watching_remote && ui->d.session) {
      ccord_error err;
      (void)ccord_session_stop_watching(ui->d.session, &err);
    }
    ui->dirty = true;
    return true;
  }
  if (ui->stream_pip_fs_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_fs_rect)) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  if (ui->stream_pip_stage_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->stream_pip_stage_rect)) {
    stream_open_stage_for_active(ui);
    return true;
  }

  uint8_t edge = pip_edge_at(ui, ev->x, ev->y);
  if (edge != 0) {
    if (ev->clicks >= 2) {
      ui->stream_pip_w = 320.0f;
      ui->stream_pip_positioned = false;
      ui_settings_save(ui);
      ui->dirty = true;
      return true;
    }
    ui->stream_pip_resize_edge = edge;
    ui->stream_pip_anchor = ui->stream_pip_rect;
    ui->stream_pip_drag_ox = ev->x;
    ui->stream_pip_drag_oy = ev->y;
    ui->stream_pip_positioned = true;
    ui->stream_pip_x = ui->stream_pip_rect.x;
    ui->stream_pip_y = ui->stream_pip_rect.y;
    ui->drag = DRAG_STREAM_PIP_RESIZE;
    ui->dirty = true;
    return true;
  }

  if (ev->clicks >= 2) {
    ui_stream_set_fullscreen(ui, true);
    return true;
  }
  ui->stream_pip_anchor = ui->stream_pip_rect;
  ui->stream_pip_drag_ox = ev->x;
  ui->stream_pip_drag_oy = ev->y;
  ui->stream_pip_moved = false;
  ui->drag = DRAG_STREAM_PIP_MOVE;
  ui->dirty = true;
  return true;
}

void ui_stream_pip_drag_move(ccord_ui *ui, float x, float y)
{
  if (!ui) return;
  if (ui->drag == DRAG_STREAM_PIP_MOVE) {
    float dx = x - ui->stream_pip_drag_ox;
    float dy = y - ui->stream_pip_drag_oy;
    if (!ui->stream_pip_moved && (fabsf(dx) >= 4.0f || fabsf(dy) >= 4.0f)) ui->stream_pip_moved = true;
    if (ui->stream_pip_moved) {
      ui->stream_pip_positioned = true;
      ui->stream_pip_x = clampf(ui->stream_pip_anchor.x + dx, 8.0f, maxf(8.0f, ui->win_w - ui->stream_pip_anchor.w - 8.0f));
      ui->stream_pip_y = clampf(ui->stream_pip_anchor.y + dy, 8.0f, maxf(8.0f, ui->win_h - ui->stream_pip_anchor.h - 8.0f));
      ui->dirty = true;
    }
    return;
  }
  if (ui->drag == DRAG_STREAM_PIP_RESIZE) {
    float aspect = ui->stream_pip_aspect > 0.1f ? ui->stream_pip_aspect : (16.0f / 9.0f);
    float min_w = 200.0f, max_w = 640.0f;
    pip_bounds(ui, aspect, &min_w, &max_w);
    uint8_t mask = ui->stream_pip_resize_edge;
    ccord_rect a = ui->stream_pip_anchor;
    float dx = x - ui->stream_pip_drag_ox;
    float dy = y - ui->stream_pip_drag_oy;
    bool has_x = (mask & (PIP_EDGE_L | PIP_EDGE_R)) != 0;
    bool has_y = (mask & (PIP_EDGE_T | PIP_EDGE_B)) != 0;
    float wx = a.w + ((mask & PIP_EDGE_R) ? dx : -dx);
    float hy = a.h + ((mask & PIP_EDGE_B) ? dy : -dy);
    float wy = hy * aspect;
    float new_w = a.w;
    if (has_x && has_y) {
      new_w = fabsf(wx - a.w) >= fabsf(wy - a.w) ? wx : wy;
    } else if (has_x) {
      new_w = wx;
    } else if (has_y) {
      new_w = wy;
    }
    new_w = clampf(new_w, min_w, max_w);
    float new_h = new_w / aspect;
    float new_x = (mask & PIP_EDGE_L) ? (a.x + a.w - new_w) : a.x;
    float new_y = (mask & PIP_EDGE_T) ? (a.y + a.h - new_h) : a.y;
    new_x = clampf(new_x, 8.0f, maxf(8.0f, ui->win_w - new_w - 8.0f));
    new_y = clampf(new_y, 8.0f, maxf(8.0f, ui->win_h - new_h - 8.0f));
    ui->stream_pip_w = new_w;
    ui->stream_pip_x = new_x;
    ui->stream_pip_y = new_y;
    ui->stream_pip_positioned = true;
    ui->dirty = true;
  }
}

void ui_stream_pip_drag_end(ccord_ui *ui)
{
  if (!ui) return;
  if (ui->drag == DRAG_STREAM_PIP_MOVE) {
    bool was_click = !ui->stream_pip_moved;
    ui->drag = DRAG_NONE;
    if (was_click) stream_open_stage_for_active(ui);
    ui->dirty = true;
  } else if (ui->drag == DRAG_STREAM_PIP_RESIZE) {
    ui->drag = DRAG_NONE;
    ui_settings_save(ui);
    ui->dirty = true;
  }
}

// ---------------------------------------------------------------- search / pinned popovers

void ui_draw_search_popover(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->search_open) return;
  ccord_rect box = ui_search_popover_rect(ui);
  uis_tokens tk = uis_tokens_of(ui);
  uis_popover_frame(ui, r, box);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_draw(r, "SEARCH MESSAGES", 15, box.x + 10.0f, box.y + 7.0f, &ls, 0.0f);
  ccord_rect input = {box.x + UIS_SP_2, box.y + 24.0f, box.w - 2.0f * UIS_SP_2, UIS_H_INPUT};
  uis_state ist = {.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, input), .focused = true};
  (void)uis_input_padded(ui, r, input, 30.0f, UIS_SP_2, &ui->search_edit, "Filter this channel", ist, false, "srchin", 0);
  ui_icon_search(r, input.x + 15.0f, input.y + input.h * 0.5f, 14.0f, COL(text_muted));
}

static void draw_pin_row(ccord_ui *ui, ccord_renderer *r, const ccord_message *m, ccord_rect ir)
{
  const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
  ui_draw_avatar(ui, r, u, ir.x + 14.0f, ir.y + ir.h * 0.5f, 26.0f, (ccord_color){0, 0, 0, 0}, false);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_secondary));
  ccord_text_draw_ellipsis(r, u ? u->name : "Unknown User", strlen(u ? u->name : "Unknown User"), ir.x + 32.0f, ir.y + 3.0f,
                           ir.w - 40.0f, &ns);
  char pline[256];
  size_t pl = ccord_message_preview(m, pline, sizeof pline);
  pl = ui_streamer_mask_inplace(ui, pline, pl, true);
  ccord_text_draw_ellipsis(r, pline, pl, ir.x + 32.0f, ir.y + 18.0f, ir.w - 40.0f, &ts);
  ui_icon_pin(r, ir.x + ir.w - 10.0f, ir.y + 12.0f, 12.0f, COL(text_muted));
}

void ui_draw_pins_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->npins_vis = 0;
  ui->pins_load_more_rect = (ccord_rect){0};
  ui->pins_thumb = (ccord_rect){0};
  ui->dbg_pins_total = 0;
  ui->dbg_pins_has_more = false;
  if (!ui->pins_open) return;

  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  const ccord_message *pins[256];
  size_t npins = 0;
  if (ccord_store_channel_pins_loaded(ui->d.store, ch)) {
    size_t bid_cnt = 0;
    const ccord_id *bids = ccord_store_channel_pin_ids(ui->d.store, ch, &bid_cnt);
    for (size_t i = 0; bids && i < bid_cnt && npins < 256u; i++) {
      const ccord_message *m = ccord_store_find_message(ui->d.store, ch, bids[i]);
      if (m) pins[npins++] = m;
    }
  } else {
    size_t total = ccord_store_message_count(ui->d.store, ch);
    for (size_t i = total; i > 0 && npins < 256u; i--) {
      const ccord_message *m = ccord_store_message_at(ui->d.store, ch, i - 1u);
      if (m && m->pinned) pins[npins++] = m;
    }
  }
  ui->dbg_pins_total = npins;
  ui->dbg_pins_has_more = ccord_store_channel_pins_has_more(ui->d.store, ch);

  ccord_rect box = ui_pins_popover_rect(ui);
  uis_popover_frame(ui, r, box);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw(r, "Pinned Messages", 15, box.x + 10.0f, box.y + 9.0f, &ls, 0.0f);
  uis_divider(ui, r, box.x + UIS_SP_2, box.y + 30.0f, box.w - 2.0f * UIS_SP_2);

  if (npins == 0) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_text_draw(r, "No pinned messages in this channel.", 35, box.x + 10.0f, box.y + 40.0f, &es, box.w - 20.0f);
    return;
  }

  float footer_h = ui->dbg_pins_has_more ? 34.0f : 0.0f;
  float list_y = box.y + 34.0f;
  float list_h = box.h - 34.0f - 8.0f - footer_h;
  if (list_h < 40.0f) list_h = 40.0f;
  float content_h = (float)npins * 44.0f;
  float max_scroll = content_h - list_h;
  if (max_scroll < 0.0f) max_scroll = 0.0f;
  ui->pins_max = max_scroll;
  if (ui->pins_scroll > max_scroll) ui->pins_scroll = max_scroll;
  if (ui->pins_scroll < 0.0f) ui->pins_scroll = 0.0f;

  ccord_push_clip(r, (ccord_rect){box.x, list_y, box.w, list_h});
  for (size_t i = 0; i < npins; i++) {
    float ry = list_y + (float)i * 44.0f - ui->pins_scroll;
    if (ry + 40.0f < list_y || ry > list_y + list_h) continue;
    ccord_rect ir = {box.x + 8.0f, ry, box.w - 16.0f, 40.0f};
    draw_pin_row(ui, r, pins[i], ir);
    if (ui->npins_vis < 64u) {
      ui->pins_vis_ids[ui->npins_vis] = pins[i]->id;
      ui->pins_vis_rects[ui->npins_vis] = ir;
      ui->npins_vis++;
    }
  }
  ccord_pop_clip(r);
  ui->pins_thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){box.x, list_y, box.w, list_h}, content_h,
                                          ui->pins_scroll, "pinsscroll", 0);

  if (ui->dbg_pins_has_more) {
    bool loading = ui->d.session && ccord_session_pins_loading(ui->d.session, ch);
    ccord_rect lm = {box.x + UIS_SP_2, box.y + box.h - 34.0f, box.w - 2.0f * UIS_SP_2, UIS_H_BTN_SM};
    ui->pins_load_more_rect = lm;
    bool hv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, lm);
    uis_button(ui, r, lm, loading ? "Loading..." : "Load more", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = hv, .disabled = loading}, "pinmore", 0);
  }
}

void ui_draw_reactions_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->nreactions_tabs = 0;
  ui->dbg_reactions_users = 0;
  ui->reactions_popover_rect = (ccord_rect){0};
  ui->reactions_close_rect = (ccord_rect){0};
  ui->reactions_thumb = (ccord_rect){0, 0, 0, 0};
  ui->reactions_tab_thumb = (ccord_rect){0, 0, 0, 0};
  if (!ui->reactions_open) return;

  const ccord_message *m = ccord_store_find_message(ui->d.store, ui->reactions_channel_id, ui->reactions_msg_id);
  if (!m || !message_has_reactions(m)) {
    ui->reactions_open = false;
    return;
  }

  float w = 440.0f, h = 300.0f;
  if (w > ui->win_w - 32.0f && ui->win_w > 64.0f) w = ui->win_w - 32.0f;
  if (h > ui->win_h - 32.0f && ui->win_h > 64.0f) h = ui->win_h - 32.0f;
  ccord_rect box = {(ui->win_w - w) * 0.5f, (ui->win_h - h) * 0.5f, w, h};
  ui->reactions_popover_rect = box;

  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, (ccord_color){0, 0, 0, 120});
  ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, 8.0f, COL(border_subtle));
  ccord_draw_rrect(r, box, 7.0f, COL(bg_floating));

  float side_w = 126.0f;
  ccord_draw_rect(r, (ccord_rect){box.x + side_w, box.y, 1.0f, box.h}, COL(divider));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw(r, "Reactions", 9, box.x + 12.0f, box.y + 10.0f, &hs, 0.0f);

  ccord_rect close_r = {box.x + box.w - 34.0f, box.y + 8.0f, 26.0f, 24.0f};
  ui->reactions_close_rect = close_r;
  ccord_draw_rrect(r, close_r, 4.0f, COL(bg_tertiary));
  ccord_text_draw(r, "X", 1, close_r.x + 9.0f, close_r.y + 4.0f, &hs, 0.0f);

  // Emoji tab column: count kinds, clamp scroll, keep the active tab in view, then clip + cull.
  ccord_rect tab_view = {box.x, box.y + 38.0f, side_w, box.h - 46.0f};
  size_t ntabs = 0;
  for (size_t i = 0; i < m->reaction_kind_count && ntabs < 16u; i++) {
    const ccord_reaction *rx = &m->reactions[i];
    if (!rx->emoji || rx->count == 0) continue;
    ntabs++;
  }
  float tab_content_h = (float)ntabs * 32.0f;
  float tab_max_s = maxf(tab_content_h - tab_view.h, 0.0f);
  ui->reactions_tab_max = tab_max_s;
  ui->reactions_tab_scroll = clampf(ui->reactions_tab_scroll, 0.0f, tab_max_s);
  if (ui->reactions_tab < ntabs) {
    float sel_top = (float)ui->reactions_tab * 32.0f;
    float sel_bot = sel_top + 28.0f;
    if (sel_top < ui->reactions_tab_scroll) ui->reactions_tab_scroll = sel_top;
    else if (sel_bot > ui->reactions_tab_scroll + tab_view.h)
      ui->reactions_tab_scroll = clampf(sel_bot - tab_view.h, 0.0f, tab_max_s);
  }
  ccord_push_clip(r, tab_view);
  size_t tab_idx = 0;
  for (size_t i = 0; i < m->reaction_kind_count && tab_idx < 16u; i++) {
    const ccord_reaction *rx = &m->reactions[i];
    if (!rx->emoji || rx->count == 0) continue;
    float ty = tab_view.y + (float)tab_idx * 32.0f - ui->reactions_tab_scroll;
    ccord_rect tr = {box.x + 8.0f, ty, side_w - 16.0f, 28.0f};
    snprintf(ui->reactions_tab_emojis[tab_idx], sizeof ui->reactions_tab_emojis[tab_idx], "%.95s", rx->emoji);
    if (ty + 28.0f < tab_view.y || ty > tab_view.y + tab_view.h) {
      ui->reactions_tab_rects[tab_idx] = (ccord_rect){0, 0, 0, 0};
      tab_idx++;
      continue;
    }
    ui->reactions_tab_rects[tab_idx] = tr;
    bool active = (tab_idx == ui->reactions_tab);
    ccord_draw_rrect(r, tr, 5.0f, active ? COL(bg_item_selected) : COL(bg_secondary));
    char tlbl[120];
    if (rx->burst_count > 0) {
      snprintf(tlbl, sizeof tlbl, "%s  %u (%u*)", rx->emoji, (unsigned)rx->count, (unsigned)rx->burst_count);
    } else {
      snprintf(tlbl, sizeof tlbl, "%s  %u", rx->emoji, (unsigned)rx->count);
    }
    ccord_text_style ts = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small) - 1.0f,
                                   active ? COL(text_header) : COL(text_secondary));
    ccord_text_draw_ellipsis(r, tlbl, strlen(tlbl), tr.x + 8.0f, tr.y + 6.0f, tr.w - 14.0f, &ts);
    tab_idx++;
  }
  ccord_pop_clip(r);
  ui->nreactions_tabs = tab_idx;
  if (ui->reactions_tab >= ui->nreactions_tabs && ui->nreactions_tabs > 0) {
    ui->reactions_tab = 0;
  }
  ui->reactions_tab_thumb = ui_draw_list_scrollbar(ui, r, tab_view, tab_content_h, ui->reactions_tab_scroll,
                                                   "reactiontabs", 0);

  float rx_x = box.x + side_w + 12.0f;
  float rx_y = box.y + 38.0f;
  float rx_w = box.w - side_w - 24.0f;
  float rx_h = box.h - 48.0f;

  size_t nuc = 0;
  bool loading = false;
  const ccord_user *users = nullptr;
  if (ui->d.session && ui->nreactions_tabs > 0) {
    ccord_id rc_ch = CCORD_ID_NONE, rc_msg = CCORD_ID_NONE;
    const char *rc_emoji = nullptr;
    size_t raw_cnt = 0;
    bool raw_load = false;
    const ccord_user *raw_users = ccord_session_reaction_users(ui->d.session, &rc_ch, &rc_msg, &rc_emoji,
                                                               &raw_cnt, &raw_load);
    if (rc_ch == ui->reactions_channel_id && rc_msg == ui->reactions_msg_id &&
        rc_emoji && strcmp(rc_emoji, ui->reactions_tab_emojis[ui->reactions_tab]) == 0) {
      users = raw_users;
      nuc = raw_cnt;
      loading = raw_load;
    }
  }
  ui->dbg_reactions_users = nuc;

  ccord_push_clip(r, (ccord_rect){rx_x, rx_y, rx_w, rx_h});
  if (nuc == 0) {
    const char *msg = loading ? "Loading reactions..." : "No reaction users loaded.";
    ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_text_draw(r, msg, strlen(msg), rx_x, rx_y + 8.0f, &ms, rx_w);
  } else {
    float max_s = (float)nuc * 40.0f - rx_h;
    if (max_s < 0.0f) max_s = 0.0f;
    ui->reactions_max = max_s;
    if (ui->reactions_scroll > max_s) ui->reactions_scroll = max_s;
    if (ui->reactions_scroll < 0.0f) ui->reactions_scroll = 0.0f;
    ccord_text_style uns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style uhs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    for (size_t u = 0; u < nuc; u++) {
      float uy = rx_y + (float)u * 40.0f - ui->reactions_scroll;
      if (uy + 36.0f < rx_y || uy > rx_y + rx_h) continue;
      const ccord_user *usr = &users[u];
      ui_draw_avatar(ui, r, usr, rx_x + 14.0f, uy + 18.0f, 28.0f, COL(bg_floating), false);
      const char *dname = (usr->display_name && usr->display_name[0]) ? usr->display_name
                          : ((usr->name && usr->name[0]) ? usr->name : "User");
      ccord_text_draw_ellipsis(r, dname, strlen(dname), rx_x + 36.0f, uy + 3.0f, rx_w - 40.0f, &uns);
      if (usr->handle && usr->handle[0]) {
        char hbuf[96];
        snprintf(hbuf, sizeof hbuf, "@%s", usr->handle);
        ccord_text_draw_ellipsis(r, hbuf, strlen(hbuf), rx_x + 36.0f, uy + 19.0f, rx_w - 40.0f, &uhs);
      }
    }
  }
  ccord_pop_clip(r);
  ui->reactions_thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){rx_x, rx_y, rx_w, rx_h},
                                               (float)nuc * 40.0f, ui->reactions_scroll, "reactionusers", 0);
}

static bool inbox_message_mentions_self(const ccord_ui *ui, ccord_id guild_id, const ccord_message *m)
{
  if (!ui || !m || ccord_message_is_ephemeral(m)) return false;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  if (self_id == CCORD_ID_NONE || m->author_id == self_id) return false;
  if (ccord_message_mentions_user(m, self_id)) return true;
  if (m->rich) {
    if (m->rich->mention_everyone) return true;
    if (m->rich->mention_role_count > 0 && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID) {
      const ccord_id *rids = nullptr;
      size_t rcnt = 0;
      if (ccord_store_member_roles(ui->d.store, guild_id, self_id, &rids, &rcnt)) {
        for (size_t k = 0; k < rcnt; k++) {
          if (ccord_rich_mentions_role(m->rich, rids[k])) return true;
        }
      }
    }
  }
  return false;
}

typedef struct inbox_entry {
  ccord_id guild_id;
  ccord_id channel_id;
  ccord_id message_id;
  uint32_t unread_count;
  uint32_t mention_count;
  char title[140];
  char subtitle[200];
} inbox_entry;

static size_t collect_inbox_entries(const ccord_ui *ui, int tab, inbox_entry *out, size_t cap)
{
  if (!ui || !out || cap == 0) return 0;
  size_t count = 0;
  size_t ng = ccord_store_guild_count(ui->d.store);
  for (size_t pass = 0; pass <= ng && count < cap; pass++) {
    ccord_id gid = (pass == 0) ? CCORD_DM_GUILD_ID : ccord_store_guild_at(ui->d.store, pass - 1)->id;
    const ccord_guild *g = (pass == 0) ? nullptr : ccord_store_guild_at(ui->d.store, pass - 1);
    if (pass > 0 && !g) continue;
    size_t nc = ccord_store_channel_count(ui->d.store, gid);
    for (size_t ci = 0; ci < nc && count < cap; ci++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, ci);
      if (!c || c->is_message_request || c->kind != CCORD_CHANNEL_TEXT || !ui_channel_visible(ui, gid, c)) continue;
      bool is_dm = (gid == CCORD_DM_GUILD_ID);
      const char *ch_lbl = is_dm ? ui_dm_display_name(ui, c) : (c->name ? c->name : "channel");
      if (tab == 0) {
        bool muted = ui_channel_muted(ui, c);
        if ((muted || c->unread_count == 0) && c->mention_count == 0) continue;
        inbox_entry *e = &out[count++];
        *e = (inbox_entry){
          .guild_id = gid,
          .channel_id = c->id,
          .message_id = CCORD_ID_NONE,
          .unread_count = c->unread_count,
          .mention_count = c->mention_count,
        };
        if (is_dm) {
          snprintf(e->title, sizeof e->title, "@%.120s", ch_lbl);
        } else {
          snprintf(e->title, sizeof e->title, "%.56s  #%.64s", g && g->name ? g->name : "Server", ch_lbl);
        }
        size_t mc = ccord_store_message_count(ui->d.store, c->id);
        const ccord_message *preview_m = nullptr;
        ccord_id jump_id = CCORD_ID_NONE;
        for (size_t mi = 0; mi < mc; mi++) {
          const ccord_message *m = ccord_store_message_at(ui->d.store, c->id, mi);
          if (!m || ccord_message_is_ephemeral(m)) continue;
          preview_m = m;
          if (jump_id == CCORD_ID_NONE && (c->last_read_id == CCORD_ID_NONE || m->id > c->last_read_id)) {
            jump_id = m->id;
          }
        }
        e->message_id = jump_id != CCORD_ID_NONE ? jump_id : (preview_m ? preview_m->id : CCORD_ID_NONE);
        if (preview_m) {
          const char *aname = is_dm ? ui_dm_display_name(ui, c) : ui_member_display_name(ui, gid, preview_m->author_id);
          const ccord_user *au = ccord_store_user(ui->d.store, preview_m->author_id);
          if (is_dm && au && au->name && au->name[0]) aname = au->name;
          char body[160];
          size_t bl = ccord_message_preview(preview_m, body, sizeof body);
          bl = ui_streamer_mask_text(ui, body, bl);
          snprintf(e->subtitle, sizeof e->subtitle, "%.40s: %.140s", aname ? aname : "User", bl > 0 ? body : "Message");
        } else if (c->mention_count > 0) {
          snprintf(e->subtitle, sizeof e->subtitle, "%u unread mention%s",
                   (unsigned)c->mention_count, c->mention_count == 1u ? "" : "s");
        } else {
          snprintf(e->subtitle, sizeof e->subtitle, "%u unread message%s",
                   (unsigned)c->unread_count, c->unread_count == 1u ? "" : "s");
        }
      } else {
        size_t mc = ccord_store_message_count(ui->d.store, c->id);
        size_t matched_in_ch = 0;
        for (size_t mi = mc; mi > 0 && count < cap; mi--) {
          const ccord_message *m = ccord_store_message_at(ui->d.store, c->id, mi - 1u);
          if (!inbox_message_mentions_self(ui, gid, m)) continue;
          matched_in_ch++;
          inbox_entry *e = &out[count++];
          *e = (inbox_entry){
            .guild_id = gid,
            .channel_id = c->id,
            .message_id = m->id,
            .unread_count = c->unread_count,
            .mention_count = c->mention_count,
          };
          if (is_dm) {
            snprintf(e->title, sizeof e->title, "@%.120s", ch_lbl);
          } else {
            snprintf(e->title, sizeof e->title, "%.56s  #%.64s", g && g->name ? g->name : "Server", ch_lbl);
          }
          const char *aname = is_dm ? ui_dm_display_name(ui, c) : ui_member_display_name(ui, gid, m->author_id);
          const ccord_user *au = ccord_store_user(ui->d.store, m->author_id);
          if (is_dm && au && au->name && au->name[0]) aname = au->name;
          char body[160];
          size_t bl = ccord_message_preview(m, body, sizeof body);
          bl = ui_streamer_mask_text(ui, body, bl);
          snprintf(e->subtitle, sizeof e->subtitle, "%.40s: %.140s", aname ? aname : "User", bl > 0 ? body : "Mentioned you");
        }
        if (matched_in_ch == 0 && c->mention_count > 0 && count < cap) {
          inbox_entry *e = &out[count++];
          *e = (inbox_entry){
            .guild_id = gid,
            .channel_id = c->id,
            .message_id = c->last_message_id,
            .unread_count = c->unread_count,
            .mention_count = c->mention_count,
          };
          if (is_dm) {
            snprintf(e->title, sizeof e->title, "@%.120s", ch_lbl);
          } else {
            snprintf(e->title, sizeof e->title, "%.56s  #%.64s", g && g->name ? g->name : "Server", ch_lbl);
          }
          snprintf(e->subtitle, sizeof e->subtitle, "%u unread mention%s — click to jump",
                   (unsigned)c->mention_count, c->mention_count == 1u ? "" : "s");
        }
      }
    }
  }
  return count;
}

void ui_draw_inbox_popover(ccord_ui *ui, ccord_renderer *r)
{
  ui->ninbox_hits = 0;
  ui->inbox_popover_rect = (ccord_rect){0};
  ui->inbox_mark_all_rect = (ccord_rect){0};
  ui->inbox_tab_rects[0] = (ccord_rect){0};
  ui->inbox_tab_rects[1] = (ccord_rect){0};
  ui->inbox_content_h = 0.0f;
  ui->inbox_thumb = (ccord_rect){0, 0, 0, 0};
  if (!ui->inbox_open) return;

  ccord_rect ir = ui_inbox_rect(ui);
  const ccord_layout_rect *h = &ui->layout.chat_header;
  float w = 420.0f, hgt = 420.0f;
  if (w > ui->win_w - 16.0f && ui->win_w > 32.0f) w = ui->win_w - 16.0f;
  if (hgt > ui->win_h - (h->y + h->h + 14.0f) && ui->win_h > 120.0f) hgt = ui->win_h - (h->y + h->h + 14.0f);
  if (hgt < 160.0f) hgt = 160.0f;
  float x = ir.x + ir.w - w;
  if (x < h->x + 8.0f) x = h->x + 8.0f;
  if (x + w > ui->win_w - 8.0f) x = ui->win_w - w - 8.0f;
  ccord_rect box = {x, h->y + h->h + 6.0f, w, hgt};
  ui->inbox_popover_rect = box;

  uis_popover_frame(ui, r, box);

  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_draw(r, "Inbox", 5, box.x + 14.0f, box.y + 12.0f, &hs, 0.0f);

  static const char *const k_tabs[2] = {"Unreads", "Mentions"};
  float tx = box.x + 76.0f;
  for (int t = 0; t < 2; t++) {
    ccord_rect tr = {tx, box.y + 8.0f, uis_label_width(ui, r, k_tabs[t]), UIS_H_BTN_SM};
    ui->inbox_tab_rects[t] = tr;
    bool thv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tr);
    uis_tab(ui, r, tr, k_tabs[t], (uis_state){.hover = thv, .selected = ui->inbox_tab == t}, "inboxtab", (uint64_t)t);
    tx += tr.w + UIS_SP_1;
  }

  inbox_entry entries[UI_INBOX_HIT_MAX];
  size_t nentries = collect_inbox_entries(ui, ui->inbox_tab, entries, UI_INBOX_HIT_MAX);

  bool has_ackable = false;
  for (size_t i = 0; i < nentries; i++) {
    if (entries[i].unread_count > 0 || entries[i].mention_count > 0) {
      has_ackable = true;
      break;
    }
  }
  const char *malbl = "Mark All Read";
  float maw = uis_label_width(ui, r, malbl);
  ccord_rect mar = {box.x + box.w - maw - 12.0f, box.y + 8.0f, maw, UIS_H_BTN_SM};
  ui->inbox_mark_all_rect = mar;
  bool mahv = has_ackable && ui->mouse_in && ui_in_rect(ui->mx, ui->my, mar);
  uis_button(ui, r, mar, malbl, UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = mahv, .disabled = !has_ackable},
             "inboxall", 0);

  uis_divider(ui, r, box.x, box.y + 44.0f, box.w);

  float list_x = box.x + 10.0f;
  float list_y = box.y + 52.0f;
  float list_w = box.w - 20.0f;
  float list_h = box.h - 60.0f;
  float card_h = 62.0f, gap = 6.0f;
  ui->inbox_content_h = nentries > 0 ? (float)nentries * (card_h + gap) - gap : 0.0f;
  float max_s = ui->inbox_content_h > list_h ? ui->inbox_content_h - list_h : 0.0f;
  ui->inbox_max = max_s;
  if (ui->inbox_scroll > max_s) ui->inbox_scroll = max_s;
  if (ui->inbox_scroll < 0.0f) ui->inbox_scroll = 0.0f;

  ccord_push_clip(r, (ccord_rect){list_x, list_y, list_w, list_h});
  if (nentries == 0) {
    const char *empty = (ui->inbox_tab == 0) ? "You're all caught up! No unread channels."
                                             : "No recent mentions.";
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, 0.0f);
    ccord_text_draw(r, empty, strlen(empty), list_x + (list_w - et.x) * 0.5f, list_y + (list_h - et.y) * 0.45f, &es, 0.0f);
  } else {
    ccord_text_style cts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style css = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
    for (size_t i = 0; i < nentries; i++) {
      float cy = list_y + (float)i * (card_h + gap) - ui->inbox_scroll;
      if (cy + card_h < list_y || cy > list_y + list_h) continue;
      ccord_rect card = {list_x, cy, list_w, card_h};
      ccord_rect ack_r = {0};
      bool can_ack = entries[i].unread_count > 0 || entries[i].mention_count > 0;
      if (can_ack) {
        ack_r = (ccord_rect){card.x + card.w - 84.0f, card.y + 4.0f, 76.0f, UIS_H_BTN_SM};
      }
      // Only fully visible cards are clickable; clipped ones must be scrolled into view first.
      if (cy >= list_y && cy + card_h <= list_y + list_h && ui->ninbox_hits < UI_INBOX_HIT_MAX) {
        ui->inbox_hits[ui->ninbox_hits++] = (ui_inbox_hit){
          .card_rect = card,
          .ack_rect = ack_r,
          .guild_id = entries[i].guild_id,
          .channel_id = entries[i].channel_id,
          .message_id = entries[i].message_id,
        };
      }
      bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
      bool ahv = can_ack && ui->mouse_in && ui_in_rect(ui->mx, ui->my, ack_r);
      uis_card(ui, r, card);
      uis_list_row_fill(ui, r, card, (uis_state){.hover = chv && !ahv}, "inboxcard", (uint64_t)i);

      float right_stop = can_ack ? (ack_r.x - 8.0f) : (card.x + card.w - 10.0f);
      if (entries[i].mention_count > 0) {
        char pbuf[24];
        snprintf(pbuf, sizeof pbuf, "@%u", (unsigned)entries[i].mention_count);
        ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_on_brand));
        ccord_vec2 pt = ccord_text_measure(r, pbuf, strlen(pbuf), &ps, 0.0f);
        ccord_rect pr_pill = {right_stop - (pt.x + 12.0f), card.y + 8.0f, pt.x + 12.0f, 18.0f};
        ccord_draw_rrect(r, pr_pill, 9.0f, COL(danger));
        ccord_text_draw(r, pbuf, strlen(pbuf), pr_pill.x + 6.0f, pr_pill.y + (pr_pill.h - pt.y) * 0.5f, &ps, 0.0f);
        right_stop = pr_pill.x - 6.0f;
      } else if (entries[i].unread_count > 0) {
        char pbuf[24];
        snprintf(pbuf, sizeof pbuf, "%u", (unsigned)entries[i].unread_count);
        ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f, COL(text_on_brand));
        ccord_vec2 pt = ccord_text_measure(r, pbuf, strlen(pbuf), &ps, 0.0f);
        ccord_rect pr_pill = {right_stop - maxf(pt.x + 10.0f, 18.0f), card.y + 8.0f, maxf(pt.x + 10.0f, 18.0f), 18.0f};
        ccord_draw_rrect(r, pr_pill, 9.0f, COL(brand));
        ccord_text_draw(r, pbuf, strlen(pbuf), pr_pill.x + (pr_pill.w - pt.x) * 0.5f,
                        pr_pill.y + (pr_pill.h - pt.y) * 0.5f, &ps, 0.0f);
        right_stop = pr_pill.x - 6.0f;
      }

      float avail_t = right_stop - (card.x + 10.0f);
      if (avail_t > 24.0f) {
        ccord_text_draw_ellipsis(r, entries[i].title, strlen(entries[i].title), card.x + 10.0f, card.y + 8.0f, avail_t, &cts);
      }
      ccord_text_draw_ellipsis(r, entries[i].subtitle, strlen(entries[i].subtitle),
                               card.x + 10.0f, card.y + 34.0f, card.w - 20.0f, &css);

      if (can_ack) {
        uis_button(ui, r, ack_r, "Mark Read", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){.hover = ahv}, "inboxack", (uint64_t)i);
      }
    }
  }
  ccord_pop_clip(r);
  ui->inbox_thumb = ui_draw_list_scrollbar(ui, r, (ccord_rect){list_x, list_y, list_w, list_h},
                                           ui->inbox_content_h, ui->inbox_scroll, "inboxscroll", 0);
}

void ui_draw_shortcuts_modal(ccord_ui *ui, ccord_renderer *r)
{
  ui->shortcuts_modal_rect = (ccord_rect){0};
  ui->shortcuts_close_rect = (ccord_rect){0};
  if (!ui->shortcuts_open) return;

  uis_tokens tk = uis_tokens_of(ui);
  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_LG + 20.0f, 420.0f);
  ui->shortcuts_modal_rect = box;
  uis_modal_frame(ui, r, box);

  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, "Keyboard Shortcuts", 18, box.x + 22.0f, box.y + 16.0f, &ts, 0.0f);
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  ccord_text_draw(r, "Navigate servers, channels, unreads, and chat without leaving the keyboard", 74,
                  box.x + 22.0f, box.y + 40.0f, &ss, 0.0f);

  ccord_rect close_r = {box.x + box.w - UIS_SP_1 - UIS_ICON_BTN - UIS_SP_2, box.y + 12.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  ui->shortcuts_close_rect = close_r;
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, close_r);
  ccord_color cg = uis_icon_button(ui, r, close_r, (uis_state){.hover = chv}, "scclose", 0);
  ui_icon_close(r, close_r.x + close_r.w * 0.5f, close_r.y + close_r.h * 0.5f, 13.0f, cg);

  uis_divider(ui, r, box.x, box.y + 64.0f, box.w);

  static const struct {
    const char *keys;
    const char *desc;
  } k_shortcuts[] = {
    {"Ctrl + K", "Quick Switcher"},
    {"Ctrl + /", "Keyboard Shortcuts"},
    {"Ctrl + I", "Toggle Inbox"},
    {"Ctrl + F", "Search Messages"},
    {"Ctrl + P", "Pinned Messages"},
    {"Alt + Left / Right", "Navigate Back / Forward"},
    {"Alt + Up / Down", "Previous / Next Channel"},
    {"Ctrl + Alt + Up / Down", "Previous / Next Server"},
    {"Alt + Shift + Up / Down", "Previous / Next Unread"},
    {"Ctrl + Shift + Alt + Up / Down", "Previous / Next Mention"},
    {"Up (empty composer)", "Edit Last Sent Message"},
    {"Esc", "Close Popover / Mark Read"},
    {"Ctrl + B / I / U", "Bold / Italic / Underline"},
    {"Ctrl + Shift + S / Ctrl + E", "Strikethrough / Inline Code"},
  };
  size_t total = sizeof k_shortcuts / sizeof k_shortcuts[0];
  size_t half = (total + 1u) / 2u;
  float col_w = (box.w - 52.0f) * 0.5f;
  float y0 = box.y + 78.0f;
  float row_h = 44.0f;

  ccord_text_style ks = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style ds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
  for (size_t i = 0; i < total; i++) {
    size_t col = i / half;
    size_t row = i % half;
    float rx = box.x + 20.0f + (float)col * (col_w + 12.0f);
    float ry = y0 + (float)row * row_h;
    if (ry + row_h > box.y + box.h - 8.0f) continue;
    ccord_text_draw_ellipsis(r, k_shortcuts[i].desc, strlen(k_shortcuts[i].desc), rx, ry + 2.0f, col_w, &ds);
    ccord_vec2 kt = ccord_text_measure(r, k_shortcuts[i].keys, strlen(k_shortcuts[i].keys), &ks, 0.0f);
    float kw = (kt.x + 14.0f < col_w) ? (kt.x + 14.0f) : col_w;
    ccord_rect cap_r = {rx, ry + 20.0f, kw, 20.0f};
    uis_plate(ui, r, cap_r, tk.r_control, COL(bg_tertiary));
    ccord_text_draw_ellipsis(r, k_shortcuts[i].keys, strlen(k_shortcuts[i].keys),
                             cap_r.x + 7.0f, cap_r.y + (cap_r.h - kt.y) * 0.5f, kw - 12.0f, &ks);
  }
}

// ---------------------------------------------------------------- Server Discovery view

static const char *const k_discovery_cat_names[6] = {
  "Home", "Gaming", "Music", "Education", "Science & Tech", "Entertainment",
};

void ui_draw_discovery(ccord_ui *ui, ccord_renderer *r)
{
  ui->ndiscovery_hits = 0;
  ui->discovery_search_rect = (ccord_rect){0};
  for (size_t i = 0; i < 6u; i++) ui->discovery_cat_rects[i] = (ccord_rect){0};
  ui->discovery_content_h = 0.0f;
  if (!ui->discovery_open) return;

  ccord_rect pane = {ui->layout.chat_header.x, 0.0f, ui->layout.chat_header.w, ui->win_h};
  if (pane.w <= 0.0f) pane.w = maxf(ui->win_w - pane.x, 0.0f);
  ccord_draw_rect(r, pane, COL(bg_primary));
  ccord_push_clip(r, pane);

  float pad_x = 24.0f;
  ccord_rect hero = {pane.x + pad_x, 16.0f, maxf(pane.w - 2.0f * pad_x, 220.0f), 116.0f};
  ccord_draw_rrect(r, hero, 10.0f, ui_mix(COL(bg_secondary), COL(brand), 0.16f));

  ccord_text_style hts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_header));
  ccord_text_draw_ellipsis(r, "Find your community on Discord", 30, hero.x + 20.0f, hero.y + 14.0f, hero.w - 40.0f, &hts);
  ccord_text_style hss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
  ccord_text_draw_ellipsis(r, "From gaming, to music, to learning, there's a place for you.", 60,
                           hero.x + 20.0f, hero.y + 39.0f, hero.w - 40.0f, &hss);

  ccord_rect sbox = {hero.x + 20.0f, hero.y + 68.0f, clampf(hero.w - 40.0f, 160.0f, 480.0f), 34.0f};
  ui->discovery_search_rect = sbox;
  bool sfocused = !ui->settings_open && !ui->menu.open && ui->modal_kind == UI_MODAL_NONE &&
                  !ui->quick_switcher_open && !ui->shortcuts_open;
  if (sfocused) {
    ccord_draw_rrect(r, (ccord_rect){sbox.x - 1.0f, sbox.y - 1.0f, sbox.w + 2.0f, sbox.h + 2.0f}, 6.0f, COL(brand));
  }
  ccord_draw_rrect(r, sbox, 5.0f, COL(bg_input));
  ui_icon_search(r, sbox.x + 15.0f, sbox.y + sbox.h * 0.5f, 15.0f, COL(text_muted));
  const char *q = ccord_text_edit_cstr(&ui->discovery_edit);
  bool has_q = q[0] != '\0';
  ccord_text_style qst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base),
                                  has_q ? COL(text_normal) : COL(text_placeholder));
  float qlh = ui_line_h(ui, qst.size);
  const char *qdisp = has_q ? q : "Explore communities";
  ccord_text_draw_ellipsis(r, qdisp, strlen(qdisp), sbox.x + 30.0f, sbox.y + (sbox.h - qlh) * 0.5f,
                           maxf(sbox.w - 40.0f, 20.0f), &qst);
  if (sfocused && ui->caret_on) {
    ccord_vec2 csz = ccord_text_measure(r, q, ui->discovery_edit.caret, &qst, 0.0f);
    float caretx = fminf(sbox.x + 30.0f + csz.x, sbox.x + sbox.w - 10.0f);
    ccord_draw_rect(r, (ccord_rect){caretx, sbox.y + (sbox.h - qlh) * 0.5f, 1.5f, qlh}, COL(text_normal));
  }

  float chip_y = hero.y + hero.h + 12.0f;
  float chip_x = hero.x;
  for (uint32_t c = 0; c < 6u; c++) {
    bool active = (ui->discovery_category == c);
    ccord_text_style cs = ui_style(ui, active ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                   active ? COL(text_on_brand) : COL(text_secondary));
    ccord_vec2 ct = ccord_text_measure(r, k_discovery_cat_names[c], strlen(k_discovery_cat_names[c]), &cs, 0.0f);
    ccord_rect cr = {chip_x, chip_y, ct.x + 24.0f, 28.0f};
    ui->discovery_cat_rects[c] = cr;
    bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr);
    ccord_draw_rrect(r, cr, 14.0f, active ? COL(brand) : (chv ? COL(bg_item_hover) : COL(bg_secondary)));
    ccord_text_draw(r, k_discovery_cat_names[c], strlen(k_discovery_cat_names[c]),
                    cr.x + 12.0f, cr.y + (cr.h - ct.y) * 0.5f, &cs, 0.0f);
    chip_x += cr.w + 8.0f;
  }

  size_t count = 0;
  uint32_t total = 0, offset = 0;
  bool loading = false;
  const ccord_discover_guild *items = ui->d.session
    ? ccord_session_discovery_results(ui->d.session, &count, &total, &offset, &loading)
    : nullptr;

  float hdr_y = chip_y + 38.0f;
  char hdr_buf[96];
  if (loading && count == 0) {
    snprintf(hdr_buf, sizeof hdr_buf, "Loading discoverable servers...");
  } else {
    snprintf(hdr_buf, sizeof hdr_buf, "%u Featured %s",
             (unsigned)total, total == 1u ? "Community" : "Communities");
  }
  ccord_text_style shs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_draw_ellipsis(r, hdr_buf, strlen(hdr_buf), hero.x, hdr_y, hero.w, &shs);

  float cards_y = hdr_y + 26.0f;
  float view_h = maxf(pane.h - cards_y - 12.0f, 48.0f);
  size_t cols = hero.w >= 540.0f ? 2u : 1u;
  float gap = 12.0f;
  float card_w = cols == 2u ? (hero.w - gap) * 0.5f : hero.w;
  float card_h = 122.0f;
  size_t rows = count > 0 ? (count + cols - 1u) / cols : 0u;
  ui->discovery_content_h = rows > 0 ? (float)rows * (card_h + gap) - gap : 0.0f;
  float max_s = ui->discovery_content_h > view_h ? ui->discovery_content_h - view_h : 0.0f;
  ui->discovery_max = max_s;
  if (ui->discovery_scroll > max_s) ui->discovery_scroll = max_s;
  if (ui->discovery_scroll < 0.0f) ui->discovery_scroll = 0.0f;

  ccord_rect list_clip = {hero.x, cards_y, hero.w, view_h};
  ccord_push_clip(r, list_clip);
  if (count == 0 && !loading) {
    const char *empty = "No discoverable servers matched your search.";
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, 0.0f);
    ccord_text_draw(r, empty, strlen(empty), hero.x + (hero.w - et.x) * 0.5f,
                    cards_y + (view_h - et.y) * 0.42f, &es, 0.0f);
  } else {
    ccord_text_style nst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_style dst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
    ccord_text_style mst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    for (size_t i = 0; i < count && i < CCORD_DISCOVERY_PAGE_MAX; i++) {
      const ccord_discover_guild *d = &items[i];
      size_t col = i % cols, row = i / cols;
      ccord_rect card = {
        hero.x + (float)col * (card_w + gap),
        cards_y + (float)row * (card_h + gap) - ui->discovery_scroll,
        card_w,
        card_h,
      };
      ccord_rect jbtn = {card.x + card.w - 86.0f, card.y + card.h - 36.0f, 74.0f, 26.0f};
      if (ui->ndiscovery_hits < CCORD_DISCOVERY_PAGE_MAX) {
        auto hit = &ui->discovery_hits[ui->ndiscovery_hits++];
        hit->join_rect = jbtn;
        hit->guild_id = d->guild_id;
        snprintf(hit->vanity_code, sizeof hit->vanity_code, "%s", d->vanity_code);
      }
      if (card.y + card.h < cards_y || card.y > cards_y + view_h) continue;

      bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
      ccord_draw_rrect(r, (ccord_rect){card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f}, 9.0f, COL(border_subtle));
      ccord_draw_rrect(r, card, 8.0f, chv ? COL(bg_item_hover) : COL(bg_secondary));

      ccord_rect ic = {card.x + 12.0f, card.y + 12.0f, 44.0f, 44.0f};
      if (d->icon_url[0] && ui_ensure_media(ui, r, d->icon_url)) {
        ccord_draw_texture_rounded(r, d->icon_url, ic, 12.0f, 255);
      } else {
        ccord_draw_rrect(r, ic, 12.0f, COL(brand));
        char init[8];
        ui_initials(d->name, init);
        ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_on_brand));
        ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
        ccord_text_draw(r, init, strlen(init), ic.x + (ic.w - it.x) * 0.5f, ic.y + (ic.h - it.y) * 0.5f, &ist, 0.0f);
      }

      const char *gname = d->name[0] ? d->name : "Server";
      ccord_text_draw_ellipsis(r, gname, strlen(gname), ic.x + ic.w + 10.0f, card.y + 14.0f,
                               maxf(card.w - 78.0f, 40.0f), &nst);
      if (d->description[0]) {
        ccord_push_clip(r, (ccord_rect){ic.x + ic.w + 10.0f, card.y + 36.0f, maxf(card.w - 78.0f, 40.0f), 38.0f});
        ccord_text_draw(r, d->description, strlen(d->description), ic.x + ic.w + 10.0f, card.y + 36.0f,
                        &dst, maxf(card.w - 78.0f, 40.0f));
        ccord_pop_clip(r);
      }

      float mcy = card.y + card.h - 23.0f;
      ccord_draw_circle(r, card.x + 18.0f, mcy, 4.0f, COL(status_online));
      char stats[80];
      snprintf(stats, sizeof stats, "%u Online  •  %u Members", (unsigned)d->online_count, (unsigned)d->member_count);
      ccord_text_draw_ellipsis(r, stats, strlen(stats), card.x + 28.0f, mcy - ui_line_h(ui, mst.size) * 0.5f,
                               maxf(jbtn.x - (card.x + 34.0f), 30.0f), &mst);

      bool joined = ui->d.store && ccord_store_guild(ui->d.store, d->guild_id) != nullptr;
      bool jhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, jbtn);
      ccord_color jbg = joined ? (jhv ? COL(bg_item_selected) : COL(bg_tertiary))
                               : (jhv ? ui_mix(COL(status_online), COL(brand), 0.35f) : COL(status_online));
      ccord_draw_rrect(r, jbtn, 5.0f, jbg);
      const char *jlbl = joined ? "Joined" : "Join";
      ccord_text_style jst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f,
                                      joined ? COL(text_header) : COL(text_on_brand));
      ccord_vec2 jt = ccord_text_measure(r, jlbl, strlen(jlbl), &jst, 0.0f);
      ccord_text_draw(r, jlbl, strlen(jlbl), jbtn.x + (jbtn.w - jt.x) * 0.5f, jbtn.y + (jbtn.h - jt.y) * 0.5f, &jst, 0.0f);
    }
  }
  ccord_pop_clip(r);
  ccord_pop_clip(r);
}

bool ui_discovery_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->discovery_open) return false;
  ccord_rect pane = {ui->layout.chat_header.x, 0.0f, ui->layout.chat_header.w, ui->win_h};
  if (pane.w <= 0.0f) pane.w = maxf(ui->win_w - pane.x, 0.0f);

  if (ev->type == CCORD_IN_WHEEL) {
    if (!ui_in_rect(ev->x, ev->y, pane)) return false;
    // The draw computed the bound (discovery_max) from the card area it lays out; reuse it so both clamp alike.
    ccord_scroller_scroll(&ui->discovery_glide, &ui->discovery_scroll, -ev->wheel * 48.0f, ui->discovery_max,
                          ui_wheel_direct(ui, ev));
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    if (!ui_in_rect(ev->x, ev->y, pane)) return false;
    if (ev->button != 1) return true;
    for (uint32_t c = 0; c < 6u; c++) {
      if (ui->discovery_cat_rects[c].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->discovery_cat_rects[c])) {
        if (ui->discovery_category != c) {
          ui->discovery_category = c;
          ui->discovery_scroll = 0.0f;
          ui->discovery_due_ms = 0;
          ui_discovery_request(ui, false);
          ui->dirty = true;
        }
        return true;
      }
    }
    float cards_top = 204.0f;
    for (size_t i = 0; i < ui->ndiscovery_hits; i++) {
      ccord_rect jr = ui->discovery_hits[i].join_rect;
      if (jr.w <= 0.0f || jr.y + jr.h < cards_top || jr.y > pane.h - 12.0f) continue;
      if (ui_in_rect(ev->x, ev->y, jr)) {
        ccord_id gid = ui->discovery_hits[i].guild_id;
        if (ui->d.store && ccord_store_guild(ui->d.store, gid) != nullptr) {
          ccord_error err;
          if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, gid, &err);
          else (void)ccord_store_select_guild(ui->d.store, gid, &err);
          ui->discovery_open = false;
          ui_compute_layout(ui);
          ui_ensure_caches(ui);
          ui_sync_messages(ui);
          ui_record_nav_location(ui);
          ui->dirty = true;
          return true;
        }
        if (ui->discovery_hits[i].vanity_code[0] != '\0') {
          ccord_admin_request req;
          ccord_error err;
          if (ccord_admin_accept_invite(&req, ui->discovery_hits[i].vanity_code, &err) != CCORD_OK) {
            ui_notice(ui, "Couldn't join server: %s", err.message);
            return true;
          }
          ui->discovery_pending_guild_id = gid;
          ui_admin_submit(ui, &req);
          ui->dirty = true;
        }
        return true;
      }
    }
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_MOVE || ev->type == CCORD_IN_MOUSE_UP) {
    return false;
  }

  if (ev->type == CCORD_IN_KEY_DOWN) {
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    if ((ev->mods & CCORD_MOD_ALT) != 0) return false;
    if (ctrl && (ev->key == CCORD_KEY_K || ev->key == CCORD_KEY_SLASH || ev->key == CCORD_KEY_I)) return false;

    if (ev->key == CCORD_KEY_ESCAPE) {
      if (ui->discovery_edit.len > 0) {
        ccord_text_edit_clear(&ui->discovery_edit);
        ui->discovery_scroll = 0.0f;
        ui->discovery_due_ms = 0;
        ui_discovery_request(ui, false);
      } else {
        ui->discovery_open = false;
        ui_compute_layout(ui);
      }
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      ui->discovery_due_ms = 0;
      ui->discovery_scroll = 0.0f;
      ui_discovery_request(ui, false);
      ui->dirty = true;
      return true;
    }

    size_t prev_len = ui->discovery_edit.len;
    switch (ev->key) {
      case CCORD_KEY_BACKSPACE:
        if (ctrl) ccord_text_edit_backspace_word(&ui->discovery_edit);
        else ccord_text_edit_backspace(&ui->discovery_edit);
        break;
      case CCORD_KEY_DELETE:
        if (ctrl) ccord_text_edit_delete_word(&ui->discovery_edit);
        else ccord_text_edit_delete(&ui->discovery_edit);
        break;
      case CCORD_KEY_LEFT: ccord_text_edit_move(&ui->discovery_edit, ctrl ? -3 : -1, shift); break;
      case CCORD_KEY_RIGHT: ccord_text_edit_move(&ui->discovery_edit, ctrl ? 3 : 1, shift); break;
      case CCORD_KEY_HOME: ccord_text_edit_move(&ui->discovery_edit, -2, shift); break;
      case CCORD_KEY_END: ccord_text_edit_move(&ui->discovery_edit, 2, shift); break;
      case CCORD_KEY_A: if (ctrl) ccord_text_edit_select_all(&ui->discovery_edit); break;
      case CCORD_KEY_V:
        if (ctrl && ui->d.host.clipboard_get) {
          char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
          if (clip) {
            ccord_error perr;
            (void)ccord_text_edit_insert(&ui->discovery_edit, clip, strlen(clip), &perr);
            ccord_free(clip);
          }
        }
        break;
      default:
        return false;
    }
    if (ui->discovery_edit.len != prev_len || ev->key == CCORD_KEY_V) {
      uint64_t due = ui->now_ms + UI_SEARCH_DEBOUNCE_MS;
      ui->discovery_due_ms = due != 0 ? due : 1u;
      ui->discovery_scroll = 0.0f;
    }
    ui->dirty = true;
    return true;
  }

  if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0') {
    ccord_error ierr;
    (void)ccord_text_edit_insert(&ui->discovery_edit, ev->text, strlen(ev->text), &ierr);
    uint64_t due = ui->now_ms + UI_SEARCH_DEBOUNCE_MS;
    ui->discovery_due_ms = due != 0 ? due : 1u;
    ui->discovery_scroll = 0.0f;
    ui->dirty = true;
    return true;
  }

  return false;
}

// ---------------------------------------------------------------- Go Live modal

static ccord_rect go_live_tab_rect(const ccord_rect *m, int tab)
{
  return (ccord_rect){m->x + 24.0f + (float)tab * 136.0f, m->y + 62.0f, 128.0f, 28.0f};
}

static ccord_rect go_live_source_card_rect(const ccord_rect *m, size_t idx)
{
  float grid_x = m->x + 24.0f, grid_y = m->y + 98.0f;
  float grid_w = m->w - 48.0f;
  float gap = 12.0f;
  float card_w = (grid_w - gap * 2.0f) / 3.0f;
  float card_h = 102.0f;
  size_t col = idx % 3u, row = idx / 3u;
  return (ccord_rect){grid_x + (float)col * (card_w + gap), grid_y + (float)row * (card_h + gap), card_w, card_h};
}

static void refresh_go_live_sources(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui->go_live_sources_dirty) return;
  ui->go_live_sources_dirty = false;
  ccord_capture_kind kind = (ui->go_live_tab == 1) ? CCORD_CAPTURE_SCREEN : CCORD_CAPTURE_WINDOW;
  ccord_error err;
  size_t count = ccord_capture_list(CCORD_CAPBE_AUTO, kind, ui->go_live_sources, 6, &err);
  if (count == 0) {
    count = ccord_capture_list(CCORD_CAPBE_FAKE, kind, ui->go_live_sources, 6, &err);
  }
  ui->go_live_source_count = count;
  if (ui->go_live_selected_source >= count) ui->go_live_selected_source = 0;

  if (!r || count == 0) return;
  enum { TH_W = 192, TH_H = 108 };
  uint8_t *rgba = ccord_malloc((size_t)TH_W * (size_t)TH_H * 4u);
  if (!rgba) return;
  ccord_capture_source fake_fallbacks[6];
  size_t nfake = ccord_capture_list(CCORD_CAPBE_FAKE, kind, fake_fallbacks, 6, &err);
  for (size_t i = 0; i < count; i++) {
    int tw = 0, th = 0;
    bool ok = ccord_capture_thumbnail(&ui->go_live_sources[i], TH_W, TH_H, rgba, &tw, &th, &err);
    if (!ok && nfake > 0) {
      ok = ccord_capture_thumbnail(&fake_fallbacks[i % nfake], TH_W, TH_H, rgba, &tw, &th, &err);
    }
    if (ok && tw > 0 && th > 0) {
      char key[48];
      snprintf(key, sizeof key, "__cap_thumb_%zu__", i);
      (void)ccord_render_upload_rgba(r, key, rgba, tw, th, &err);
    }
  }
  ccord_free(rgba);
}

void ui_draw_go_live(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->go_live_open) return;
  refresh_go_live_sources(ui, r);
  ccord_rect m = ui_go_live_rect(ui);
  uis_modal_frame(ui, r, m);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, "Screen Share", 12, m.x + 24.0f, m.y + 16.0f, &ts, 0.0f);
  const ccord_voice_state *v = ui_self_voice(ui);
  const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id) : nullptr;
  char sub[160];
  snprintf(sub, sizeof sub, "Select an application window or screen to share to %s", ch ? ch->name : "your voice channel");
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), m.x + 24.0f, m.y + 39.0f, m.w - 72.0f, &ss);
  {
    ccord_rect cr = ui_go_live_close_rect(ui);
    ccord_color cg = uis_icon_button(ui, r, cr, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr)}, "glclose", 0);
    ui_icon_close(r, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 14.0f, cg);
  }
  uis_tokens gtk = uis_tokens_of(ui);

  // Source Picker Tabs: Applications | Screens
  static const char *const k_tabs[2] = {"Applications", "Screens"};
  for (int t = 0; t < 2; t++) {
    ccord_rect tr = go_live_tab_rect(&m, t);
    bool active = (ui->go_live_tab == (uint8_t)t);
    uis_tab(ui, r, tr, k_tabs[t], (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, tr), .selected = active},
            "gltab", (uint64_t)t);
  }

  // Grid of live source cards with thumbnails
  for (size_t i = 0; i < ui->go_live_source_count && i < 6; i++) {
    ccord_rect card = go_live_source_card_rect(&m, i);
    bool sel = (i == ui->go_live_selected_source);
    if (sel) {
      ccord_draw_rrect(r, (ccord_rect){card.x - 2.0f, card.y - 2.0f, card.w + 4.0f, card.h + 4.0f}, 8.0f, COL(brand));
    }
    uis_plate(ui, r, card, gtk.r_control, COL(bg_tertiary));
    uis_list_row_fill(ui, r, card, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card), .selected = sel},
                      "glsrc", (uint64_t)i);
    ccord_rect thumb = {card.x + 6.0f, card.y + 6.0f, card.w - 12.0f, 68.0f};
    char key[48];
    snprintf(key, sizeof key, "__cap_thumb_%zu__", i);
    if (ccord_render_has_texture(r, key, nullptr, nullptr)) {
      ccord_draw_texture_rounded(r, key, thumb, 4.0f, 255);
    } else {
      ccord_draw_rrect(r, thumb, 4.0f, COL(bg_primary));
    }
    const char *title = ui->go_live_sources[i].title[0] ? ui->go_live_sources[i].title : "Window";
    ccord_text_style cts = ui_style(ui, sel ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small) - 2.0f,
                                    sel ? COL(text_header) : COL(text_secondary));
    ccord_text_draw_ellipsis(r, title, strlen(title), card.x + 8.0f, card.y + 78.0f, card.w - 16.0f, &cts);
  }

  // Stream Quality Box (Resolution & Frame Rate ONLY — encoder/codec/framework are in Settings)
  ccord_rect qbox = {m.x + 24.0f, m.y + 320.0f, m.w - 48.0f, 86.0f};
  uis_plate(ui, r, qbox, gtk.r_control, COL(bg_primary));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw(r, "RESOLUTION", 10, m.x + 36.0f, m.y + 338.0f, &ls, 0.0f);
  ccord_text_draw(r, "FRAME RATE", 10, m.x + 36.0f, m.y + 376.0f, &ls, 0.0f);

  for (int cat = 3; cat <= 4; cat++) {
    int count = ui_go_live_option_count(cat);
    int cur = (cat == 3) ? ui->gl_resolution : ui->gl_fps;
    for (int opt = 0; opt < count; opt++) {
      ccord_rect o = ui_go_live_option_rect(ui, cat, opt);
      bool sel = (cur == opt);
      uis_tab(ui, r, o, (cat == 3) ? resolution_label(opt) : fps_label(opt),
              (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, o), .selected = sel}, "glopt",
              (uint64_t)(cat * 16 + opt));
    }
  }

  // Share System Audio: the broadcast carries the desktop/monitor loopback mix instead of the
  // mic downmix (app_media reads the toggle through ccord_ui_go_live_share_audio).
  ccord_rect ar = ui_go_live_audio_rect(ui);
  ccord_rect cb = {ar.x, ar.y + (ar.h - 18.0f) * 0.5f, 18.0f, 18.0f};
  bool ahov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ar);
  ccord_draw_rrect(r, cb, 4.0f, ui->go_live_share_audio ? COL(brand) : COL(bg_tertiary));
  if (ui->go_live_share_audio) {
    ccord_draw_line(r, (ccord_vec2){cb.x + 4.0f, cb.y + 9.5f}, (ccord_vec2){cb.x + 7.5f, cb.y + 13.0f}, 2.0f,
                    COL(text_on_brand));
    ccord_draw_line(r, (ccord_vec2){cb.x + 7.5f, cb.y + 13.0f}, (ccord_vec2){cb.x + 14.0f, cb.y + 5.5f}, 2.0f,
                    COL(text_on_brand));
  }
  ccord_text_style als = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small),
                                  ahov ? COL(text_header) : COL(text_secondary));
  ccord_vec2 alt = ccord_text_measure(r, "Share System Audio", 18, &als, 0.0f);
  ccord_text_draw(r, "Share System Audio", 18, cb.x + cb.w + 10.0f,
                  ar.y + (ar.h - alt.y) * 0.5f, &als, 0.0f);
  ccord_text_style ahs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  float hint_x = cb.x + cb.w + 10.0f + alt.x + 10.0f;
  const char *hint = "desktop / monitor loopback";
  ccord_text_draw_ellipsis(r, hint, strlen(hint), hint_x,
                           ar.y + (ar.h - ui_line_h(ui, ahs.size)) * 0.5f, ar.x + ar.w - hint_x, &ahs);

  // Footer: selected source summary + Go Live / Stop Stream button
  ccord_rect start = ui_go_live_start_rect(ui);
  if (ui->go_live_selected_source < ui->go_live_source_count) {
    char sel_lbl[192];
    snprintf(sel_lbl, sizeof sel_lbl, "Selected: %s", ui->go_live_sources[ui->go_live_selected_source].title);
    ccord_text_draw_ellipsis(r, sel_lbl, strlen(sel_lbl), m.x + 24.0f, start.y + 7.0f, start.x - (m.x + 36.0f), &ss);
  }
  bool live = ccord_store_stream_active(ui->d.store);
  uis_button(ui, r, start, live ? "Stop Stream" : "Go Live", live ? UIS_BTN_DANGER : UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, start)}, "glstart", 0);
}

bool ui_go_live_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui->go_live_open) return false;
  if (ev->type != CCORD_IN_MOUSE_DOWN || ev->button != 1) return true;
  ccord_rect m = ui_go_live_rect(ui);
  if (ui_in_rect(ev->x, ev->y, ui_go_live_close_rect(ui)) || !ui_in_rect(ev->x, ev->y, m)) {
    ui->go_live_open = false;
    ui->dirty = true;
    return true;
  }
  for (int t = 0; t < 2; t++) {
    if (ui_in_rect(ev->x, ev->y, go_live_tab_rect(&m, t))) {
      if (ui->go_live_tab != (uint8_t)t) {
        ui->go_live_tab = (uint8_t)t;
        ui->go_live_selected_source = 0;
        ui->go_live_sources_dirty = true;
        ui->dirty = true;
      }
      return true;
    }
  }
  for (size_t i = 0; i < ui->go_live_source_count && i < 6; i++) {
    if (ui_in_rect(ev->x, ev->y, go_live_source_card_rect(&m, i))) {
      ui->go_live_selected_source = i;
      ui->dirty = true;
      return true;
    }
  }
  for (int cat = 0; cat < 5; cat++) {
    int count = ui_go_live_option_count(cat);
    for (int opt = 0; opt < count; opt++) {
      if (!ui_in_rect(ev->x, ev->y, ui_go_live_option_rect(ui, cat, opt))) continue;
      int *slot = cat == 0 ? &ui->gl_encoder : cat == 1 ? &ui->gl_codec : cat == 2 ? &ui->gl_framework
                 : cat == 3 ? &ui->gl_resolution : &ui->gl_fps;
      *slot = opt;
      ui->dirty = true;
      return true;
    }
  }
  if (ui_in_rect(ev->x, ev->y, ui_go_live_audio_rect(ui))) {
    ui->go_live_share_audio = !ui->go_live_share_audio;
    ui->dirty = true;
    return true;
  }
  if (ui_in_rect(ev->x, ev->y, ui_go_live_start_rect(ui))) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return true;
    }
    const ccord_voice_state *v = ui_self_voice(ui);
    const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id) : nullptr;
    if (!ch) {
      ui_notice(ui, "Join a voice channel first");
      return true;
    }
    ccord_error err;
    if (ccord_store_stream_active(ui->d.store)) {
      ui->has_active_capture_source = false;
      if (ccord_session_stop_stream(ui->d.session, ch->guild_id, &err) != CCORD_OK) {
        ui_notice(ui, "Cannot stop stream: %s", err.message);
        return true;
      }
    } else {
      if (ui->go_live_selected_source < ui->go_live_source_count) {
        ui->active_capture_source = ui->go_live_sources[ui->go_live_selected_source];
        ui->has_active_capture_source = true;
      }
      ui->stream_hide_preview = false;
      ccord_stream_config cfg = {
        .encoder = (ccord_stream_encoder)ui->gl_encoder,
        .codec = (ccord_stream_codec)ui->gl_codec,
        .framework = (ccord_stream_framework)ui->gl_framework,
        .resolution = (ccord_stream_resolution)ui->gl_resolution,
        .fps = (ccord_stream_fps)ui->gl_fps,
      };
      if (ccord_session_start_stream(ui->d.session, ch->guild_id, ch->id, &cfg, &err) != CCORD_OK) {
        ui_notice(ui, "Cannot go live: %s", err.message);
        return true;
      }
      ui->stage_channel = ch->id;
      ui->stage_armed = true;
    }
    ui->go_live_open = false;
    ui->dirty = true;
  }
  return true;
}

// ---------------------------------------------------------------- emoji picker & autocomplete

static const char *const k_picker_emojis[UI_EMOJI_PICKER_COUNT] = {
  "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\xAE",
  "\xF0\x9F\x98\xA2", "\xF0\x9F\x94\xA5",         "\xF0\x9F\x8E\x89", "\xF0\x9F\x91\x80",
};

static const char *const k_emoji_shortcodes[UI_EMOJI_PICKER_COUNT] = {
  ":thumbsup:", ":heart:", ":joy:", ":open_mouth:",
  ":sob:",      ":fire:",  ":tada:", ":eyes:",
};

void ui_draw_emoji_picker(ccord_ui *ui, ccord_renderer *r)
{
  ui_expr_picker_draw(ui, r);
}

static bool ascii_prefix_ci(const char *str, const char *prefix, size_t plen)
{
  if (!str) return false;
  for (size_t i = 0; i < plen; i++) {
    char a = str[i], b = prefix[i];
    if (a == '\0') return false;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

static uint64_t slash_fnv1a_step(uint64_t h, const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void format_slash_choice_insert(const char *opt_name, bool named_already, const char *val,
                                       char out[static 96])
{
  bool need_quote = false;
  for (const char *p = val; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '"') {
      need_quote = true;
      break;
    }
  }
  if (named_already) {
    if (need_quote) snprintf(out, 96, "\"%.88s\" ", val);
    else snprintf(out, 96, "%.92s ", val);
  } else {
    if (need_quote) snprintf(out, 96, "%.32s:\"%.54s\" ", opt_name, val);
    else snprintf(out, 96, "%.32s:%.56s ", opt_name, val);
  }
}

static void ui_schedule_slash_autocomplete(ccord_ui *ui, const ccord_channel *ch, const ccord_slash_command *cmd,
                                           const slash_parsed *sp, size_t opt_idx)
{
  if (!ui || !ui->d.session || !ch || !cmd || opt_idx >= cmd->option_count) return;
  const char *focused_val = sp->opt_vals[opt_idx];
  uint64_t key = 14695981039346656037ULL;
  key = slash_fnv1a_step(key, &ch->id, sizeof ch->id);
  key = slash_fnv1a_step(key, &cmd->id, sizeof cmd->id);
  key = slash_fnv1a_step(key, &opt_idx, sizeof opt_idx);
  key = slash_fnv1a_step(key, focused_val, strlen(focused_val));

  if (ui->slash_auto_cmd_id != cmd->id || ui->slash_auto_opt_idx != opt_idx) {
    ccord_session_clear_slash_choices(ui->d.session);
  }
  if (key == ui->slash_auto_sent_key) return;

  ui->slash_auto_sent_key = key;
  ui->slash_auto_cmd_id = cmd->id;
  ui->slash_auto_opt_idx = opt_idx;
  snprintf(ui->slash_auto_cmd_name, sizeof ui->slash_auto_cmd_name, "%s", cmd->name);
  ui->slash_auto_sub_group[0] = '\0';
  ui->slash_auto_sub_cmd[0] = '\0';
  if (sp->sub_group_idx >= 0) {
    snprintf(ui->slash_auto_sub_group, sizeof ui->slash_auto_sub_group, "%s", cmd->options[sp->sub_group_idx].name);
  }
  if (sp->sub_cmd_idx >= 0) {
    snprintf(ui->slash_auto_sub_cmd, sizeof ui->slash_auto_sub_cmd, "%s", cmd->options[sp->sub_cmd_idx].name);
  }

  size_t nopts = 0;
  int32_t focused_out = 0;
  for (size_t i = 0; i < cmd->option_count && nopts < CCORD_SLASH_MAX_OPTIONS; i++) {
    if (!slash_opt_in_scope(cmd, i, sp)) continue;
    if (i != opt_idx && !sp->opt_set[i]) continue;
    snprintf(ui->slash_auto_opt_names[nopts], sizeof ui->slash_auto_opt_names[nopts], "%s", cmd->options[i].name);
    ui->slash_auto_opt_kinds[nopts] = (uint8_t)cmd->options[i].kind;
    snprintf(ui->slash_auto_opt_vals[nopts], sizeof ui->slash_auto_opt_vals[nopts], "%s", sp->opt_vals[i]);
    ui->slash_auto_opt_name_ptrs[nopts] = ui->slash_auto_opt_names[nopts];
    ui->slash_auto_opt_val_ptrs[nopts] = ui->slash_auto_opt_vals[nopts];
    if (i == opt_idx) focused_out = (int32_t)nopts;
    nopts++;
  }
  ui->slash_auto_req = (ccord_interaction_request){
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = cmd->application_id,
    .guild_id = ch->guild_id == CCORD_DM_GUILD_ID ? CCORD_ID_NONE : ch->guild_id,
    .channel_id = ch->id,
    .command_id = cmd->id,
    .command_version = cmd->version != CCORD_ID_NONE ? cmd->version : cmd->id,
    .command_name = ui->slash_auto_cmd_name,
    .option_names = ui->slash_auto_opt_name_ptrs,
    .option_kinds = ui->slash_auto_opt_kinds,
    .option_values = ui->slash_auto_opt_val_ptrs,
    .option_count = nopts,
    .focused_option = focused_out,
    .command_type = 1u,
    .subcommand_group = ui->slash_auto_sub_group[0] != '\0' ? ui->slash_auto_sub_group : nullptr,
    .subcommand = ui->slash_auto_sub_cmd[0] != '\0' ? ui->slash_auto_sub_cmd : nullptr,
  };
  uint64_t due = ui->now_ms + UI_SLASH_AUTO_DEBOUNCE_MS;
  ui->slash_auto_due_ms = due != 0 ? due : 1u;
}

static bool substr_ci(const char *hay, const char *needle, size_t nlen);

static bool id_in_list(const ccord_id *ids, size_t n, ccord_id id)
{
  for (size_t i = 0; i < n; i++) {
    if (ids[i] == id) return true;
  }
  return false;
}

static bool ac_text_match(const char *s, const char *q, size_t qlen, bool prefix_only)
{
  if (!s || !s[0]) return false;
  if (qlen == 0) return true;
  return prefix_only ? ascii_prefix_ci(s, q, qlen) : substr_ci(s, q, qlen);
}

static void add_user_autocomplete(ccord_ui *ui, const ccord_member *m, const ccord_user *u,
                                  ccord_id seen_uids[static UI_AUTOCOMPLETE_MAX], size_t *nseen,
                                  const char *query, size_t qlen, bool prefix_only)
{
  if (!ui || !u || ui->nautocomplete >= UI_AUTOCOMPLETE_MAX) return;
  const char *ins_name = (u->name && u->name[0]) ? u->name : ((u->handle && u->handle[0]) ? u->handle : nullptr);
  if (!ins_name) return;
  if (id_in_list(seen_uids, *nseen, u->id)) return;
  bool match = (qlen == 0) || ac_text_match(u->name, query, qlen, prefix_only) ||
               ac_text_match(u->handle, query, qlen, prefix_only) ||
               (m && ac_text_match(m->nick, query, qlen, prefix_only));
  if (!match) return;
  seen_uids[(*nseen)++] = u->id;
  ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
  if (m && m->nick[0]) {
    snprintf(it->display, sizeof it->display, "@%.40s (%.40s)", m->nick, ins_name);
  } else if (u->name && u->name[0] && u->handle && u->handle[0] && strcasecmp(u->name, u->handle) != 0) {
    snprintf(it->display, sizeof it->display, "@%.40s (@%.38s)", u->name, u->handle);
  } else {
    snprintf(it->display, sizeof it->display, "@%.80s", ins_name);
  }
  snprintf(it->insert, sizeof it->insert, "@%s ", ins_name);
}

static void collect_at_autocomplete(ccord_ui *ui, const ccord_channel *ch, ccord_id gid,
                                    const char *query, size_t qlen)
{
  ccord_id seen_uids[UI_AUTOCOMPLETE_MAX] = {0};
  size_t nseen_u = 0;
  ccord_id seen_rids[UI_AUTOCOMPLETE_MAX] = {0};
  size_t nseen_r = 0;
  bool added_everyone = false, added_here = false;
  bool in_guild = gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID;
  bool can_everyone = in_guild && can_mention_everyone_in(ui->d.store, gid, ch->id, ui->now_unix_ms);
  size_t passes = qlen > 0 ? 2u : 1u;

  for (size_t pass = 0; pass < passes && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; pass++) {
    bool prefix_only = (pass == 0);
    if (in_guild) {
      size_t mc = ccord_store_member_count(ui->d.store, gid);
      for (size_t i = 0; i < mc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        const ccord_member *m = ccord_store_member_at(ui->d.store, gid, i);
        const ccord_user *u = m ? ccord_store_user(ui->d.store, m->user_id) : nullptr;
        add_user_autocomplete(ui, m, u, seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      size_t msg_c = ccord_store_message_count(ui->d.store, ch->id);
      for (size_t i = msg_c; i > 0 && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i--) {
        const ccord_message *msg = ccord_store_message_at(ui->d.store, ch->id, i - 1u);
        if (!msg || msg->author_id == CCORD_ID_NONE) continue;
        const ccord_member *m = ccord_store_find_member(ui->d.store, gid, msg->author_id);
        const ccord_user *u = ccord_store_user(ui->d.store, msg->author_id);
        add_user_autocomplete(ui, m, u, seen_uids, &nseen_u, query, qlen, prefix_only);
      }
    } else if (gid == CCORD_DM_GUILD_ID) {
      if (ch->user_id != CCORD_ID_NONE) {
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, ch->user_id),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      for (uint16_t r = 0; r < ch->recipient_count && r < CCORD_MAX_DM_RECIPIENTS &&
                           ui->nautocomplete < UI_AUTOCOMPLETE_MAX; r++) {
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, ch->recipient_ids[r]),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      size_t msg_c = ccord_store_message_count(ui->d.store, ch->id);
      for (size_t i = msg_c; i > 0 && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i--) {
        const ccord_message *msg = ccord_store_message_at(ui->d.store, ch->id, i - 1u);
        if (!msg || msg->author_id == CCORD_ID_NONE) continue;
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, msg->author_id),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
      ccord_id self_id = ccord_store_self_id(ui->d.store);
      if (self_id != CCORD_ID_NONE) {
        add_user_autocomplete(ui, nullptr, ccord_store_user(ui->d.store, self_id),
                              seen_uids, &nseen_u, query, qlen, prefix_only);
      }
    }
    if (in_guild) {
      size_t rc = 0;
      const ccord_role *roles = ccord_store_roles(ui->d.store, gid, &rc);
      for (size_t i = 0; i < rc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        const ccord_role *rl = &roles[i];
        if (rl->id == gid || !rl->name || !rl->name[0]) continue;
        if (!rl->mentionable && !can_everyone) continue;
        if (id_in_list(seen_rids, nseen_r, rl->id)) continue;
        if (!ac_text_match(rl->name, query, qlen, prefix_only)) continue;
        seen_rids[nseen_r++] = rl->id;
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        snprintf(it->display, sizeof it->display, "@%.72s (role)", rl->name);
        snprintf(it->insert, sizeof it->insert, "@%s ", rl->name);
      }
      if (can_everyone) {
        if (!added_everyone && ui->nautocomplete < UI_AUTOCOMPLETE_MAX &&
            ac_text_match("everyone", query, qlen, prefix_only)) {
          added_everyone = true;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, "@everyone");
          snprintf(it->insert, sizeof it->insert, "@everyone ");
        }
        if (!added_here && ui->nautocomplete < UI_AUTOCOMPLETE_MAX &&
            ac_text_match("here", query, qlen, prefix_only)) {
          added_here = true;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, "@here");
          snprintf(it->insert, sizeof it->insert, "@here ");
        }
      }
    }
  }
}

void ui_update_autocomplete(ccord_ui *ui)
{
  ui->autocomplete_open = false;
  ui->nautocomplete = 0;
  if (!ui || !ui->d.store || ui->settings_open || ui->menu.open || ui->edit.len == 0 || ui->edit.caret == 0) {
    if (ui) ui->slash_auto_due_ms = 0;
    return;
  }

  const char *text = ccord_text_edit_cstr(&ui->edit);
  size_t caret = ui->edit.caret;
  if (caret > ui->edit.len) caret = ui->edit.len;
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) {
    ui->slash_auto_due_ms = 0;
    return;
  }
  ccord_id gid = ch->guild_id != CCORD_ID_NONE ? ch->guild_id : ccord_store_selected_guild(ui->d.store);

  // Slash command & option picker when composer starts with '/'
  if (text[0] == '/' && ui->editing_message == CCORD_ID_NONE && ui->staged_file_count == 0 &&
      ui->reply_message == CCORD_ID_NONE && ch && ch->kind == CCORD_CHANNEL_TEXT &&
      memchr(text, '\n', caret) == nullptr) {
    if (ui->d.session) (void)ccord_session_ensure_slash_commands(ui->d.session, ch->guild_id, ch->id, ui->now_ms);
    ui->built_slash_store_rev = ui->d.store ? ccord_store_revision(ui->d.store) : 0;
    size_t nchoices_init = 0;
    uint64_t seq_init = 0;
    if (ui->d.session) (void)ccord_session_slash_choices(ui->d.session, &nchoices_init, &seq_init);
    ui->built_slash_choice_seq = seq_init;

    size_t sp_pos = 1u;
    while (sp_pos < caret && text[sp_pos] != ' ' && text[sp_pos] != '\t') sp_pos++;
    if (sp_pos == caret) {
      ui->slash_auto_due_ms = 0;
      const char *q = text + 1u;
      size_t qlen = caret - 1u;
      size_t ncmds = 0;
      const ccord_slash_command *cmds = ui->d.store ? ccord_store_slash_commands(ui->d.store, ch->guild_id, &ncmds) : nullptr;
      for (size_t i = 0; i < ncmds && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        if (!slash_cmd_allowed(ui, ch, &cmds[i])) continue;
        if (qlen > 0 && !ascii_prefix_ci(cmds[i].name, q, qlen)) continue;
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        if (cmds[i].description[0] != '\0') {
          snprintf(it->display, sizeof it->display, "/%.32s \xE2\x80\x94 %.54s", cmds[i].name, cmds[i].description);
        } else {
          snprintf(it->display, sizeof it->display, "/%.32s", cmds[i].name);
        }
        snprintf(it->insert, sizeof it->insert, "/%.88s ", cmds[i].name);
      }
      // /tts is a built-in composer command (not an application command): always offered.
      if ((qlen == 0 || ascii_prefix_ci("tts", q, qlen)) && ui->nautocomplete < UI_AUTOCOMPLETE_MAX) {
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        snprintf(it->display, sizeof it->display, "/tts \xE2\x80\x94 Send a text-to-speech message");
        snprintf(it->insert, sizeof it->insert, "/tts ");
      }
      if (ui->nautocomplete > 0) {
        ui->autocomplete_open = true;
        ui->autocomplete_trigger = '/';
        ui->autocomplete_start = 0;
        if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
      }
      return;
    }

    const ccord_slash_command *cmd = find_slash_cmd(ui, ch->guild_id, text + 1u, sp_pos - 1u);
    if (cmd && slash_cmd_allowed(ui, ch, cmd)) {
      slash_parsed sp;
      slash_parse_args(cmd, text, caret, &sp);
      size_t rep_start = sp.token_start;
      bool named_already = !sp.completing_opt_name;

      // 0. Subcommand tree: offer the group / subcommand names while one is being picked
      if (sp.selecting_sub) {
        size_t plen = strlen(sp.cur_prefix);
        for (size_t i = 0; i < cmd->option_count && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          const ccord_slash_option *o = &cmd->options[i];
          bool want;
          if (sp.sub_group_idx < 0) {
            want = o->parent_plus_one == 0 &&
                   (o->kind == CCORD_SLASH_OPT_SUB_COMMAND || o->kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP);
          } else {
            want = o->parent_plus_one == (uint8_t)(sp.sub_group_idx + 1) && o->kind == CCORD_SLASH_OPT_SUB_COMMAND;
          }
          if (!want) continue;
          if (plen > 0 && !ascii_prefix_ci(o->name, sp.cur_prefix, plen)) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          if (o->description[0] != '\0') {
            snprintf(it->display, sizeof it->display, "%.32s \xE2\x80\x94 %.54s", o->name, o->description);
          } else {
            snprintf(it->display, sizeof it->display, "%.32s", o->name);
          }
          snprintf(it->insert, sizeof it->insert, "%.88s ", o->name);
        }
      }

      // 1. Offer matching option names when typing an unquoted token or after space
      if (sp.completing_opt_name && !sp.selecting_sub) {
        size_t plen = strlen(sp.cur_prefix);
        for (size_t i = 0; i < cmd->option_count && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          if (!slash_opt_in_scope(cmd, i, &sp)) continue;
          if (sp.opt_set[i] && (int)i != sp.focused_opt) continue;
          if (plen > 0 && !ascii_prefix_ci(cmd->options[i].name, sp.cur_prefix, plen)) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          if (cmd->options[i].description[0] != '\0') {
            snprintf(it->display, sizeof it->display, "%.32s:%s \xE2\x80\x94 %.52s",
                     cmd->options[i].name, cmd->options[i].required ? "*" : "", cmd->options[i].description);
          } else {
            snprintf(it->display, sizeof it->display, "%.32s:%s",
                     cmd->options[i].name, cmd->options[i].required ? "*" : "");
          }
          snprintf(it->insert, sizeof it->insert, "%.88s:", cmd->options[i].name);
        }
      }

      // 2. Offer option values / dynamic autocomplete for the focused option
      if (sp.focused_opt >= 0 && (size_t)sp.focused_opt < cmd->option_count) {
        size_t oidx = (size_t)sp.focused_opt;
        const ccord_slash_option *opt = &cmd->options[oidx];
        const char *vq = sp.opt_vals[oidx];
        size_t vqlen = strlen(vq);
        if (opt->autocomplete) {
          ui_schedule_slash_autocomplete(ui, ch, cmd, &sp, oidx);
          size_t nch = 0;
          uint64_t cseq = 0;
          const ccord_slash_choice *choices = ui->d.session ? ccord_session_slash_choices(ui->d.session, &nch, &cseq) : nullptr;
          ui->built_slash_choice_seq = cseq;
          if (choices && ui->slash_auto_cmd_id == cmd->id && ui->slash_auto_opt_idx == oidx) {
            for (size_t c = 0; c < nch && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; c++) {
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "%.58s (%.32s)", choices[c].name, opt->name);
              format_slash_choice_insert(opt->name, named_already, choices[c].value, it->insert);
            }
          }
        } else {
          ui->slash_auto_due_ms = 0;
          if (opt->kind == CCORD_SLASH_OPT_BOOLEAN) {
            const char *const bvals[2] = {"true", "false"};
            for (size_t b = 0; b < 2u && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; b++) {
              if (vqlen > 0 && !ascii_prefix_ci(bvals[b], vq, vqlen)) continue;
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "%s (%.32s)", bvals[b], opt->name);
              format_slash_choice_insert(opt->name, named_already, bvals[b], it->insert);
            }
          } else if (opt->kind == CCORD_SLASH_OPT_CHANNEL && gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID) {
            const char *cq = (vq[0] == '#') ? vq + 1u : vq;
            size_t cqlen = strlen(cq);
            size_t cc = ccord_store_channel_count(ui->d.store, gid);
            for (size_t c = 0; c < cc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; c++) {
              const ccord_channel * cand = ccord_store_channel_at(ui->d.store, gid, c);
              if (!cand || cand->kind == CCORD_CHANNEL_CATEGORY || !cand->name || !ui_channel_visible(ui, gid, cand)) continue;
              if (cqlen > 0 && !ascii_prefix_ci(cand->name, cq, cqlen)) continue;
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "#%.54s (%.32s)", cand->name, opt->name);
              char hname[96];
              snprintf(hname, sizeof hname, "#%.90s", cand->name);
              format_slash_choice_insert(opt->name, named_already, hname, it->insert);
            }
          } else if (opt->kind == CCORD_SLASH_OPT_ROLE && gid != CCORD_ID_NONE && gid != CCORD_DM_GUILD_ID) {
            const char *rq = (vq[0] == '@') ? vq + 1u : vq;
            size_t rqlen = strlen(rq);
            size_t rc = 0;
            const ccord_role *roles = ccord_store_roles(ui->d.store, gid, &rc);
            for (size_t r = 0; r < rc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; r++) {
              if (roles[r].id == gid || !roles[r].name || !roles[r].name[0]) continue;
              if (rqlen > 0 && !ascii_prefix_ci(roles[r].name, rq, rqlen)) continue;
              ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
              snprintf(it->display, sizeof it->display, "@%.54s (%.32s)", roles[r].name, opt->name);
              char rname[96];
              snprintf(rname, sizeof rname, "@%.90s", roles[r].name);
              format_slash_choice_insert(opt->name, named_already, rname, it->insert);
            }
          }
        }
      } else {
        ui->slash_auto_due_ms = 0;
      }

      if (ui->nautocomplete > 0) {
        ui->autocomplete_open = true;
        ui->autocomplete_trigger = '/';
        ui->autocomplete_start = rep_start;
        if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
      }
      return;
    }
    ui->slash_auto_due_ms = 0;
    return;
  }
  ui->slash_auto_due_ms = 0;

  size_t start = caret;
  while (start > 0) {
    char c = text[start - 1];
    if (c == ' ' || c == '\n' || c == '\t') break;
    start--;
    if (c == '@' || c == '#' || c == ':') break;
  }
  char trig = text[start];
  if (trig != '@' && trig != '#' && trig != ':') return;
  if (start > 0) {
    if (trig == '@') {
      if (!has_at_mention_boundary_before(text, start)) return;
    } else if (text[start - 1] != ' ' && text[start - 1] != '\n' && text[start - 1] != '\t') {
      return;
    }
  }

  const char *query = text + start + 1;
  size_t qlen = caret - (start + 1);
  if (qlen > 32) return;
  for (size_t i = 0; i < qlen; i++) {
    if (query[i] == ' ' || query[i] == '\n') return;
  }

  if (trig == '@') {
    collect_at_autocomplete(ui, ch, gid, query, qlen);
  } else if (trig == '#') {
    size_t cc = ccord_store_channel_count(ui->d.store, gid);
    for (size_t i = 0; i < cc && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, gid, i);
      if (!c || c->kind != CCORD_CHANNEL_TEXT || !c->name || !ui_channel_visible(ui, gid, c)) continue;
      if (qlen > 0 && !ascii_prefix_ci(c->name, query, qlen)) continue;
      ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
      snprintf(it->display, sizeof it->display, "#%s", c->name);
      snprintf(it->insert, sizeof it->insert, "#%s ", c->name);
    }
  } else if (trig == ':') {
    if (qlen == 0) return;
    if (qlen == 4 && strncasecmp(query, "fire", 4) == 0) {
      ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
      snprintf(it->display, sizeof it->display, "\xF0\x9F\x94\xA5 :fire:");
      snprintf(it->insert, sizeof it->insert, "\xF0\x9F\x94\xA5 ");
    } else {
      // 1. Guild custom emojis (selected guild first, then all guilds)
      if (ui->d.store) {
        size_t ec = ccord_store_guild_emoji_count(ui->d.store, gid);
        for (size_t i = 0; i < ec && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, gid, i);
          if (!ce || !ce->name || !ce->name[0] || !ascii_prefix_ci(ce->name, query, qlen)) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, ":%s:", ce->name);
          snprintf(it->insert, sizeof it->insert, "<%s:%s:%llu> ",
                   ce->animated ? "a" : "", ce->name, (unsigned long long)ce->id);
        }
        size_t ng = ccord_store_guild_count(ui->d.store);
        for (size_t g = 0; g < ng && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; g++) {
          const ccord_guild *gd = ccord_store_guild_at(ui->d.store, g);
          if (!gd || gd->id == gid) continue;
          size_t gec = ccord_store_guild_emoji_count(ui->d.store, gd->id);
          for (size_t i = 0; i < gec && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
            const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, gd->id, i);
            if (!ce || !ce->name || !ce->name[0] || !ascii_prefix_ci(ce->name, query, qlen)) continue;
            ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
            snprintf(it->display, sizeof it->display, ":%s: (%s)", ce->name, gd->name ? gd->name : "");
            snprintf(it->insert, sizeof it->insert, "<%s:%s:%llu> ",
                     ce->animated ? "a" : "", ce->name, (unsigned long long)ce->id);
          }
        }
      }
      // 2. Common shortcodes + full Unicode emoji dataset
      for (size_t i = 0; i < UI_EMOJI_PICKER_COUNT && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
        const char *sc = k_emoji_shortcodes[i] + 1;
        if (!ascii_prefix_ci(sc, query, qlen)) continue;
        ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
        snprintf(it->display, sizeof it->display, "%s %s", k_picker_emojis[i], k_emoji_shortcodes[i]);
        snprintf(it->insert, sizeof it->insert, "%s ", k_picker_emojis[i]);
      }
      if (ui->nautocomplete < UI_AUTOCOMPLETE_MAX) {
        size_t match_idx[UI_AUTOCOMPLETE_MAX];
        size_t nm = ccord_emoji_complete(query, qlen, match_idx, UI_AUTOCOMPLETE_MAX);
        if (nm == 0) nm = ccord_emoji_search(query, qlen, match_idx, UI_AUTOCOMPLETE_MAX);
        for (size_t i = 0; i < nm && ui->nautocomplete < UI_AUTOCOMPLETE_MAX; i++) {
          const ccord_emoji_entry *e = ccord_emoji_at(match_idx[i]);
          if (!e || !e->utf8 || !e->name) continue;
          bool dup = false;
          for (size_t k = 0; k < ui->nautocomplete; k++) {
            if (strncmp(ui->autocomplete_items[k].insert, e->utf8, strlen(e->utf8)) == 0) {
              dup = true;
              break;
            }
          }
          if (dup) continue;
          ui_autocomplete_item *it = &ui->autocomplete_items[ui->nautocomplete++];
          snprintf(it->display, sizeof it->display, "%s :%s:", e->utf8, e->name);
          snprintf(it->insert, sizeof it->insert, "%s ", e->utf8);
        }
      }
    }
  }

  if (ui->nautocomplete > 0) {
    ui->autocomplete_open = true;
    ui->autocomplete_trigger = trig;
    ui->autocomplete_start = start;
    if (ui->autocomplete_sel >= ui->nautocomplete) ui->autocomplete_sel = 0;
  }
}

bool ui_accept_autocomplete(ccord_ui *ui, size_t index)
{
  if (!ui || !ui->autocomplete_open || index >= ui->nautocomplete) return false;
  char trig = ui->autocomplete_trigger;
  const char *ins = ui->autocomplete_items[index].insert;
  size_t ilen = strlen(ins);
  size_t caret = ui->edit.caret;
  if (caret > ui->edit.len) caret = ui->edit.len;
  size_t start = ui->autocomplete_start;
  if (start > caret) start = caret;

  char buf[CCORD_SEND_MAX_BYTES + 1];
  size_t tail_len = ui->edit.len - caret;
  if (start + ilen + tail_len >= sizeof buf) return false;
  memcpy(buf, ui->edit.data, start);
  memcpy(buf + start, ins, ilen);
  memcpy(buf + start + ilen, ui->edit.data + caret, tail_len);
  buf[start + ilen + tail_len] = '\0';
  ccord_error ierr;
  ccord_text_edit_clear(&ui->edit);
  (void)ccord_text_edit_insert(&ui->edit, buf, start + ilen + tail_len, &ierr);
  ccord_text_edit_set_caret(&ui->edit, start + ilen, false);
  ui->autocomplete_open = false;
  ui->nautocomplete = 0;
  if (trig == '/') ui_update_autocomplete(ui);
  ui->dirty = true;
  return true;
}

void ui_draw_autocomplete(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->autocomplete_open || ui->nautocomplete == 0) {
    ui->autocomplete_rect = (ccord_rect){0, 0, 0, 0};
    return;
  }
  ccord_rect cbox = ui_composer_box(ui);
  float row_h = 26.0f;
  float h = (float)ui->nautocomplete * row_h + 12.0f;
  float max_w = ui->autocomplete_trigger == '/' ? 380.0f : 280.0f;
  float w = clampf(cbox.w, 180.0f, max_w);
  ccord_rect pop = {cbox.x, fmaxf(cbox.y - h - 6.0f, 6.0f), w, h};
  ui->autocomplete_rect = pop;
  uis_popover_frame(ui, r, pop);
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_normal));
  for (size_t i = 0; i < ui->nautocomplete; i++) {
    ccord_rect rr = {pop.x + 6.0f, pop.y + 6.0f + (float)i * row_h, pop.w - 12.0f, row_h};
    bool hover = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, rr);
    uis_list_row_fill(ui, r, rr, (uis_state){.selected = i == ui->autocomplete_sel, .hover = hover},
                      "autocrow", (uint64_t)i);
    ccord_text_draw_ellipsis(r, ui->autocomplete_items[i].display, strlen(ui->autocomplete_items[i].display),
                             rr.x + 8.0f, rr.y + (rr.h - ui_line_h(ui, st.size)) * 0.5f, rr.w - 16.0f, &st);
  }
}

// ---------------------------------------------------------------- overlays: Quick Switcher, Lightbox & Modals

static bool substr_ci(const char *hay, const char *needle, size_t nlen)
{
  if (nlen == 0) return true;
  if (!hay) return false;
  size_t hlen = strlen(hay);
  if (hlen < nlen) return false;
  for (size_t i = 0; i + nlen <= hlen; i++) {
    if (ascii_prefix_ci(hay + i, needle, nlen)) return true;
  }
  return false;
}

ccord_rect ui_quick_switcher_rect(const ccord_ui *ui)
{
  float mw = clampf(ui->win_w - 64.0f, 320.0f, 540.0f);
  float mh = fminf(360.0f, ui->win_h - 24.0f);   // a short window keeps the old height: the list scrolls inside it
  if (mh < 160.0f) mh = 160.0f;
  return uis_modal_box(ui, mw, mh);
}

// A new query or a fresh open starts at the first row at the top; a wheel offset must not survive it.
void ui_quick_switcher_reset_list(ccord_ui *ui)
{
  ui->quick_switcher_sel = 0;
  ui->quick_switcher_scroll = 0.0f;
  ccord_scroller_cancel(&ui->quick_switcher_glide);
}

static void populate_quick_switcher(ccord_ui *ui)
{
  ui->nquick_switcher_items = 0;
  if (!ui || !ui->d.store) return;
  const char *q = ccord_text_edit_cstr(&ui->quick_switcher_edit);
  size_t qlen = ui->quick_switcher_edit.len;
  char filter = '\0';
  if (qlen > 0 && (q[0] == '@' || q[0] == '#' || q[0] == '!' || q[0] == '*')) {
    filter = q[0];
    q++;
    qlen--;
  }
  const ccord_store *st = ui->d.store;

  // 1. DM channels (@)
  if (filter == '\0' || filter == '@') {
    size_t dmc = ccord_store_channel_count(st, CCORD_DM_GUILD_ID);
    for (size_t i = 0; i < dmc && ui->nquick_switcher_items < 10; i++) {
      const ccord_channel *c = ccord_store_channel_at(st, CCORD_DM_GUILD_ID, i);
      if (!c || c->is_message_request) continue;
      const char *dname = ui_dm_display_name(ui, c);
      if (!substr_ci(dname, q, qlen) && !substr_ci(c->name, q, qlen)) continue;
      auto it = &ui->quick_switcher_items[ui->nquick_switcher_items++];
      it->guild_id = CCORD_DM_GUILD_ID;
      it->channel_id = c->id;
      it->user_id = c->user_id;
      snprintf(it->label, sizeof it->label, "@ %s", dname ? dname : c->name);
      snprintf(it->sub, sizeof it->sub, "Direct Message");
    }
  }

  // 2. Guild channels (# text, ! voice) and Guilds (*)
  size_t gc = ccord_store_guild_count(st);
  for (size_t g = 0; g < gc && ui->nquick_switcher_items < 10; g++) {
    const ccord_guild *gd = ccord_store_guild_at(st, g);
    if (!gd) continue;
    if ((filter == '\0' || filter == '*') && substr_ci(gd->name, q, qlen) && ui->nquick_switcher_items < 10) {
      auto it = &ui->quick_switcher_items[ui->nquick_switcher_items++];
      it->guild_id = gd->id;
      it->channel_id = CCORD_ID_NONE;
      it->user_id = CCORD_ID_NONE;
      snprintf(it->label, sizeof it->label, "* %s", gd->name);
      snprintf(it->sub, sizeof it->sub, "Server");
    }
    if (filter == '*' || filter == '@') continue;
    size_t cc = ccord_store_channel_count(st, gd->id);
    for (size_t c = 0; c < cc && ui->nquick_switcher_items < 10; c++) {
      const ccord_channel *ch = ccord_store_channel_at(st, gd->id, c);
      if (!ch || ch->kind == CCORD_CHANNEL_CATEGORY || !ui_channel_visible(ui, gd->id, ch)) continue;
      if (filter == '#' && ch->kind != CCORD_CHANNEL_TEXT) continue;
      if (filter == '!' && ch->kind != CCORD_CHANNEL_VOICE) continue;
      if (!substr_ci(ch->name, q, qlen)) continue;
      auto it = &ui->quick_switcher_items[ui->nquick_switcher_items++];
      it->guild_id = gd->id;
      it->channel_id = ch->id;
      it->user_id = CCORD_ID_NONE;
      snprintf(it->label, sizeof it->label, "%s %s", ch->kind == CCORD_CHANNEL_VOICE ? "!" : "#", ch->name);
      snprintf(it->sub, sizeof it->sub, "%s", gd->name ? gd->name : "");
    }
  }
  if (ui->quick_switcher_sel >= ui->nquick_switcher_items) ui_quick_switcher_reset_list(ui);
}

static void activate_quick_switcher_item(ccord_ui *ui, size_t idx)
{
  if (!ui || idx >= ui->nquick_switcher_items) return;
  auto it = &ui->quick_switcher_items[idx];
  ui->quick_switcher_open = false;
  ui->dm_friends_view = false;
  ui->dm_requests_view = false;
  ccord_error err;
  if (it->guild_id != CCORD_ID_NONE) {
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, it->guild_id, &err);
    else (void)ccord_store_select_guild(ui->d.store, it->guild_id, &err);
  }
  if (it->channel_id != CCORD_ID_NONE) {
    if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, it->channel_id, &err);
    else (void)ccord_store_select_channel(ui->d.store, it->channel_id, &err);
  }
  ui_ensure_caches(ui);
  ui_sync_messages(ui);
  ui_save_current_location(ui);
  ui->dirty = true;
}

// ---- modal geometry (shared by drawing and hit-testing, so the two cannot drift apart) ----

typedef struct confirm_choice {
  const char *label;
  int64_t value;   // timeout: milliseconds; ban: seconds of message history to delete
} confirm_choice;

static const confirm_choice k_timeout_choices[] = {
  {"60 secs", 60LL * 1000},        {"5 mins", 5LL * 60 * 1000},    {"10 mins", 10LL * 60 * 1000},
  {"1 hour", 60LL * 60 * 1000},    {"1 day", 24LL * 60 * 60 * 1000}, {"1 week", 7LL * 24 * 60 * 60 * 1000},
};
static const confirm_choice k_ban_choices[] = {
  {"None", 0}, {"1 hour", 3600}, {"24 hours", 86400}, {"3 days", 3 * 86400}, {"7 days", 7 * 86400},
};
#define UI_CONFIRM_DEFAULT_TIMEOUT 2   // "10 mins", Discord's default

static const confirm_choice *confirm_choices(ccord_admin_op op, size_t *n)
{
  if (op == CCORD_ADMIN_TIMEOUT) {
    *n = sizeof k_timeout_choices / sizeof k_timeout_choices[0];
    return k_timeout_choices;
  }
  if (op == CCORD_ADMIN_BAN) {
    *n = sizeof k_ban_choices / sizeof k_ban_choices[0];
    return k_ban_choices;
  }
  *n = 0;
  return nullptr;
}

#define UI_CONFIRM_MAX_CHIPS 8
static const uint32_t k_folder_color_presets[6] = {0u, 0x5865F2u, 0x57F287u, 0xFEE75Cu, 0xEB459Eu, 0xED4245u};

static bool parse_folder_hex_color(const char *s, uint32_t *out)
{
  *out = 0u;
  if (!s) return true;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return true;
  if (*s == '#') s++;
  uint32_t val = 0u;
  size_t digits = 0;
  while (s[digits] && s[digits] != ' ' && s[digits] != '\t') {
    unsigned char ch = (unsigned char)s[digits];
    uint32_t nib = 0;
    if (ch >= '0' && ch <= '9') nib = (uint32_t)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') nib = (uint32_t)(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') nib = (uint32_t)(ch - 'A' + 10);
    else return false;
    if (digits >= 6u) return false;
    val = (val << 4u) | nib;
    digits++;
  }
  const char *tail = s + digits;
  while (*tail == ' ' || *tail == '\t') tail++;
  if (*tail != '\0' || digits != 6u) return false;
  *out = val;
  return true;
}

typedef struct modal_geom {
  ccord_rect box, input, input2, ok, cancel;
  ccord_rect tabs[2], preview_btn;
  ccord_rect chip[UI_CONFIRM_MAX_CHIPS];
  size_t nchips;
  float body_y, choices_label_y;
} modal_geom;

static modal_geom modal_layout(const ccord_ui *ui)
{
  modal_geom g = {0};
  float mw = clampf(ui->win_w - 64.0f, 300.0f, ui->modal_kind == UI_MODAL_ADD_SERVER ? 460.0f : 440.0f);
  float mh = 220.0f;
  if (ui->modal_kind == UI_MODAL_CONFIRM) {
    size_t n = 0;
    (void)confirm_choices(ui->confirm_op, &n);
    bool no_input = (ui->confirm_op == CCORD_ADMIN_LEAVE_GUILD || ui->confirm_op == CCORD_ADMIN_TRANSFER_OWNERSHIP);
    mh = 56.0f + 64.0f + (n ? 56.0f : 0.0f) + (no_input ? 0.0f : 52.0f) + 62.0f;   // title, body, choices, input, buttons
  } else if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
    mh = (ui->modal_type_sel == 0) ? 318.0f : 246.0f;
  } else if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
    mh = 296.0f;
  }
  g.box = uis_modal_box(ui, mw, mh);
  g.ok = (ccord_rect){g.box.x + g.box.w - 160.0f, g.box.y + g.box.h - 46.0f, 140.0f, UIS_H_BTN};
  if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
    float tw = (mw - 48.0f) * 0.5f;
    g.tabs[0] = (ccord_rect){g.box.x + 20.0f, g.box.y + 55.0f, tw, UIS_H_BTN_SM};
    g.tabs[1] = (ccord_rect){g.tabs[0].x + tw + 8.0f, g.box.y + 55.0f, tw, UIS_H_BTN_SM};
    if (ui->modal_type_sel == 0) {
      g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 114.0f, mw - 40.0f - 92.0f, 36.0f};
      g.preview_btn = (ccord_rect){g.input.x + g.input.w + 8.0f, g.input.y + 2.0f, 84.0f, UIS_H_BTN};
    } else {
      g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 114.0f, mw - 40.0f, 36.0f};
    }
    g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, 32.0f};
    return g;
  }
  if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
    g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 74.0f, mw - 40.0f, 36.0f};
    g.input2 = (ccord_rect){g.box.x + 20.0f, g.box.y + 142.0f, mw - 40.0f, 36.0f};
    float gap = 8.0f, inner = mw - 40.0f;
    float cw = (inner - gap * 5.0f) / 6.0f;
    for (size_t i = 0; i < 6u; i++) {
      g.chip[i] = (ccord_rect){g.box.x + 20.0f + (float)i * (cw + gap), g.box.y + 190.0f, cw, 28.0f};
    }
    g.nchips = 6u;
    g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, 32.0f};
    return g;
  }
  if (ui->modal_kind != UI_MODAL_CONFIRM) {
    g.input = (ccord_rect){g.box.x + 20.0f, g.box.y + 64.0f, g.box.w - 40.0f, 36.0f};
    g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, UIS_H_BTN};
    return g;
  }
  g.cancel = (ccord_rect){g.ok.x - 100.0f, g.ok.y, 90.0f, 32.0f};
  size_t n = 0;
  (void)confirm_choices(ui->confirm_op, &n);
  g.body_y = g.box.y + 54.0f;
  float y = g.body_y + 64.0f;
  if (n > 0 && n <= UI_CONFIRM_MAX_CHIPS) {
    g.choices_label_y = y;
    float gap = 6.0f, inner = g.box.w - 40.0f;
    float cw = (inner - gap * (float)(n - 1)) / (float)n;
    for (size_t i = 0; i < n; i++) {
      g.chip[i] = (ccord_rect){g.box.x + 20.0f + (float)i * (cw + gap), y + 20.0f, cw, 28.0f};
    }
    g.nchips = n;
    y += 56.0f;
  }
  if (ui->confirm_op != CCORD_ADMIN_LEAVE_GUILD && ui->confirm_op != CCORD_ADMIN_TRANSFER_OWNERSHIP) {
    g.input = (ccord_rect){g.box.x + 20.0f, y, g.box.w - 40.0f, 36.0f};
  }
  return g;
}

void ui_confirm_open(ccord_ui *ui, ccord_admin_op op, ccord_id guild_id, ccord_id target_id, const char *name)
{
  ui->modal_kind = UI_MODAL_CONFIRM;
  ui->confirm_op = op;
  ui->modal_guild_id = guild_id;
  ui->modal_target_id = target_id;
  ui->modal_type_sel = op == CCORD_ADMIN_TIMEOUT ? UI_CONFIRM_DEFAULT_TIMEOUT : 0;
  (void)snprintf(ui->confirm_name, sizeof ui->confirm_name, "%s", name ? name : "");
  ccord_text_edit_clear(&ui->modal_edit1);
  ui->dirty = true;
}

static bool has_global_name_differs(const char *global, const char *shown)
{
  return global && global[0] && (!shown || strcmp(global, shown) != 0);
}

static bool new_dm_has_recipient(const ccord_ui *ui, ccord_id uid)
{
  for (size_t i = 0; i < ui->new_dm_recipient_count; i++) {
    if (ui->new_dm_recipients[i] == uid) return true;
  }
  return false;
}

static void new_dm_toggle_recipient(ccord_ui *ui, ccord_id uid, size_t max_add)
{
  for (size_t i = 0; i < ui->new_dm_recipient_count; i++) {
    if (ui->new_dm_recipients[i] == uid) {
      for (size_t k = i + 1u; k < ui->new_dm_recipient_count; k++) {
        ui->new_dm_recipients[k - 1u] = ui->new_dm_recipients[k];
      }
      ui->new_dm_recipient_count--;
      ui->dirty = true;
      return;
    }
  }
  if (ui->new_dm_recipient_count >= max_add || ui->new_dm_recipient_count >= CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
    ui_notice(ui, "Group DMs can have at most 10 members");
    return;
  }
  ui->new_dm_recipients[ui->new_dm_recipient_count++] = uid;
  ui->dirty = true;
}

static void draw_group_dm_modal(ccord_ui *ui, ccord_renderer *r, bool is_add)
{
  ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
  float mw = clampf(ui->win_w - 64.0f, 320.0f, 460.0f);
  float mh = is_add ? 380.0f : 436.0f;
  ccord_rect m = uis_modal_box(ui, mw, mh);
  uis_modal_frame(ui, r, m);

  const ccord_channel *ech = is_add ? ccord_store_channel(ui->d.store, ui->modal_target_id) : nullptr;
  size_t existing = (ech && ech->recipient_count <= CCORD_MAX_DM_RECIPIENTS) ? ech->recipient_count : 0u;
  size_t max_add = existing < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX ? (CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX - existing) : 0u;
  size_t rem_slots = max_add > ui->new_dm_recipient_count ? (max_add - ui->new_dm_recipient_count) : 0u;

  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  const char *title = is_add ? "Add Friends to DM" : "Select Friends";
  ccord_text_draw(r, title, strlen(title), m.x + 20.0f, m.y + 16.0f, &ts, 0.0f);

  char sub_buf[96];
  snprintf(sub_buf, sizeof sub_buf, "You can add %zu more friend%s.", rem_slots, rem_slots == 1u ? "" : "s");
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, sub_buf, strlen(sub_buf), m.x + 20.0f, m.y + 42.0f, m.w - 40.0f, &ss);

  ccord_rect inp = {m.x + 20.0f, m.y + 66.0f, m.w - 40.0f, 36.0f};
  ui->modal_input_rect = inp;
  const char *v1 = ccord_text_edit_cstr(&ui->modal_edit1);
  (void)uis_input(ui, r, inp, &ui->modal_edit1, "Type the username or ID of a friend",
                  (uis_state){.focused = ui->modal_focus_field == 0}, false, "gdmin", 0);

  float fy = inp.y + inp.h + 10.0f;
  float list_bottom = is_add ? (m.y + m.h - 58.0f) : (m.y + m.h - 118.0f);
  ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
  size_t nrel = ui->d.store ? ccord_store_relationship_count(ui->d.store) : 0u;
  size_t qlen = strlen(v1);
  ccord_text_style fns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style fhs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));

  for (size_t i = 0; i < nrel && ui->nnew_dm_friend_hits < 8u && fy + 30.0f <= list_bottom; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    if (!rel || rel->kind != CCORD_RELATIONSHIP_FRIEND || rel->user_id == self_id) continue;
    if (ech) {
      bool already_in = false;
      for (size_t ri = 0; ri < ech->recipient_count && ri < CCORD_MAX_DM_RECIPIENTS; ri++) {
        if (ech->recipient_ids[ri] == rel->user_id) {
          already_in = true;
          break;
        }
      }
      if (already_in) continue;
    }
    const ccord_user *fu = ccord_store_user(ui->d.store, rel->user_id);
    const char *fname = (fu && fu->display_name && fu->display_name[0]) ? fu->display_name
                        : ((fu && fu->name && fu->name[0]) ? fu->name : "Friend");
    const char *fhand = (fu && fu->handle && fu->handle[0]) ? fu->handle : fname;
    if (qlen > 0 && strchr(v1, ',') == nullptr && !substr_ci(fname, v1, qlen) && !substr_ci(fhand, v1, qlen)) continue;

    ccord_rect fr = {m.x + 16.0f, fy, m.w - 32.0f, 28.0f};
    bool fhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, fr);
    bool sel = new_dm_has_recipient(ui, rel->user_id);
    uis_plate(ui, r, fr, uis_tokens_of(ui).r_control, COL(bg_tertiary));
    uis_list_row_fill(ui, r, fr, (uis_state){.hover = fhv, .selected = sel}, "gdmrow", (uint64_t)rel->user_id);
    ui_draw_avatar(ui, r, fu, fr.x + 16.0f, fr.y + fr.h * 0.5f, 20.0f, COL(bg_tertiary), false);
    float nw = ccord_text_draw_ellipsis(r, fname, strlen(fname), fr.x + 32.0f, fr.y + 5.0f, fr.w - 120.0f, &fns);
    char htag[80];
    snprintf(htag, sizeof htag, "@%s", fhand);
    ccord_text_draw_ellipsis(r, htag, strlen(htag), fr.x + 38.0f + nw, fr.y + 7.0f,
                             maxf(fr.w - (64.0f + nw), 16.0f), &fhs);
    ccord_rect cb = {fr.x + fr.w - 22.0f, fr.y + 7.0f, 14.0f, 14.0f};
    if (sel) {
      ccord_draw_rrect(r, cb, 3.0f, COL(brand));
      ccord_draw_rrect(r, (ccord_rect){cb.x + 4.0f, cb.y + 4.0f, 6.0f, 6.0f}, 1.5f, COL(text_on_brand));
    } else {
      ccord_draw_rrect(r, cb, 3.0f, COL(text_muted));
      ccord_draw_rrect(r, (ccord_rect){cb.x + 1.5f, cb.y + 1.5f, 11.0f, 11.0f}, 2.0f, COL(bg_secondary));
    }
    ui->new_dm_friend_hits[ui->nnew_dm_friend_hits].rect = fr;
    ui->new_dm_friend_hits[ui->nnew_dm_friend_hits].user_id = rel->user_id;
    ui->nnew_dm_friend_hits++;
    fy += 31.0f;
  }

  if (!is_add) {
    ccord_text_style gls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_draw(r, "GROUP NAME (OPTIONAL)", 21, m.x + 20.0f, m.y + m.h - 108.0f, &gls, 0.0f);
    ccord_rect inp2 = {m.x + 20.0f, m.y + m.h - 92.0f, m.w - 40.0f, UIS_H_INPUT};
    ui->modal_input2_rect = inp2;
    (void)uis_input(ui, r, inp2, &ui->modal_edit2, "Optional group name", (uis_state){.focused = ui->modal_focus_field == 1}, false,
                    "gdmin2", 0);
  }

  ccord_rect ok_r = {m.x + m.w - 160.0f, m.y + m.h - 44.0f, 140.0f, 32.0f};
  ccord_rect cn_r = {ok_r.x - 98.0f, ok_r.y, 88.0f, 32.0f};
  ui->modal_ok_rect = ok_r;
  ui->inter_modal_box = m;
  ui->inter_modal_cancel = cn_r;
  uis_button(ui, r, cn_r, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cn_r)}, "modcancel", 0);
  uis_button(ui, r, ok_r, is_add ? "Add" : (ui->new_dm_recipient_count >= 2u ? "Create Group DM" : "Create DM"), UIS_BTN_PRIMARY,
             UIS_BTN_S_MD, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ok_r)}, "modok", 0);
  ui_popup_end(r);
}

static void draw_edit_group_dm_modal(ccord_ui *ui, ccord_renderer *r)
{
  ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
  float mw = clampf(ui->win_w - 64.0f, 300.0f, 440.0f), mh = 248.0f;
  ccord_rect m = uis_modal_box(ui, mw, mh);
  ui->inter_modal_box = m;
  uis_modal_frame(ui, r, m);

  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, "Edit Group DM", 13, m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_draw(r, "GROUP NAME", 10, m.x + 20.0f, m.y + 56.0f, &ls, 0.0f);

  ccord_rect inp = {m.x + 20.0f, m.y + 74.0f, m.w - 40.0f, 36.0f};
  ui->modal_input_rect = inp;
  (void)uis_input(ui, r, inp, &ui->modal_edit1, "Group DM Name", (uis_state){.focused = true}, false, "egdmin", 0);

  ccord_rect ic_chip = {m.x + 20.0f, m.y + 124.0f, 180.0f, 28.0f};
  ui->modal_tab_rects[0] = ic_chip;
  bool clr_icon = (ui->modal_type_sel == 1);
  uis_tab(ui, r, ic_chip, clr_icon ? "Removing Group Icon" : "Remove Group Icon",
          (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ic_chip), .selected = clr_icon}, "egdmicon", 0);

  ccord_rect ok_r = {m.x + m.w - 150.0f, m.y + m.h - 44.0f, 130.0f, 32.0f};
  ccord_rect cn_r = {ok_r.x - 96.0f, ok_r.y, 86.0f, 32.0f};
  ui->modal_ok_rect = ok_r;
  ui->inter_modal_cancel = cn_r;
  uis_button(ui, r, cn_r, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cn_r)}, "modcancel", 0);
  uis_button(ui, r, ok_r, "Save Changes", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ok_r)}, "modok", 0);
  ui_popup_end(r);
}

static void draw_user_profile_modal(ccord_ui *ui, ccord_renderer *r)
{
  ccord_id uid = ui->modal_target_id;
  if (uid == CCORD_ID_NONE) return;
  ccord_id gid = (ui->modal_guild_id != CCORD_ID_NONE && ui->modal_guild_id != CCORD_DM_GUILD_ID)
                   ? ui->modal_guild_id
                   : CCORD_ID_NONE;
  if (ui->d.session) {
    ccord_error perr;
    (void)ccord_session_request_profile_ex(ui->d.session, uid, gid, ui->now_ms, false, &perr);
  }
  const ccord_user *store_u = ccord_store_user(ui->d.store, uid);
  const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, uid);
  ccord_user view = store_u ? *store_u : (ccord_user){0};
  view.id = uid;
  const char *p_name = (prof && prof->display_name && prof->display_name[0]) ? prof->display_name : nullptr;
  const char *u_dname = (store_u && store_u->display_name && store_u->display_name[0]) ? store_u->display_name : nullptr;
  const char *u_name = (store_u && store_u->name && store_u->name[0]) ? store_u->name : nullptr;
  const char *p_hand = (prof && prof->handle && prof->handle[0]) ? prof->handle : nullptr;
  const char *u_hand = (store_u && store_u->handle && store_u->handle[0]) ? store_u->handle : nullptr;
  view.name = p_name ? p_name : (u_dname ? u_dname : (u_name ? u_name : (p_hand ? p_hand : (u_hand ? u_hand : "Discord User"))));
  view.display_name = view.name;
  view.handle = p_hand ? p_hand : (u_hand ? u_hand : view.name);
  view.avatar_url = (prof && prof->avatar_url && prof->avatar_url[0]) ? prof->avatar_url
                    : ((store_u && store_u->avatar_url) ? store_u->avatar_url : "");
  view.banner_url = (prof && prof->banner_url && prof->banner_url[0]) ? prof->banner_url
                    : ((store_u && store_u->banner_url) ? store_u->banner_url : "");
  view.accent_color = (prof && prof->accent_color != 0) ? prof->accent_color : (store_u ? store_u->accent_color : 0);
  view.bio = (prof && prof->bio && prof->bio[0]) ? prof->bio : ((store_u && store_u->bio) ? store_u->bio : "");
  view.pronouns = (prof && prof->pronouns && prof->pronouns[0]) ? prof->pronouns
                  : ((store_u && store_u->pronouns) ? store_u->pronouns : "");
  if (prof && prof->clan_tag[0] != '\0') {
    snprintf(view.clan_tag, sizeof view.clan_tag, "%s", prof->clan_tag);
  }
  if (prof && prof->clan_guild_id != CCORD_ID_NONE) {
    view.clan_guild_id = prof->clan_guild_id;
  }
  if (prof && prof->clan_badge[0] != '\0') {
    snprintf(view.clan_badge, sizeof view.clan_badge, "%s", prof->clan_badge);
  }
  if (!store_u) {
    view.presence = CCORD_PRESENCE_OFFLINE;
    view.avatar_palette = (uint8_t)(uid % 6u);
  }
  ui_profile_look look;
  ui_profile_resolve(prof, gid, &view, &look);
  ui_profile_text ptx = ui_profile_text_colors(ui, &look);
  const ccord_user *u = &view;

  ui_popup_begin(ui, r, "pmodal", uid, UI_MODAL_MS, 10.0f);
  float mw = clampf(ui->win_w - 48.0f, 360.0f, UIS_MODAL_W_LG);
  float mh = clampf(ui->win_h - 48.0f, 340.0f, 620.0f);
  ccord_rect m = uis_modal_box(ui, mw, mh);
  ui->profile_modal_box = m;
  uis_modal_frame(ui, r, m);
  uis_tokens ptk = uis_tokens_of(ui);
  ccord_push_clip(r, m);

  ui_profile_draw_theme_bg(ui, r, &look, m, 104.0f, ptk.r_surface);
  ccord_rect banner = {m.x, m.y, m.w, 104.0f};
  ui_profile_draw_banner(ui, r, &look, u, banner, ptk.r_surface);
  ui->profile_modal_close_rect = (ccord_rect){m.x + m.w - UIS_ICON_BTN - UIS_SP_2, m.y + UIS_SP_2, UIS_ICON_BTN, UIS_ICON_BTN};
  {
    ccord_rect cr = ui->profile_modal_close_rect;
    ccord_draw_rrect(r, cr, ptk.r_control, ui_alpha(COL(bg_tertiary), 0.8f));   // scrim chip so the glyph reads on a banner
    ccord_color cg = uis_icon_button(ui, r, cr, (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr)},
                                     "pmclose", 0);
    ui_icon_close(r, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 12.0f, cg);
  }

  float av_cx = m.x + 24.0f + 38.0f, av_cy = m.y + 104.0f;
  ccord_color av_ring = ui_profile_card_bg(ui, &look, COL(bg_secondary));
  ccord_draw_circle(r, av_cx, av_cy, 43.0f, av_ring);
  ui_draw_avatar(ui, r, u, av_cx, av_cy, 76.0f, av_ring, false);
  ui_profile_draw_deco(ui, r, &look, av_cx, av_cy, 76.0f);

  ccord_id self_id = ccord_store_self_id(ui->d.store);
  if (uid != self_id) {
    ccord_rect mb = {m.x + m.w - 148.0f, m.y + 116.0f, 124.0f, UIS_H_BTN};
    ui->profile_modal_msg_rect = mb;
    uis_button(ui, r, mb, "Send Message", UIS_BTN_SUCCESS, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mb)}, "pmmsg", 0);
  }

  float hx = m.x + 24.0f, hy = m.y + 150.0f, hw = m.w - 48.0f;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 1.0f, ptx.header);
  float nlh = ui_line_h(ui, ns.size);
  float nw = ccord_text_draw_ellipsis(r, u->display_name, strlen(u->display_name), hx, hy, hw - 110.0f, &ns);
  float bx = hx + nw + 8.0f;
  if (u->clan_tag[0]) {
    bx += ui_draw_clan_badge_ex(ui, r, u->clan_guild_id, u->clan_tag, u->clan_badge, bx, hy, nlh, true) + 6.0f;
  }
  if (u->is_bot) {
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
    ccord_vec2 bt = ccord_text_measure(r, "BOT", 3, &bs, 0.0f);
    ccord_rect btag = {bx, hy + (nlh - 16.0f) * 0.5f, bt.x + 10.0f, 16.0f};
    ccord_draw_rrect(r, btag, 3.5f, COL(brand));
    ccord_text_draw(r, "BOT", 3, btag.x + 5.0f, btag.y + (btag.h - bt.y) * 0.5f, &bs, 0.0f);
  }
  hy += nlh + 2.0f;

  char hline[256];
  bool show_global = look.server_profile && has_global_name_differs(look.global_name, u->display_name);
  int hn = snprintf(hline, sizeof hline, "%s%s@%s", show_global ? look.global_name : "", show_global ? " \xE2\x80\xA2 " : "", u->handle);
  if (u->pronouns && u->pronouns[0] && hn > 0 && (size_t)hn < sizeof hline) {
    snprintf(hline + hn, sizeof hline - (size_t)hn, " \xE2\x80\xA2 %s", u->pronouns);
  }
  ccord_text_style sub = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), ptx.secondary);
  ccord_text_draw_ellipsis(r, hline, strlen(hline), hx, hy, hw, &sub);
  hy += ui_line_h(ui, sub.size) + 10.0f;
  {
    float bh = ui_profile_draw_badges(ui, r, &look, hx, hy - 4.0f, hw, false);
    if (bh > 0.0f) hy += bh;
  }

  size_t mg_cnt = prof ? prof->mutual_guild_count : 0u;
  uint32_t mf_cnt = prof ? (prof->mutual_friend_total >= prof->mutual_friend_count
                              ? prof->mutual_friend_total
                              : (uint32_t)prof->mutual_friend_count)
                         : 0u;
  char t1_lbl[48], t2_lbl[48];
  snprintf(t1_lbl, sizeof t1_lbl, "Mutual Servers (%zu)", mg_cnt);
  snprintf(t2_lbl, sizeof t2_lbl, "Mutual Friends (%u)", (unsigned)mf_cnt);
  const char *ptabs[3] = {"User Info", t1_lbl, t2_lbl};
  float tx = hx;
  for (int t = 0; t < 3; t++) {
    bool sel = (ui->profile_modal_tab == t);
    ccord_text_style ts_t = ui_style(ui, sel ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     sel ? ptx.header : ptx.muted);
    ccord_vec2 tt = ccord_text_measure(r, ptabs[t], strlen(ptabs[t]), &ts_t, 0.0f);
    ccord_rect tr = {tx, hy, tt.x + 16.0f, 28.0f};
    ui->profile_modal_tab_rects[t] = tr;
    ccord_text_draw(r, ptabs[t], strlen(ptabs[t]), tr.x + 8.0f, tr.y + (tr.h - tt.y) * 0.5f, &ts_t, 0.0f);
    if (sel) {
      ccord_draw_rect(r, (ccord_rect){tr.x + 4.0f, tr.y + tr.h - 2.0f, tr.w - 8.0f, 2.0f}, COL(brand));
    }
    tx += tr.w + 10.0f;
  }
  hy += 30.0f;
  uis_divider(ui, r, hx, hy, hw);
  hy += 10.0f;

  ccord_rect body_area = {m.x + 16.0f, hy, m.w - 32.0f, maxf((m.y + m.h - 14.0f) - hy, 40.0f)};
  uis_plate(ui, r, body_area, ptk.r_surface,
            look.has_theme ? ui_alpha(look.light_text ? (ccord_color){0, 0, 0, 255} : (ccord_color){255, 255, 255, 255}, 0.22f)
                           : COL(bg_tertiary));
  ccord_push_clip(r, body_area);
  ui_profile_roles_set_clip(ui, body_area);   // the role + / x hit only where the body is visible
  float body_y0 = body_area.y, body_y1 = body_area.y + body_area.h;

  float ix = body_area.x + 14.0f, iw = body_area.w - 28.0f;
  float max_s = maxf(ui->profile_modal_content_h - body_area.h, 0.0f);
  ui->profile_modal_max = max_s;
  ui->profile_modal_scroll = clampf(ui->profile_modal_scroll, 0.0f, max_s);
  float cy = body_area.y + 12.0f - ui->profile_modal_scroll;

  ccord_text_style hdr_s = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, ptx.header);
  ccord_text_style bdy_s = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), ptx.secondary);
  bool dates_shown = false;

  if (ui->profile_modal_tab == 0) {
    ccord_text_draw(r, "ABOUT ME", 8, ix, cy, &hdr_s, 0.0f);
    cy += ui_line_h(ui, hdr_s.size) + 4.0f;
    const char *bio = (u->bio && u->bio[0]) ? u->bio : "No bio set.";
    ccord_vec2 bm = ccord_text_measure(r, bio, strlen(bio), &bdy_s, iw);
    ccord_text_draw(r, bio, strlen(bio), ix, cy, &bdy_s, iw);
    cy += maxf(bm.y, ui_line_h(ui, bdy_s.size)) + 12.0f;

    if (u->has_activity && u->activity_name[0] != '\0') {
      cy += ui_draw_activity_card(ui, r, u, ix, cy, iw, false) + 12.0f;
    }

    ccord_text_draw(r, "MEMBER SINCE", 12, ix, cy, &hdr_s, 0.0f);
    cy += ui_line_h(ui, hdr_s.size) + 4.0f;
    cy += ui_profile_draw_since(ui, r, &look, ix, cy, iw, &bdy_s, false) + 12.0f;
    dates_shown = true;

    if (gid != CCORD_ID_NONE) {
      float rh = ui_draw_member_role_pills(ui, r, gid, uid, ix, cy, iw, true);
      if (rh > 0.0f) {
        ccord_text_draw(r, "ROLES", 5, ix, cy, &hdr_s, 0.0f);
        cy += ui_line_h(ui, hdr_s.size) + 4.0f;
        cy += ui_draw_member_role_pills(ui, r, gid, uid, ix, cy, iw, false) + 8.0f;
      }
    }

    ccord_text_draw(r, "NOTE (ONLY VISIBLE TO YOU)", 26, ix, cy, &hdr_s, 0.0f);
    cy += ui_line_h(ui, hdr_s.size) + 4.0f;
    ccord_rect nr = {ix, cy, maxf(iw - 92.0f, 80.0f), UIS_H_INPUT};
    ccord_rect nsb = {nr.x + nr.w + 8.0f, cy + 2.0f, 84.0f, UIS_H_BTN};
    if (nr.y + nr.h > body_y0 && nr.y < body_y1) {
      float ny0 = maxf(nr.y, body_y0), ny1 = fminf(nr.y + nr.h, body_y1);
      ui->profile_modal_note_rect = (ccord_rect){nr.x, ny0, nr.w, ny1 - ny0};
      ui->profile_modal_note_save_rect = (ccord_rect){nsb.x, ny0, nsb.w, ny1 - ny0};
    }
    (void)uis_input(ui, r, nr, &ui->modal_edit1, "Click to add a note", (uis_state){.focused = true}, false, "pmnote", 0);
    uis_button(ui, r, nsb, "Save Note", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, nsb)}, "pmnotesave", 0);
    cy += 44.0f;

    if (prof && prof->connection_count > 0) {
      ccord_text_draw(r, "CONNECTIONS", 11, ix, cy, &hdr_s, 0.0f);
      cy += ui_line_h(ui, hdr_s.size) + 6.0f;
      float cw = (iw - 8.0f) * 0.5f;
      for (size_t ci = 0; ci < prof->connection_count; ci++) {
        const ccord_profile_connection *cn = &prof->connections[ci];
        float cx = ix + (float)(ci % 2u) * (cw + 8.0f);
        float cry = cy + (float)(ci / 2u) * 40.0f;
        ccord_rect cr = {cx, cry, cw, 34.0f};
        bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cr);
        uis_plate(ui, r, cr, ptk.r_control, COL(bg_secondary));
        uis_list_row_fill(ui, r, cr, (uis_state){.hover = chv}, "pmconn", (uint64_t)ci);
        char clbl[160];
        snprintf(clbl, sizeof clbl, "%s: %s", cn->type[0] ? cn->type : "Account", cn->name);
        float vres = cn->verified ? 58.0f : 0.0f;
        ccord_text_style cts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
        ccord_text_draw_ellipsis(r, clbl, strlen(clbl), cr.x + 8.0f, cr.y + 9.0f, maxf(cr.w - 16.0f - vres, 20.0f), &cts);
        if (cn->verified) {
          ccord_text_style vts = ui_style(ui, CCORD_FONT_BOLD, 9.0f, COL(status_online));
          ccord_text_draw(r, "VERIFIED", 8, cr.x + cr.w - 54.0f, cr.y + 11.0f, &vts, 0.0f);
        }
        if (ui->nprofile_modal_conn_hits < CCORD_PROFILE_MAX_CONNECTIONS &&
            cr.y + cr.h > body_y0 && cr.y < body_y1) {
          float cy0 = maxf(cr.y, body_y0), cy1 = fminf(cr.y + cr.h, body_y1);
          char tmp_url[256] = "";
          if (cn->name[0]) {
            if (strcmp(cn->type, "github") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://github.com/%s", cn->name);
            } else if (strcmp(cn->type, "twitch") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://twitch.tv/%s", cn->name);
            } else if (strcmp(cn->type, "reddit") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://reddit.com/u/%s", cn->name);
            } else if (strcmp(cn->type, "twitter") == 0 || strcmp(cn->type, "x") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://x.com/%s", cn->name);
            } else if (strcmp(cn->type, "youtube") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://youtube.com/@%s", cn->name);
            } else if (strcmp(cn->type, "steam") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://steamcommunity.com/id/%s", cn->name);
            } else if (strcmp(cn->type, "bluesky") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://bsky.app/profile/%s", cn->name);
            } else if (strcmp(cn->type, "tiktok") == 0) {
              snprintf(tmp_url, sizeof tmp_url, "https://tiktok.com/@%s", cn->name);
            }
          }
          size_t idx = ui->nprofile_modal_conn_hits++;
          ui->profile_modal_conn_hits[idx].rect = (ccord_rect){cr.x, cy0, cr.w, cy1 - cy0};
          ui->profile_modal_conn_hits[idx].url = ui_hit_cstr(ui, tmp_url);
        }
      }
      cy += (float)((prof->connection_count + 1u) / 2u) * 40.0f + 6.0f;
    }
  } else if (ui->profile_modal_tab == 1) {
    if (!prof || prof->mutual_guild_count == 0) {
      ccord_text_draw(r, "No mutual servers", 17, ix, cy + 8.0f, &bdy_s, 0.0f);
      cy += 36.0f;
    } else {
      ccord_text_style rts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
      for (size_t i = 0; i < prof->mutual_guild_count; i++) {
        ccord_id mgid = prof->mutual_guild_ids[i];
        const ccord_guild *gd = ccord_store_guild(ui->d.store, mgid);
        char gname_buf[64];
        const char *gname = (gd && gd->name && gd->name[0]) ? gd->name : nullptr;
        if (!gname) {
          snprintf(gname_buf, sizeof gname_buf, "Server %llu", (unsigned long long)mgid);
          gname = gname_buf;
        }
        ccord_rect rr = {ix, cy, iw, 40.0f};
        bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rr);
        uis_plate(ui, r, rr, ptk.r_control, COL(bg_secondary));
        uis_list_row_fill(ui, r, rr, (uis_state){.hover = rhv}, "pmmg", (uint64_t)mgid);
        ccord_rect gic = {rr.x + 8.0f, rr.y + 6.0f, 28.0f, 28.0f};
        ccord_draw_rrect(r, gic, 8.0f, COL(brand));
        char init[8];
        ui_initials(gname, init);
        ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_on_brand));
        ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
        ccord_text_draw(r, init, strlen(init), gic.x + (gic.w - it.x) * 0.5f, gic.y + (gic.h - it.y) * 0.5f, &ist, 0.0f);
        ccord_text_draw_ellipsis(r, gname, strlen(gname), rr.x + 46.0f, rr.y + 10.0f, rr.w - 56.0f, &rts);
        if (ui->nprofile_modal_mutual_hits < 32u && rr.y + rr.h > body_y0 && rr.y < body_y1) {
          float ry0 = maxf(rr.y, body_y0), ry1 = fminf(rr.y + rr.h, body_y1);
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].rect = (ccord_rect){rr.x, ry0, rr.w, ry1 - ry0};
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].id = mgid;
          ui->nprofile_modal_mutual_hits++;
        }
        cy += 44.0f;
      }
    }
  } else {
    if (!prof || prof->mutual_friend_count == 0) {
      ccord_text_draw(r, "No mutual friends", 17, ix, cy + 8.0f, &bdy_s, 0.0f);
      cy += 36.0f;
    } else {
      ccord_text_style rts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
      for (size_t i = 0; i < prof->mutual_friend_count; i++) {
        const ccord_mutual_friend *mf = &prof->mutual_friends[i];
        const char *fnm = mf->name[0] ? mf->name : (mf->handle[0] ? mf->handle : "User");
        ccord_rect rr = {ix, cy, iw, 40.0f};
        bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rr);
        uis_plate(ui, r, rr, ptk.r_control, COL(bg_secondary));
        uis_list_row_fill(ui, r, rr, (uis_state){.hover = rhv}, "pmmf", (uint64_t)mf->user_id);
        ccord_user fu = {
          .id = mf->user_id,
          .name = fnm,
          .avatar_url = mf->avatar_url,
          .presence = CCORD_PRESENCE_OFFLINE,
          .avatar_palette = (uint8_t)(mf->user_id % 6u),
        };
        ui_draw_avatar(ui, r, &fu, rr.x + 22.0f, rr.y + 20.0f, 28.0f, COL(bg_secondary), false);
        float fnw = ccord_text_draw_ellipsis(r, fnm, strlen(fnm), rr.x + 44.0f, rr.y + 10.0f, rr.w * 0.55f, &rts);
        if (mf->handle[0]) {
          char htag[112];
          snprintf(htag, sizeof htag, "@%s", mf->handle);
          ccord_text_draw_ellipsis(r, htag, strlen(htag), rr.x + 52.0f + fnw, rr.y + 12.0f,
                                   maxf(rr.w - (60.0f + fnw), 20.0f), &bdy_s);
        }
        if (ui->nprofile_modal_mutual_hits < 32u && rr.y + rr.h > body_y0 && rr.y < body_y1) {
          float ry0 = maxf(rr.y, body_y0), ry1 = fminf(rr.y + rr.h, body_y1);
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].rect = (ccord_rect){rr.x, ry0, rr.w, ry1 - ry0};
          ui->profile_modal_mutual_hits[ui->nprofile_modal_mutual_hits].id = mf->user_id;
          ui->nprofile_modal_mutual_hits++;
        }
        cy += 44.0f;
      }
    }
  }
  ui->profile_modal_content_h = (cy + ui->profile_modal_scroll) - body_area.y + 8.0f;
  ccord_pop_clip(r);
  ccord_pop_clip(r);
  ui_profile_view_debug_record(true, &look, u, dates_shown);
  ui_profile_flush_tooltip(ui, r);
  ui_popup_end(r);
}

static ccord_id resolve_user_token(const ccord_ui *ui, const char *raw, size_t len)
{
  while (len > 0 && (*raw == ' ' || *raw == '\t')) { raw++; len--; }
  while (len > 0 && (raw[len - 1u] == ' ' || raw[len - 1u] == '\t')) len--;
  if (len == 0 || len >= 96u) return CCORD_ID_NONE;
  char tok[96];
  memcpy(tok, raw, len);
  tok[len] = '\0';
  ccord_id uid = CCORD_ID_NONE;
  if (ccord_id_parse(tok, len, &uid) && uid >= (1ULL << 22)) return uid;
  if (!ui->d.store) return CCORD_ID_NONE;
  size_t nrel = ccord_store_relationship_count(ui->d.store);
  for (size_t i = 0; i < nrel && uid == CCORD_ID_NONE; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    const ccord_user *u = rel ? ccord_store_user(ui->d.store, rel->user_id) : nullptr;
    if (u && ((u->name && strcasecmp(u->name, tok) == 0) ||
              (u->handle && strcasecmp(u->handle, tok) == 0) ||
              (u->display_name && strcasecmp(u->display_name, tok) == 0))) {
      uid = u->id;
    }
  }
  size_t ng = ccord_store_guild_count(ui->d.store);
  for (size_t g = 0; g < ng && uid == CCORD_ID_NONE; g++) {
    const ccord_guild *gd = ccord_store_guild_at(ui->d.store, g);
    if (!gd) continue;
    size_t nm = ccord_store_member_count(ui->d.store, gd->id);
    for (size_t i = 0; i < nm && uid == CCORD_ID_NONE; i++) {
      const ccord_member *m = ccord_store_member_at(ui->d.store, gd->id, i);
      const ccord_user *u = m ? ccord_store_user(ui->d.store, m->user_id) : nullptr;
      if (u && ((u->name && strcasecmp(u->name, tok) == 0) ||
                (u->handle && strcasecmp(u->handle, tok) == 0) ||
                (u->display_name && strcasecmp(u->display_name, tok) == 0))) {
        uid = u->id;
      }
    }
  }
  return uid;
}

static void save_profile_modal_note(ccord_ui *ui)
{
  if (!ui || ui->modal_target_id == CCORD_ID_NONE) return;
  const char *note = ccord_text_edit_cstr(&ui->modal_edit1);
  ccord_admin_request req;
  ccord_error err;
  if (ccord_admin_set_user_note(&req, ui->modal_target_id, note, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't save note: %s", err.message);
    return;
  }
  ui_admin_submit(ui, &req);
  ui_notice(ui, "Note saved");
}

// ---------------------------------------------------------------- membership screening
// A guild with member verification keeps a freshly joined member "pending" until they accept its rules. The modal
// shows the form the session fetched (GET /guilds/{g}/member-verification); Submit asks the session to PUT it back and
// Discord's confirmation (the member's pending flag clearing) is what unlocks the composer.

bool ui_self_pending(const ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  if (ui->d.session) return ccord_session_self_pending(ui->d.session, guild_id);
  const ccord_member *me = ccord_store_find_member(ui->d.store, guild_id, ccord_store_self_id(ui->d.store));
  return me && me->pending;
}

static const ccord_member_verification *mv_form(const ccord_ui *ui)
{
  return ui->d.session ? ccord_session_member_verification(ui->d.session, ui->mv.guild_id) : nullptr;
}

static bool mv_field_is_terms(const ccord_member_verification_field *f)
{
  return f->field_type == CCORD_MEMBER_VERIFICATION_TERMS;
}

static size_t mv_field_count(const ccord_member_verification *form)
{
  if (!form) return 0;
  return form->field_count < CCORD_MEMBER_VERIFICATION_MAX_FIELDS ? form->field_count
                                                                   : CCORD_MEMBER_VERIFICATION_MAX_FIELDS;
}

// Every TERMS field is an acceptance of the rules, so each box must be ticked whatever its `required` flag says.
static bool mv_can_submit(const ccord_ui *ui, const ccord_member_verification *form)
{
  if (!form) return false;
  size_t n = mv_field_count(form);
  for (size_t i = 0; i < n; i++) {
    if (mv_field_is_terms(&form->fields[i]) && !ui->mv.agreed[i]) return false;
  }
  return true;
}

static void mv_close(ccord_ui *ui)
{
  if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
}

void ui_member_verification_open(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return;
  ui_member_verification *mv = &ui->mv;
  // Every opening starts unticked and at the top: agreeing to the rules is a fresh act each time.
  memset(mv->agreed, 0, sizeof mv->agreed);
  mv->version[0] = '\0';
  mv->focus = 0;
  mv->scroll = 0.0f;
  mv->guild_id = guild_id;
  ui->modal_kind = UI_MODAL_MEMBER_VERIFICATION;
  ui->dirty = true;
  if (ui->d.session && !ccord_session_member_verification(ui->d.session, guild_id)) {
    ccord_error err;
    if (ccord_session_member_verification_fetch(ui->d.session, guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't load the server rules: %s", err.message);
    }
  }
}

void ui_member_verification_sync(ccord_ui *ui)
{
  if (!ui || !ui->d.store) return;
  ui_member_verification *mv = &ui->mv;
  // Forget a guild once its screening is done, so joining it again (or a new form) opens the modal again.
  if (mv->auto_guild != CCORD_ID_NONE && !ui_self_pending(ui, mv->auto_guild)) mv->auto_guild = CCORD_ID_NONE;
  if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
    // Discord cleared the flag (or the guild is gone): there is nothing left to ask.
    if (!ui_self_pending(ui, mv->guild_id)) mv_close(ui);
    return;
  }
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID || g == mv->auto_guild) return;
  if (!ui_self_pending(ui, g)) return;   // unknown or not pending yet: look again on the next call
  if (ui->modal_kind != UI_MODAL_NONE) return;   // another dialog is up; ask once it is closed
  mv->auto_guild = g;
  ui_member_verification_open(ui, g);
}

static void mv_submit(ccord_ui *ui)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_member_verification_submit(ui->d.session, ui->mv.guild_id, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't submit the rules: %s", err.message);
    return;
  }
  // The composer stays locked (with its button) until Discord confirms and the pending flag clears.
  mv_close(ui);
  ui_notice(ui, "Rules submitted. Waiting for Discord to confirm.");
}

static void mv_draw_check(const ccord_ui *ui, ccord_renderer *r, ccord_rect b, bool on, bool focus)
{
  if (focus) ccord_draw_rrect(r, (ccord_rect){b.x - 2.0f, b.y - 2.0f, b.w + 4.0f, b.h + 4.0f}, 6.0f, COL(brand));
  ccord_draw_rrect(r, b, 4.0f, on ? COL(brand) : COL(text_muted));
  if (!on) ccord_draw_rrect(r, (ccord_rect){b.x + 2.0f, b.y + 2.0f, b.w - 4.0f, b.h - 4.0f}, 3.0f, COL(bg_input));
  if (on) {
    ccord_color c = COL(text_on_brand);
    ccord_draw_line(r, (ccord_vec2){b.x + b.w * 0.24f, b.y + b.h * 0.54f},
                    (ccord_vec2){b.x + b.w * 0.43f, b.y + b.h * 0.72f}, 2.0f, c);
    ccord_draw_line(r, (ccord_vec2){b.x + b.w * 0.43f, b.y + b.h * 0.72f},
                    (ccord_vec2){b.x + b.w * 0.78f, b.y + b.h * 0.30f}, 2.0f, c);
  }
}

// Lays out (and, when `draw`, paints) the form body inside `view`, scrolled by ui->mv.scroll. Returns the content height
// so the box can be sized to it and the scroll clamped. Checkbox rects are only recorded when drawing.
static float mv_body(ccord_ui *ui, ccord_renderer *r, const ccord_member_verification *form, ccord_rect view, bool draw)
{
  ui_member_verification *mv = &ui->mv;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  float top = view.y - (draw ? mv->scroll : 0.0f);
  float y = top;
  if (!form) {
    const char *msg = "Loading the server rules...";
    ccord_vec2 sz = ccord_text_measure(r, msg, strlen(msg), &ms, view.w);
    if (draw) ccord_text_draw(r, msg, strlen(msg), view.x, y, &ms, view.w);
    return sz.y + 8.0f;
  }
  if (form->description && form->description[0]) {
    size_t dl = strlen(form->description);
    ccord_vec2 sz = ccord_text_measure(r, form->description, dl, &ns, view.w);
    if (draw) ccord_text_draw(r, form->description, dl, view.x, y, &ns, view.w);
    y += sz.y + 16.0f;
  }
  size_t n = mv_field_count(form);
  for (size_t i = 0; i < n; i++) {
    const ccord_member_verification_field *f = &form->fields[i];
    const char *label = (f->label && f->label[0]) ? f->label : (mv_field_is_terms(f) ? "Rules" : "Question");
    ccord_vec2 lsz = ccord_text_measure(r, label, strlen(label), &hs, view.w);
    if (draw) ccord_text_draw(r, label, strlen(label), view.x, y, &hs, view.w);
    y += lsz.y + 8.0f;
    if (!mv_field_is_terms(f)) {
      // A free-text answer cannot be carried by the session's submit, so the question is shown, not asked.
      const char *note = "This question can't be answered in C-Cord yet.";
      ccord_vec2 nsz = ccord_text_measure(r, note, strlen(note), &ms, view.w);
      if (draw) ccord_text_draw(r, note, strlen(note), view.x, y, &ms, view.w);
      y += nsz.y + 16.0f;
      continue;
    }
    size_t nv = f->value_count < CCORD_MEMBER_VERIFICATION_MAX_VALUES ? f->value_count
                                                                      : CCORD_MEMBER_VERIFICATION_MAX_VALUES;
    const float num_w = 26.0f;
    for (size_t j = 0; j < nv; j++) {
      const char *rule = f->values[j] ? f->values[j] : "";
      char num[16];
      snprintf(num, sizeof num, "%zu.", j + 1u);
      ccord_vec2 rsz = ccord_text_measure(r, rule, strlen(rule), &ns, maxf(view.w - num_w, 40.0f));
      if (draw) {
        ccord_text_draw(r, num, strlen(num), view.x, y, &ms, 0.0f);
        ccord_text_draw(r, rule, strlen(rule), view.x + num_w, y, &ns, maxf(view.w - num_w, 40.0f));
      }
      y += rsz.y + 6.0f;
    }
    y += 6.0f;
    ccord_rect row = {view.x, y, view.w, 24.0f};
    if (draw) {
      mv->checks[i] = row;
      mv_draw_check(ui, r, (ccord_rect){row.x, row.y + 2.0f, 20.0f, 20.0f}, mv->agreed[i], i == mv->focus);
      const char *agree = "I have read and agree to the rules";
      ccord_text_style as = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
      ccord_text_draw_ellipsis(r, agree, strlen(agree), row.x + 30.0f, row.y + (row.h - ui_line_h(ui, as.size)) * 0.5f,
                               maxf(row.w - 30.0f, 20.0f), &as);
    }
    y += row.h + 16.0f;
  }
  return y - top;
}

void ui_member_verification_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_member_verification *mv = &ui->mv;
  const ccord_member_verification *form = mv_form(ui);
  // Ticks belong to one version of the rules: a changed form starts unticked.
  if (form && form->version && strcmp(form->version, mv->version) != 0) {
    memset(mv->agreed, 0, sizeof mv->agreed);
    snprintf(mv->version, sizeof mv->version, "%s", form->version);
  }
  ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
  ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));
  float mw = clampf(ui->win_w - 64.0f, 320.0f, 520.0f);
  float inner_w = mw - 40.0f;
  const float head_h = 66.0f, foot_h = 62.0f;
  float content_h = mv_body(ui, r, form, (ccord_rect){0, 0, inner_w, 0}, false);
  float mh = clampf(head_h + content_h + foot_h, 220.0f, maxf(ui->win_h - 48.0f, 220.0f));
  ccord_rect m = {((float)ui->win_w - mw) * 0.5f, maxf(((float)ui->win_h - mh) * 0.42f, 12.0f), mw, mh};
  ccord_rect view = {m.x + 20.0f, m.y + head_h, inner_w, m.h - head_h - foot_h};
  mv->box = m;
  mv->view = view;
  mv->content_h = content_h;
  mv->scroll = clampf(mv->scroll, 0.0f, maxf(content_h - view.h, 0.0f));
  ccord_draw_rrect(r, (ccord_rect){m.x - 1.0f, m.y - 1.0f, m.w + 2.0f, m.h + 2.0f}, 9.0f, COL(border_subtle));
  ccord_draw_rrect(r, m, 8.0f, COL(bg_secondary));

  const ccord_guild *gd = ccord_store_guild(ui->d.store, mv->guild_id);
  char title[160];
  if (gd && gd->name && gd->name[0]) snprintf(title, sizeof title, "Welcome to %s", gd->name);
  else snprintf(title, sizeof title, "Membership Screening");
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw_ellipsis(r, title, strlen(title), m.x + 20.0f, m.y + 16.0f, m.w - 40.0f, &ts);
  if (!form || !form->description || !form->description[0]) {   // the form's own description already says it
    const char *sub = "Agree to the rules to start chatting.";
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    ccord_text_draw_ellipsis(r, sub, strlen(sub), m.x + 20.0f, m.y + 16.0f + ui_line_h(ui, ts.size) + 2.0f, m.w - 40.0f,
                             &ss);
  }

  memset(mv->checks, 0, sizeof mv->checks);
  ccord_push_clip(r, view);
  (void)mv_body(ui, r, form, view, true);
  ccord_pop_clip(r);
  if (content_h > view.h) {   // a thin scrollbar when the rules overflow
    float th = maxf(view.h * view.h / content_h, 24.0f);
    float ty = view.y + (view.h - th) * (mv->scroll / (content_h - view.h));
    ccord_draw_rrect(r, (ccord_rect){view.x + view.w + 6.0f, ty, 4.0f, th}, 2.0f, COL(text_muted));
  }

  mv->submit = (ccord_rect){m.x + m.w - 160.0f, m.y + m.h - 46.0f, 140.0f, 32.0f};
  mv->cancel = (ccord_rect){mv->submit.x - 100.0f, mv->submit.y, 90.0f, 32.0f};
  mv->submit_enabled = mv_can_submit(ui, form);
  bool chv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mv->cancel);
  ccord_draw_rrect(r, mv->cancel, 4.0f, chv ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_secondary));
  ccord_vec2 cm = ccord_text_measure(r, "Not now", 7, &cs, 0.0f);
  ccord_text_draw(r, "Not now", 7, mv->cancel.x + (mv->cancel.w - cm.x) * 0.5f, mv->cancel.y + (mv->cancel.h - cm.y) * 0.5f,
                  &cs, 0.0f);
  bool shv = mv->submit_enabled && ui->mouse_in && ui_in_rect(ui->mx, ui->my, mv->submit);
  ccord_color sbg = mv->submit_enabled ? (shv ? ui_mix(COL(brand), COL(text_on_brand), 0.12f) : COL(brand))
                                       : ui_alpha(COL(brand), 0.4f);
  ccord_draw_rrect(r, mv->submit, 4.0f, sbg);
  ccord_text_style ks = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small),
                                 mv->submit_enabled ? COL(text_on_brand) : ui_alpha(COL(text_on_brand), 0.6f));
  ccord_vec2 km = ccord_text_measure(r, "Submit", 6, &ks, 0.0f);
  ccord_text_draw(r, "Submit", 6, mv->submit.x + (mv->submit.w - km.x) * 0.5f, mv->submit.y + (mv->submit.h - km.y) * 0.5f,
                  &ks, 0.0f);
  ui_popup_end(r);
}

static void mv_toggle(ccord_ui *ui, size_t i)
{
  if (i >= CCORD_MEMBER_VERIFICATION_MAX_FIELDS) return;
  ui->mv.agreed[i] = !ui->mv.agreed[i];
  ui->mv.focus = i;
  ui->dirty = true;
}

// Moves the focus ring to the next/previous TERMS field (Tab, Up, Down).
static void mv_move_focus(ccord_ui *ui, int dir)
{
  const ccord_member_verification *form = mv_form(ui);
  size_t n = mv_field_count(form);
  if (n == 0) return;
  size_t cur = ui->mv.focus < n ? ui->mv.focus : 0;
  for (size_t step = 1; step <= n; step++) {
    size_t i = (cur + (dir > 0 ? step : n - step % n)) % n;
    if (mv_field_is_terms(&form->fields[i])) {
      ui->mv.focus = i;
      ui->dirty = true;
      return;
    }
  }
}

bool ui_member_verification_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || ui->modal_kind != UI_MODAL_MEMBER_VERIFICATION) return false;
  ui_member_verification *mv = &ui->mv;
  const ccord_member_verification *form = mv_form(ui);
  switch (ev->type) {
    case CCORD_IN_KEY_DOWN:
      if (ev->key == CCORD_KEY_ESCAPE) {
        mv_close(ui);
      } else if (ev->key == CCORD_KEY_RETURN) {
        if (mv_can_submit(ui, form)) mv_submit(ui);
      } else if (ev->key == CCORD_KEY_TAB) {
        mv_move_focus(ui, (ev->mods & CCORD_MOD_SHIFT) ? -1 : 1);
      } else if (ev->key == CCORD_KEY_DOWN) {
        mv_move_focus(ui, 1);
      } else if (ev->key == CCORD_KEY_UP) {
        mv_move_focus(ui, -1);
      }
      return true;
    case CCORD_IN_TEXT:
      if (ev->text[0] == ' ' && form && mv->focus < mv_field_count(form) &&
          mv_field_is_terms(&form->fields[mv->focus])) {
        mv_toggle(ui, mv->focus);
      }
      return true;
    case CCORD_IN_WHEEL:
      ccord_scroller_scroll(&mv->glide, &mv->scroll, -ev->wheel * 42.0f, maxf(mv->content_h - mv->view.h, 0.0f),
                            ui_wheel_direct(ui, ev));
      ui->dirty = true;
      return true;
    case CCORD_IN_MOUSE_DOWN:
      if (ev->button != 1) return true;
      if (mv->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, mv->box)) {
        mv_close(ui);
        return true;
      }
      if (mv->cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, mv->cancel)) {
        mv_close(ui);
        return true;
      }
      if (mv->submit.w > 0.0f && ui_in_rect(ev->x, ev->y, mv->submit)) {
        if (mv_can_submit(ui, form)) mv_submit(ui);   // a disabled button swallows the click
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, mv->view)) {
        size_t n = mv_field_count(form);
        for (size_t i = 0; i < n; i++) {
          if (mv->checks[i].w > 0.0f && ui_in_rect(ev->x, ev->y, mv->checks[i])) {
            mv_toggle(ui, i);
            return true;
          }
        }
      }
      return true;
    default:
      return true;
  }
}

// Forgets the hit rectangles of every action modal: only the one drawn this frame may be clicked.
static void modal_reset_hits(ccord_ui *ui)
{
  ui->inter_modal_box = (ccord_rect){0, 0, 0, 0};
  ui->inter_modal_ok = (ccord_rect){0, 0, 0, 0};
  ui->inter_modal_cancel = (ccord_rect){0, 0, 0, 0};
  ui->modal_tab_rects[0] = (ccord_rect){0, 0, 0, 0};
  ui->modal_tab_rects[1] = (ccord_rect){0, 0, 0, 0};
  ui->modal_preview_btn_rect = (ccord_rect){0, 0, 0, 0};
  ui->modal_ok_rect = (ccord_rect){0, 0, 0, 0};
  ui->modal_input_rect = (ccord_rect){0, 0, 0, 0};
  ui->modal_input2_rect = (ccord_rect){0, 0, 0, 0};
  ui->nnew_dm_friend_hits = 0;
  ui->profile_modal_box = (ccord_rect){0, 0, 0, 0};
  ui->profile_modal_close_rect = (ccord_rect){0, 0, 0, 0};
  ui->profile_modal_msg_rect = (ccord_rect){0, 0, 0, 0};
  memset(ui->profile_modal_tab_rects, 0, sizeof ui->profile_modal_tab_rects);
  ui->profile_modal_note_rect = (ccord_rect){0, 0, 0, 0};
  ui->profile_modal_note_save_rect = (ccord_rect){0, 0, 0, 0};
  ui->nprofile_modal_mutual_hits = 0;
  ui->nprofile_modal_conn_hits = 0;
  ui->mv.box = ui->mv.view = ui->mv.cancel = ui->mv.submit = (ccord_rect){0, 0, 0, 0};
  memset(ui->mv.checks, 0, sizeof ui->mv.checks);
  for (size_t fi = 0; fi < CCORD_RICH_MAX_COMPONENT_ROWS; fi++) {
    ui->inter_modal_field_rects[fi] = (ccord_rect){0, 0, 0, 0};
  }
}

void ui_draw_overlays(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;

  // M5 voice surfaces borrow the generic modal routing (UI_MODAL_EDIT_CHANNEL); intercept before the
  // legacy modal code so the prompt / DAVE / RTC dialogs own the screen while they are open.
  if (ui_call_prompt_active(ui)) {
    ui_call_prompt_draw(ui, r);
    return;
  }
  if (ui_dave_modal_active(ui)) {
    ui_dave_modal_draw(ui, r);
    return;
  }
  // Scheduled events / stage lifecycle prompts borrow their own modal kind; same interception.
  if (ui_scheduled_events_active(ui)) {
    ui_scheduled_events_draw(ui, r);
    return;
  }
  // The Forward channel picker owns the screen while its modal kind is set.
  if (ui_forward_picker_active(ui)) {
    ui_forward_picker_draw(ui, r);
    return;
  }
  // The Activities shelf owns the screen while its modal kind is set.
  if (ui_activity_shelf_active(ui)) {
    ui_activity_shelf_draw(ui, r);
    return;
  }

  // 1. Inline Image Lightbox
  if (!ui->lightbox_open) {
    if (ui->lightbox_urls) ui_close_lightbox(ui);
    ui->lightbox_img_rect = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_open_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_prev_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_next_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_close_btn = (ccord_rect){0, 0, 0, 0};
    ui->lightbox_zoom_btn = (ccord_rect){0, 0, 0, 0};
  }
  if (ui->lightbox_open && (ui->lightbox_url[0] || ui->lightbox_count > 0)) {
    if (ui->lightbox_count == 0 && ui->lightbox_url[0]) {
      const char *one = ui->lightbox_url;
      ui_open_lightbox(ui, &one, 1u, 0u);
    }
    if (ui->lightbox_index >= ui->lightbox_count && ui->lightbox_count > 0) {
      ui->lightbox_index = ui->lightbox_count - 1u;
    }
    if (ui->lightbox_urls && ui->lightbox_count > 0) {
      ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
    }
    if (ui->lightbox_zoom < 0.5f || ui->lightbox_zoom > 4.0f) ui->lightbox_zoom = 1.0f;

    ui_popup_begin(ui, r, "lightbox", 0, UI_MODAL_MS, 8.0f);
    ccord_draw_rect(r, (ccord_rect){0, 0, ui->win_w, ui->win_h}, COL(overlay));
    bool lb_vid = ui_url_path_ends(ui->lightbox_url, ".mp4") || ui_url_path_ends(ui->lightbox_url, ".webm") ||
                  ui_url_path_ends(ui->lightbox_url, ".mov") || ui_url_path_ends(ui->lightbox_url, ".m4v");
    (void)ui_ensure_media_ex(ui, r, ui->lightbox_url, 0, lb_vid);
    int tw = 0, th = 0;
    bool has_tex = ccord_render_has_texture(r, ui->lightbox_url, &tw, &th);
    float max_w = clampf(ui->win_w * 0.74f, 220.0f, 920.0f);
    float max_h = clampf(ui->win_h * 0.68f, 160.0f, 680.0f);
    float base_w = fminf(max_w, 480.0f), base_h = fminf(max_h, 320.0f);
    if (has_tex && tw > 4 && th > 4) {
      float scale = fminf(max_w / (float)tw, max_h / (float)th);
      if (scale > 2.0f) scale = 2.0f;
      base_w = maxf((float)tw * scale, 120.0f);
      base_h = maxf((float)th * scale, 90.0f);
    }
    float iw = base_w * ui->lightbox_zoom;
    float ih = base_h * ui->lightbox_zoom;
    ccord_rect img_r = {(ui->win_w - iw) * 0.5f, (ui->win_h - ih) * 0.5f - 14.0f, iw, ih};
    ui->lightbox_img_rect = img_r;
    ccord_draw_rrect(r, (ccord_rect){img_r.x - 4.0f, img_r.y - 4.0f, img_r.w + 8.0f, img_r.h + 8.0f}, 8.0f,
                     COL(bg_secondary));
    if (has_tex) {
      ccord_draw_texture_rounded(r, ui->lightbox_url, img_r, 6.0f, 255);
    } else {
      ui_icon_attachment(r, img_r.x + img_r.w * 0.5f, img_r.y + img_r.h * 0.5f, 32.0f, COL(text_secondary));
    }

    float bar_y = clampf(img_r.y + img_r.h + 10.0f, 16.0f, maxf(ui->win_h - 42.0f, 16.0f));
    float bar_x = clampf(img_r.x, 24.0f, maxf(ui->win_w - 280.0f, 24.0f));
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_link));
    const char *open_lbl = "Open in Browser";
    ccord_vec2 ot = ccord_text_measure(r, open_lbl, strlen(open_lbl), &ls, 0.0f);
    ui->lightbox_open_btn = (ccord_rect){bar_x, bar_y, ot.x + 20.0f, 28.0f};
    ccord_draw_rrect(r, ui->lightbox_open_btn, 5.0f, COL(bg_secondary));
    ccord_text_draw(r, open_lbl, strlen(open_lbl), bar_x + 10.0f, bar_y + (28.0f - ot.y) * 0.5f, &ls, 0.0f);

    char zbuf[24];
    snprintf(zbuf, sizeof zbuf, "%d%%", (int)lroundf(ui->lightbox_zoom * 100.0f));
    ccord_text_style zs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
    ccord_vec2 zt = ccord_text_measure(r, zbuf, strlen(zbuf), &zs, 0.0f);
    ui->lightbox_zoom_btn = (ccord_rect){ui->lightbox_open_btn.x + ui->lightbox_open_btn.w + 8.0f, bar_y,
                                         zt.x + 18.0f, 28.0f};
    ccord_draw_rrect(r, ui->lightbox_zoom_btn, 5.0f, COL(bg_secondary));
    ccord_text_draw(r, zbuf, strlen(zbuf), ui->lightbox_zoom_btn.x + 9.0f, bar_y + (28.0f - zt.y) * 0.5f, &zs, 0.0f);

    ui->lightbox_close_btn = (ccord_rect){ui->win_w - 48.0f, 16.0f, 32.0f, 32.0f};
    ccord_draw_rrect(r, ui->lightbox_close_btn, 16.0f, COL(bg_secondary));
    ui_icon_close(r, ui->lightbox_close_btn.x + 16.0f, ui->lightbox_close_btn.y + 16.0f, 14.0f, COL(text_header));

    if (ui->lightbox_count > 1u) {
      char cbuf[32];
      snprintf(cbuf, sizeof cbuf, "%zu / %zu", ui->lightbox_index + 1u, ui->lightbox_count);
      ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_secondary));
      ccord_vec2 ct = ccord_text_measure(r, cbuf, strlen(cbuf), &cs, 0.0f);
      ccord_rect cpill = {ui->lightbox_zoom_btn.x + ui->lightbox_zoom_btn.w + 8.0f, bar_y, ct.x + 18.0f, 28.0f};
      ccord_draw_rrect(r, cpill, 5.0f, COL(bg_secondary));
      ccord_text_draw(r, cbuf, strlen(cbuf), cpill.x + 9.0f, bar_y + (28.0f - ct.y) * 0.5f, &cs, 0.0f);

      ui->lightbox_prev_btn = (ccord_rect){20.0f, (ui->win_h - 44.0f) * 0.5f, 40.0f, 44.0f};
      ui->lightbox_next_btn = (ccord_rect){ui->win_w - 60.0f, (ui->win_h - 44.0f) * 0.5f, 40.0f, 44.0f};
      ccord_draw_rrect(r, ui->lightbox_prev_btn, 8.0f, COL(bg_secondary));
      ccord_draw_rrect(r, ui->lightbox_next_btn, 8.0f, COL(bg_secondary));
      ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_header));
      ccord_vec2 lt = ccord_text_measure(r, "<", 1, &as, 0.0f);
      ccord_vec2 rt = ccord_text_measure(r, ">", 1, &as, 0.0f);
      ccord_text_draw(r, "<", 1, ui->lightbox_prev_btn.x + (40.0f - lt.x) * 0.5f,
                      ui->lightbox_prev_btn.y + (44.0f - lt.y) * 0.5f, &as, 0.0f);
      ccord_text_draw(r, ">", 1, ui->lightbox_next_btn.x + (40.0f - rt.x) * 0.5f,
                      ui->lightbox_next_btn.y + (44.0f - rt.y) * 0.5f, &as, 0.0f);
    } else {
      ui->lightbox_prev_btn = (ccord_rect){0, 0, 0, 0};
      ui->lightbox_next_btn = (ccord_rect){0, 0, 0, 0};
    }
    ui_popup_end(r);
  }

  // 2. Quick Switcher (Ctrl+K)
  if (ui->quick_switcher_open) {
    populate_quick_switcher(ui);
    ui->quick_switcher_thumb = (ccord_rect){0, 0, 0, 0};
    ui_popup_begin(ui, r, "qswitch", 0, UI_MODAL_MS, 8.0f);
    ccord_rect m = ui_quick_switcher_rect(ui);
    uis_modal_frame(ui, r, m);
    ccord_rect inp = {m.x + UIS_SP_4, m.y + UIS_SP_4, m.w - 2.0f * UIS_SP_4, UIS_H_INPUT};
    (void)uis_input(ui, r, inp, &ui->quick_switcher_edit, "Where would you like to go? (@user, #channel, *server)",
                    (uis_state){.focused = true}, false, "qswin", 0);
    // The result list scrolls inside the dialog so every match stays reachable on short windows.
    ccord_rect lview = {m.x + 12.0f, inp.y + inp.h + 12.0f, m.w - 24.0f, m.y + m.h - 10.0f - (inp.y + inp.h + 12.0f)};
    if (lview.h < 0.0f) lview.h = 0.0f;
    float content_h = (float)ui->nquick_switcher_items * 30.0f;
    float max_s = maxf(content_h - lview.h, 0.0f);
    ui->quick_switcher_scroll = clampf(ui->quick_switcher_scroll, 0.0f, max_s);
    // Follow the selection only when it moved (keys, reopen), so a wheel offset is not snapped back each frame.
    if (ui->quick_switcher_sel != ui->quick_switcher_sel_seen && ui->quick_switcher_sel < ui->nquick_switcher_items) {
      float sel_top = (float)ui->quick_switcher_sel * 30.0f;
      float sel_bot = sel_top + 28.0f;
      ccord_scroller_cancel(&ui->quick_switcher_glide);   // the follow-scroll replaces a wheel glide (scroll.h)
      if (sel_top < ui->quick_switcher_scroll) ui->quick_switcher_scroll = sel_top;
      else if (sel_bot > ui->quick_switcher_scroll + lview.h)
        ui->quick_switcher_scroll = clampf(sel_bot - lview.h, 0.0f, max_s);
    }
    ui->quick_switcher_sel_seen = ui->quick_switcher_sel;
    ccord_push_clip(r, lview);
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    for (size_t i = 0; i < ui->nquick_switcher_items; i++) {
      auto it = &ui->quick_switcher_items[i];
      float ry = lview.y + (float)i * 30.0f - ui->quick_switcher_scroll;
      if (ry + 28.0f < lview.y || ry > lview.y + lview.h) {
        it->rect = (ccord_rect){0, 0, 0, 0};
        continue;
      }
      ccord_rect rr = {lview.x, ry, lview.w, 28.0f};
      it->rect = rr;
      uis_list_row_fill(ui, r, rr, (uis_state){.selected = i == ui->quick_switcher_sel}, "qsw", i);
      ccord_text_draw_ellipsis(r, it->label, strlen(it->label), rr.x + 10.0f, rr.y + 4.0f, rr.w - 140.0f, &ls);
      ccord_vec2 st = ccord_text_measure(r, it->sub, strlen(it->sub), &ss, 0.0f);
      ccord_text_draw_ellipsis(r, it->sub, strlen(it->sub), rr.x + rr.w - fminf(st.x, 120.0f) - 10.0f,
                               rr.y + 6.0f, 120.0f, &ss);
    }
    ccord_pop_clip(r);
    ui->quick_switcher_thumb = ui_draw_list_scrollbar(ui, r, lview, content_h, ui->quick_switcher_scroll,
                                                      "qswitchlist", 0);
    ui_popup_end(r);
  }

  // 2b. Message Component Select Menu Dropdown
  ui->select_dropdown_rect = (ccord_rect){0, 0, 0, 0};
  ui->select_confirm_btn = (ccord_rect){0, 0, 0, 0};
  if (ui->select_open && ui->select_target && ui->select_opts) {
    bool is_multi = ui->select_target->max_values > 1u;
    size_t nopt = ui->select_opt_count;
    float row_h = 34.0f;
    float list_h = (nopt > 0 ? (float)nopt : 1.0f) * row_h + 8.0f;
    float foot_h = is_multi ? 38.0f : 0.0f;
    float dw = clampf(maxf(ui->select_anchor_rect.w, 240.0f), 180.0f, maxf(ui->win_w - 24.0f, 180.0f));
    float dh = list_h + foot_h;
    float dx = clampf(ui->select_anchor_rect.x, 8.0f, maxf(ui->win_w - dw - 8.0f, 8.0f));
    float dy = ui->select_anchor_rect.y + ui->select_anchor_rect.h + 4.0f;
    if (dy + dh > ui->win_h - 8.0f && ui->select_anchor_rect.y - dh - 4.0f >= 8.0f) {
      dy = ui->select_anchor_rect.y - dh - 4.0f;
    }
    dy = clampf(dy, 8.0f, maxf(ui->win_h - dh - 8.0f, 8.0f));
    ccord_rect dbox = {dx, dy, dw, dh};
    ui->select_dropdown_rect = dbox;
    ui_popup_begin(ui, r, "seldd", ui->select_target->message_id ^ (uint64_t)ui->select_target->row_idx, UI_POPUP_MS, 4.0f);
    uis_popover_frame(ui, r, dbox);
    ccord_text_style lst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style dst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    float oy = dbox.y + 4.0f;
    if (nopt == 0) {
      ccord_text_draw(r, "No options available", 20, dbox.x + 12.0f, oy + 8.0f, &dst, 0.0f);
    }
    for (size_t i = 0; i < nopt; i++) {
      ccord_rect or_ = {dbox.x + 4.0f, oy, dbox.w - 8.0f, row_h - 2.0f};
      ui->select_opt_rects[i] = or_;
      bool ohov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, or_);
      uis_list_row_fill(ui, r, or_, (uis_state){.hover = ohov, .selected = ui->select_opts[i].selected}, "selopt", i);
      float tx = or_.x + 10.0f;
      if (is_multi) {
        ccord_rect cb = {or_.x + 8.0f, or_.y + (or_.h - 14.0f) * 0.5f, 14.0f, 14.0f};
        if (ui->select_opts[i].selected) {
          ccord_draw_rrect(r, cb, 3.0f, COL(brand));
          ccord_draw_rrect(r, (ccord_rect){cb.x + 4.0f, cb.y + 4.0f, 6.0f, 6.0f}, 1.5f, COL(text_on_brand));
        } else {
          ccord_draw_rrect(r, cb, 3.0f, COL(text_muted));
          ccord_draw_rrect(r, (ccord_rect){cb.x + 1.5f, cb.y + 1.5f, 11.0f, 11.0f}, 2.0f, COL(bg_secondary));
        }
        tx = or_.x + 28.0f;
      } else if (ui->select_opts[i].selected) {
        ccord_draw_rrect(r, (ccord_rect){or_.x + or_.w - 16.0f, or_.y + (or_.h - 8.0f) * 0.5f, 8.0f, 8.0f}, 4.0f, COL(brand));
      }
      if (ui->select_opts[i].desc[0]) {
        ccord_text_draw_ellipsis(r, ui->select_opts[i].label, strlen(ui->select_opts[i].label), tx, or_.y + 2.0f,
                                 maxf(or_.w - (tx - or_.x) - 24.0f, 16.0f), &lst);
        ccord_text_draw_ellipsis(r, ui->select_opts[i].desc, strlen(ui->select_opts[i].desc), tx, or_.y + 17.0f,
                                 maxf(or_.w - (tx - or_.x) - 24.0f, 16.0f), &dst);
      } else {
        float llh = ui_line_h(ui, lst.size);
        ccord_text_draw_ellipsis(r, ui->select_opts[i].label, strlen(ui->select_opts[i].label), tx,
                                 or_.y + (or_.h - llh) * 0.5f, maxf(or_.w - (tx - or_.x) - 24.0f, 16.0f), &lst);
      }
      oy += row_h;
    }
    if (is_multi) {
      float fy = dbox.y + list_h;
      uis_divider(ui, r, dbox.x + 6.0f, fy, dbox.w - 12.0f);
      ccord_rect abtn = {dbox.x + dbox.w - 76.0f, fy + 5.0f, 68.0f, UIS_H_BTN_SM};
      ui->select_confirm_btn = abtn;
      uis_button(ui, r, abtn, "Apply", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
                 (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, abtn)}, "selapply", 0);
    }
    ui_popup_end(r);
  }

  // 3. Action Modals (Add Friend, New DM, Create/Edit Channel, Change Nickname, Create Thread, Confirm, Interaction, Add Server, Folder Settings)
  modal_reset_hits(ui);
  // The modal fades in and out as a whole (ui_overlay_begin); a closed one is drawn while it fades out with its kind
  // put back for that call only, so the handlers still see no modal and a click goes to what is underneath.
  float modal_t = ui_overlay_presence(ui, UI_OVL_MODAL, ui->modal_kind != UI_MODAL_NONE, UI_MODAL_MS,
                                      UI_MODAL_CLOSE_MS);
  if (ui->modal_kind != UI_MODAL_NONE) {
    // The dialog's own entrance (per kind, starts over after a frame it was not open) or the presence, whichever is
    // further along: reopened mid-exit it continues from where it is.
    ui->modal_ghost_kind = ui->modal_kind;
    float enter = ui_enter(ui, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS);
    if (enter > modal_t) modal_t = enter;
  }
  bool modal_fading = ui->modal_kind == UI_MODAL_NONE && ui->modal_ghost_kind != UI_MODAL_NONE &&
                      modal_t > UI_PRESENCE_EPS;
  if (modal_fading) ui->modal_kind = ui->modal_ghost_kind;
  bool modal_drawn = ui->modal_kind != UI_MODAL_NONE;
  if (modal_drawn) ui_overlay_begin(ui, r, UI_OVL_MODAL, modal_t, 10.0f);
  if (ui->modal_kind == UI_MODAL_USER_PROFILE) {
    draw_user_profile_modal(ui, r);
  } else if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
    draw_group_dm_modal(ui, r, ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT);
  } else if (ui->modal_kind == UI_MODAL_EDIT_GROUP_DM) {
    draw_edit_group_dm_modal(ui, r);
  } else if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) {
    ui_member_verification_draw(ui, r);
  } else if (ui->modal_kind == UI_MODAL_INTERACTION && ui->inter_modal && ui->inter_modal_edits) {
    ui_popup_begin(ui, r, "imodal", 1u, UI_MODAL_MS, 10.0f);
    uis_tokens mtk = uis_tokens_of(ui);
    float mw = clampf(ui->win_w - 64.0f, 320.0f, 460.0f);
    float mh = 56.0f + 56.0f;
    for (size_t fi = 0; fi < ui->inter_modal->field_count; fi++) {
      mh += 22.0f + (ui->inter_modal->fields[fi].style == 2 ? 72.0f : 36.0f) + 14.0f;
    }
    ccord_rect m = uis_modal_box(ui, mw, mh);
    ui->inter_modal_box = m;
    uis_modal_frame(ui, r, m);
    const char *mtitle = ui->inter_modal->title[0] ? ui->inter_modal->title : "Form";
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    ccord_text_draw_ellipsis(r, mtitle, strlen(mtitle), m.x + 20.0f, m.y + 18.0f, m.w - 40.0f, &ts);
    float fy = m.y + 56.0f;
    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_secondary));
    ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(danger));
    for (size_t fi = 0; fi < ui->inter_modal->field_count; fi++) {
      const ccord_modal_field *fld = &ui->inter_modal->fields[fi];
      const char *flbl = fld->label[0] ? fld->label : "Input";
      float lw = ccord_text_draw_ellipsis(r, flbl, strlen(flbl), m.x + 20.0f, fy, m.w - 52.0f, &ls);
      if (fld->required) {
        ccord_text_draw(r, " *", 2, m.x + 20.0f + lw, fy, &rs, 0.0f);
      }
      fy += 22.0f;
      float fh = fld->style == 2 ? 72.0f : 36.0f;
      ccord_rect fbox = {m.x + 20.0f, fy, m.w - 40.0f, fh};
      ui->inter_modal_field_rects[fi] = fbox;
      bool focused = (fi == ui->inter_modal_focus);
      if (fld->style != 2) {
        (void)uis_input(ui, r, fbox, &ui->inter_modal_edits[fi], fld->placeholder, (uis_state){.focused = focused}, false,
                        "imodin", (uint64_t)fi);
      } else {
        if (focused) {
          ccord_draw_rrect(r, (ccord_rect){fbox.x - 1.0f, fbox.y - 1.0f, fbox.w + 2.0f, fbox.h + 2.0f}, mtk.r_control + 1.0f,
                           COL(brand));
        }
        ccord_draw_rrect(r, fbox, mtk.r_control, COL(bg_input));   // multi-line field: no shared helper yet
      }
      const char *fval = ccord_text_edit_cstr(&ui->inter_modal_edits[fi]);
      bool has_val = fval[0] != '\0';
      const char *fdisp = has_val ? fval : fld->placeholder;
      ccord_text_style vst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base),
                                      has_val ? COL(text_normal) : COL(text_placeholder));
      if (fld->style == 2) {
        ccord_push_clip(r, fbox);
        if (fdisp[0]) ccord_text_draw(r, fdisp, strlen(fdisp), fbox.x + 10.0f, fbox.y + 8.0f, &vst, maxf(fbox.w - 20.0f, 20.0f));
        ccord_pop_clip(r);
      }
      fy += fh + 14.0f;
    }
    ccord_rect ok_r = uis_modal_footer_button(m, 0, 84.0f);
    ccord_rect cn_r = {ok_r.x - UIS_SP_2 - 76.0f, ok_r.y, 76.0f, ok_r.h};
    ui->inter_modal_ok = ok_r;
    ui->inter_modal_cancel = cn_r;
    uis_button(ui, r, cn_r, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cn_r)}, "imodcn", 0);
    uis_button(ui, r, ok_r, "Submit", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ok_r)}, "imodok", 0);
    ui_popup_end(r);
  } else if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
    ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
    modal_geom mg = modal_layout(ui);
    ccord_rect m = mg.box;
    ui->modal_tab_rects[0] = mg.tabs[0];
    ui->modal_tab_rects[1] = mg.tabs[1];
    ui->modal_preview_btn_rect = mg.preview_btn;
    ui->modal_ok_rect = mg.ok;
    ui->modal_input_rect = mg.input;
    uis_modal_frame(ui, r, m);

    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    const char *mtitle = (ui->modal_type_sel == 0) ? "Join a Server" : "Create Your Server";
    ccord_text_draw(r, mtitle, strlen(mtitle), m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);

    static const char *const k_add_tabs[2] = {"Join a Server", "Create a Server"};
    for (int t = 0; t < 2; t++) {
      uis_tab(ui, r, mg.tabs[t], k_add_tabs[t],
              (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.tabs[t]), .selected = ui->modal_type_sel == t},
              "addsrvtab", (uint64_t)t);
    }

    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
    const char *flbl = (ui->modal_type_sel == 0) ? "INVITE LINK OR CODE" : "SERVER NAME";
    ccord_text_draw(r, flbl, strlen(flbl), m.x + 20.0f, m.y + 94.0f, &ls, 0.0f);

    ccord_rect inp = mg.input;
    (void)uis_input(ui, r, inp, &ui->modal_edit1,
                    (ui->modal_type_sel == 0) ? "https://discord.gg/linux-gaming" : "My New Server",
                    (uis_state){.focused = true}, false, "addsrvin", 0);

    if (ui->modal_type_sel == 0) {
      uis_button(ui, r, mg.preview_btn, "Preview", UIS_BTN_SECONDARY, UIS_BTN_S_MD,
                 (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.preview_btn)}, "addsrvprev", 0);

      bool prev_loading = false;
      const ccord_invite_preview *prev = ui->d.session
        ? ccord_session_invite_preview(ui->d.session, &prev_loading)
        : nullptr;
      ccord_rect pcard = {m.x + 20.0f, m.y + 162.0f, m.w - 40.0f, 92.0f};
      uis_plate(ui, r, pcard, uis_tokens_of(ui).r_surface, COL(bg_tertiary));
      if (prev && prev->guild_id != CCORD_ID_NONE) {
        ccord_rect pic = {pcard.x + 12.0f, pcard.y + 12.0f, 40.0f, 40.0f};
        if (prev->guild_icon_url[0] && ui_ensure_media(ui, r, prev->guild_icon_url)) {
          ccord_draw_texture_rounded(r, prev->guild_icon_url, pic, 10.0f, 255);
        } else {
          ccord_draw_rrect(r, pic, 10.0f, COL(brand));
          char init[8];
          ui_initials(prev->guild_name, init);
          ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_on_brand));
          ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
          ccord_text_draw(r, init, strlen(init), pic.x + (pic.w - it.x) * 0.5f, pic.y + (pic.h - it.y) * 0.5f, &ist, 0.0f);
        }
        ccord_text_style pns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
        ccord_text_draw_ellipsis(r, prev->guild_name, strlen(prev->guild_name), pic.x + 50.0f, pcard.y + 13.0f,
                                 pcard.w - 72.0f, &pns);
        char pstats[96];
        snprintf(pstats, sizeof pstats, "%u Online  •  %u Members",
                 (unsigned)prev->online_count, (unsigned)prev->member_count);
        ccord_text_style pms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
        ccord_text_draw_ellipsis(r, pstats, strlen(pstats), pic.x + 50.0f, pcard.y + 34.0f, pcard.w - 72.0f, &pms);
        const char *pdesc = prev->guild_description[0] ? prev->guild_description : prev->channel_name;
        if (pdesc[0]) {
          ccord_text_style pds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
          ccord_text_draw_ellipsis(r, pdesc, strlen(pdesc), pcard.x + 12.0f, pcard.y + 62.0f, pcard.w - 24.0f, &pds);
        }
      } else {
        const char *hint = prev_loading
          ? "Fetching invite preview..."
          : "Enter an invite code or discord.gg link and click Preview, or Join Server directly.";
        ccord_text_style hst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
        ccord_text_draw(r, hint, strlen(hint), pcard.x + 12.0f, pcard.y + 14.0f, &hst, pcard.w - 24.0f);
      }
    } else {
      const char *chint = "By creating a server, you get text and voice channels ready to invite friends.";
      ccord_text_style hst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
      ccord_text_draw(r, chint, strlen(chint), m.x + 20.0f, m.y + 160.0f, &hst, m.w - 40.0f);
    }

    uis_button(ui, r, mg.cancel, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.cancel)}, "modcancel", 0);
    uis_button(ui, r, mg.ok, (ui->modal_type_sel == 0) ? "Join Server" : "Create Server", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.ok)}, "modok", 0);
    ui_popup_end(r);
  } else if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
    ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
    modal_geom mg = modal_layout(ui);
    ccord_rect m = mg.box;
    ui->modal_ok_rect = mg.ok;
    ui->modal_input_rect = mg.input;
    ui->modal_input2_rect = mg.input2;
    uis_modal_frame(ui, r, m);

    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    ccord_text_draw(r, "Folder Settings", 15, m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);

    ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_draw(r, "FOLDER NAME", 11, m.x + 20.0f, m.y + 54.0f, &ls, 0.0f);
    ccord_text_draw(r, "FOLDER COLOR (#RRGGBB)", 22, m.x + 20.0f, m.y + 122.0f, &ls, 0.0f);

    ccord_rect boxes[2] = {mg.input, mg.input2};
    ccord_text_edit *edits[2] = {&ui->modal_edit1, &ui->modal_edit2};
    const char *phs[2] = {"Server Folder", "#5865F2"};
    for (int fi = 0; fi < 2; fi++) {
      (void)uis_input(ui, r, boxes[fi], edits[fi], phs[fi], (uis_state){.focused = ui->modal_focus_field == fi}, false,
                      "foldin", (uint64_t)fi);
    }

    for (size_t i = 0; i < mg.nchips; i++) {
      uis_state cst = {.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.chip[i]), .selected = (int)i == ui->modal_type_sel};
      if (i == 0) {
        uis_tab(ui, r, mg.chip[i], "Auto", cst, "foldcol", i);
      } else {
        ccord_rect sw = {mg.chip[i].x + (mg.chip[i].w - UIS_SWATCH_D) * 0.5f, mg.chip[i].y + (mg.chip[i].h - UIS_SWATCH_D) * 0.5f,
                         UIS_SWATCH_D, UIS_SWATCH_D};
        uis_swatch(ui, r, sw, ui_rgb_color(k_folder_color_presets[i]), cst, "foldcol", i);
      }
    }

    uis_button(ui, r, mg.cancel, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.cancel)}, "modcancel", 0);
    uis_button(ui, r, mg.ok, "Save", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.ok)}, "modok", 0);
    ui_popup_end(r);
  } else if (ui->modal_kind != UI_MODAL_NONE) {
    ui_popup_begin(ui, r, "modal", (uint64_t)ui->modal_kind, UI_MODAL_MS, 10.0f);
    modal_geom mg = modal_layout(ui);
    ui->modal_ok_rect = mg.ok;
    ui->modal_input_rect = mg.input;
    ccord_rect m = mg.box;
    uis_modal_frame(ui, r, m);
    const char *title = "Modal";
    const char *placeholder = "Enter value...";
    const char *btn_lbl = "Save";
    char title_buf[CCORD_NAME_MAX_BYTES + 24];
    char body_buf[CCORD_NAME_MAX_BYTES + 160];
    const char *body = nullptr;
    const char *choices_label = nullptr;
    switch (ui->modal_kind) {
      case UI_MODAL_ADD_FRIEND:
        title = "Add Friend";
        placeholder = "You can add friends with their Discord username or ID";
        btn_lbl = "Send Friend Request";
        break;
      case UI_MODAL_NEW_DM:
        title = "New Direct Message";
        placeholder = "Enter username or User ID to message";
        btn_lbl = "Create DM";
        break;
      case UI_MODAL_CREATE_CHANNEL:
        title = "Create Channel";
        placeholder = "new-channel";
        btn_lbl = "Create Channel";
        break;
      case UI_MODAL_EDIT_CHANNEL:
        title = "Edit Channel";
        placeholder = "channel-name";
        btn_lbl = "Save Changes";
        break;
      case UI_MODAL_CHANGE_NICK:
        title = "Change Server Nickname";
        placeholder = "Enter a new nickname";
        btn_lbl = "Save";
        break;
      case UI_MODAL_USER_NOTE:
        title = "Add Note";
        placeholder = "Click to add a note (only visible to you)";
        btn_lbl = "Save";
        break;
      case UI_MODAL_FRIEND_NICK:
        title = "Friend Nickname";
        placeholder = "Enter a nickname for this friend";
        btn_lbl = "Save";
        break;
      case UI_MODAL_CREATE_THREAD:
        title = "Create Thread";
        placeholder = "Thread Name";
        btn_lbl = "Create Thread";
        break;
      case UI_MODAL_CONFIRM: {
        const char *who = ui->confirm_name[0] ? ui->confirm_name : "this";
        placeholder = "Reason (shows in the audit log, optional)";
        switch (ui->confirm_op) {
          case CCORD_ADMIN_DELETE_CHANNEL:
            title = "Delete Channel";
            btn_lbl = "Delete Channel";
            (void)snprintf(body_buf, sizeof body_buf, "Are you sure you want to delete %s? This cannot be undone.", who);
            break;
          case CCORD_ADMIN_DELETE_ROLE:
            title = "Delete Role";
            btn_lbl = "Delete Role";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete the role %s? Members lose it and the permissions it gave them.", who);
            break;
          case CCORD_ADMIN_REVOKE_INVITE:
            title = "Revoke Invite";
            btn_lbl = "Revoke Invite";
            {
              // Streamer Mode: `who` is the invite code; mask it with Hide Instant Invite Links.
              char who_mask[sizeof ui->confirm_name];
              const char *shown_who = ui->confirm_name[0] ? ui_streamer_mask_code(ui, who, who_mask, sizeof who_mask, true) : who;
              (void)snprintf(body_buf, sizeof body_buf, "Are you sure you want to revoke the invite %s? Its link will stop working.",
                             shown_who);
            }
            break;
          case CCORD_ADMIN_KICK:
            (void)snprintf(title_buf, sizeof title_buf, "Kick %s", who);
            title = title_buf;
            btn_lbl = "Kick";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to kick %s from the server? They can rejoin with a new invite.", who);
            break;
          case CCORD_ADMIN_BAN:
            (void)snprintf(title_buf, sizeof title_buf, "Ban %s", who);
            title = title_buf;
            btn_lbl = "Ban";
            choices_label = "DELETE MESSAGE HISTORY";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to ban %s? They can't rejoin until you unban them.", who);
            break;
          case CCORD_ADMIN_TIMEOUT:
            (void)snprintf(title_buf, sizeof title_buf, "Timeout %s", who);
            title = title_buf;
            btn_lbl = "Timeout";
            choices_label = "DURATION";
            (void)snprintf(body_buf, sizeof body_buf,
                           "%s won't be able to send messages, react, or speak in voice channels until it ends.", who);
            break;
          case CCORD_ADMIN_LEAVE_GUILD:
            (void)snprintf(title_buf, sizeof title_buf, "Leave '%s'", who);
            title = title_buf;
            btn_lbl = "Leave Server";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to leave %s? You won't be able to rejoin unless you are re-invited.", who);
            break;
          case CCORD_ADMIN_DELETE_GUILD:
            (void)snprintf(title_buf, sizeof title_buf, "Delete '%s'", who);
            title = title_buf;
            btn_lbl = "Delete Server";
            placeholder = "Enter server name to confirm";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete %s? This cannot be undone. Type the server name below to confirm.",
                           who);
            break;
          case CCORD_ADMIN_TRANSFER_OWNERSHIP: {
            const ccord_guild *tgd = ui->d.store ? ccord_store_guild(ui->d.store, ui->modal_guild_id) : nullptr;
            const char *gname = (tgd && tgd->name && tgd->name[0]) ? tgd->name : "this server";
            title = "Transfer Server Ownership";
            btn_lbl = "Transfer Ownership";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Transfer ownership of %s to %s? You will no longer be the server owner.", gname, who);
            break;
          }
          case CCORD_ADMIN_DELETE_EMOJI:
            title = "Delete Emoji";
            btn_lbl = "Delete Emoji";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete :%s:? This cannot be undone.", who);
            break;
          case CCORD_ADMIN_DELETE_STICKER:
            title = "Delete Sticker";
            btn_lbl = "Delete Sticker";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete the sticker %s? This cannot be undone.", who);
            break;
          case CCORD_ADMIN_DELETE_SOUNDBOARD:
            title = "Delete Sound";
            btn_lbl = "Delete Sound";
            (void)snprintf(body_buf, sizeof body_buf,
                           "Are you sure you want to delete the soundboard sound %s? This cannot be undone.", who);
            break;
          default:
            body_buf[0] = '\0';
            break;
        }
        body = body_buf;
        break;
      }
      default:
        break;
    }
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    ccord_text_draw(r, title, strlen(title), m.x + 20.0f, m.y + 18.0f, &ts, 0.0f);
    if (body) {
      ccord_text_style bodys = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
      ccord_text_draw(r, body, strlen(body), m.x + 20.0f, mg.body_y, &bodys, m.w - 40.0f);
    }
    if (choices_label && mg.nchips > 0) {
      ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_muted));
      ccord_text_draw(r, choices_label, strlen(choices_label), m.x + 20.0f, mg.choices_label_y, &ls, 0.0f);
      size_t nch = 0;
      const confirm_choice *choices = confirm_choices(ui->confirm_op, &nch);
      for (size_t i = 0; i < mg.nchips && i < nch; i++) {
        uis_tab(ui, r, mg.chip[i], choices[i].label,
                (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.chip[i]), .selected = (int)i == ui->modal_type_sel},
                "confchoice", (uint64_t)i);
      }
    }
    ccord_rect inp = mg.input;
    if (inp.w > 0.0f) {
      (void)uis_input(ui, r, inp, &ui->modal_edit1, placeholder, (uis_state){.focused = ui->modal_focus_field == 0}, false,
                      "modin", 0);
    }
    bool danger = ui->modal_kind == UI_MODAL_CONFIRM;
    uis_button(ui, r, mg.cancel, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.cancel)}, "modcancel", 0);
    uis_button(ui, r, mg.ok, btn_lbl, danger ? UIS_BTN_DANGER : UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, mg.ok)}, "modok", 0);
    ui_popup_end(r);
  }
  if (modal_drawn) ui_overlay_end(ui, r);
  if (modal_fading) {
    ui->modal_kind = UI_MODAL_NONE;
    modal_reset_hits(ui);
  } else if (ui->modal_kind != UI_MODAL_INTERACTION && ui->inter_modal) {
    ui_free_inter_modal(ui);
  }
  if (!ui->select_open && (ui->select_opts || ui->select_target)) {
    ui_close_select_dropdown(ui);
  }

  // M5: DAVE badge / RTC button by the voice bar, then the ring banner & DM call bar on top.
  ui_dave_draw_badge(ui, r);
  ui_call_draw(ui, r);
}

// Builds and sends the call the dialog was asking about. `reason` is the optional audit-log note.
static void confirm_run(ccord_ui *ui, const char *reason)
{
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = CCORD_ERR_INVALID;
  size_t n = 0;
  const confirm_choice *choices = confirm_choices(ui->confirm_op, &n);
  int64_t pick = choices && ui->modal_type_sel >= 0 && (size_t)ui->modal_type_sel < n ? choices[ui->modal_type_sel].value : 0;
  switch (ui->confirm_op) {
    case CCORD_ADMIN_DELETE_CHANNEL:
      st = ccord_admin_delete_channel(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_ROLE:
      st = ccord_admin_delete_role(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_REVOKE_INVITE:
      st = ccord_admin_revoke_invite(&req, ui->modal_guild_id, ui->confirm_name, reason, &err);
      break;
    case CCORD_ADMIN_KICK:
      st = ccord_admin_kick(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_BAN:
      st = ccord_admin_ban(&req, ui->modal_guild_id, ui->modal_target_id, (int32_t)pick, reason, &err);
      break;
    case CCORD_ADMIN_TIMEOUT:
      st = ccord_admin_timeout(&req, ui->modal_guild_id, ui->modal_target_id, ui->now_unix_ms, pick, reason, &err);
      break;
    case CCORD_ADMIN_LEAVE_GUILD:
      st = ccord_admin_leave_guild(&req, ui->modal_guild_id, &err);
      break;
    case CCORD_ADMIN_DELETE_GUILD:
      st = ccord_admin_delete_guild(&req, ui->modal_guild_id, &err);
      break;
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
      st = ccord_admin_transfer_ownership(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_EMOJI:
      st = ccord_admin_delete_emoji(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_STICKER:
      st = ccord_admin_delete_sticker(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    case CCORD_ADMIN_DELETE_SOUNDBOARD:
      st = ccord_admin_delete_soundboard(&req, ui->modal_guild_id, ui->modal_target_id, reason, &err);
      break;
    default:
      return;
  }
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", ccord_admin_op_verb(ui->confirm_op), err.message);
    return;
  }
  ui_admin_submit(ui, &req);
}

static void submit_modal(ccord_ui *ui)
{
  if (!ui || ui->modal_kind == UI_MODAL_NONE) return;
  const char *val = ccord_text_edit_cstr(&ui->modal_edit1);
  size_t vlen = ui->modal_edit1.len;
  char name_buf[128];
  snprintf(name_buf, sizeof name_buf, "%s", val ? val : "");
  ui_modal_kind kind = ui->modal_kind;
  ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
  if (kind == UI_MODAL_USER_PROFILE) {
    ui->modal_kind = UI_MODAL_USER_PROFILE;
    save_profile_modal_note(ui);
    return;
  }
  if (kind == UI_MODAL_EDIT_GROUP_DM) {
    ccord_admin_request req;
    ccord_error err;
    ccord_admin_group_dm_edit edit = {
      .name = name_buf,
      .icon_data_uri = (ui->modal_type_sel == 1) ? "" : nullptr,
    };
    if (ccord_admin_edit_group_dm(&req, ui->modal_target_id, &edit, &err) != CCORD_OK) {
      ui->modal_kind = UI_MODAL_EDIT_GROUP_DM;
      ui_notice(ui, "Couldn't edit Group DM: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (kind == UI_MODAL_NEW_DM || kind == UI_MODAL_ADD_DM_RECIPIENT) {
    ccord_id recips[CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX];
    size_t nrec = 0;
    ccord_id self_id = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
    for (size_t i = 0; i < ui->new_dm_recipient_count && nrec < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX; i++) {
      ccord_id uid = ui->new_dm_recipients[i];
      if (uid != CCORD_ID_NONE && uid != self_id) recips[nrec++] = uid;
    }
    if (vlen > 0) {
      const char *p = val;
      while (*p) {
        const char *comma = strchr(p, ',');
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        ccord_id uid = resolve_user_token(ui, p, tlen);
        if (uid == CCORD_ID_NONE && tlen > 0) {
          ui_notice(ui, "Could not find user \"%.*s\"", (int)tlen, p);
          return;
        }
        if (uid != CCORD_ID_NONE && uid != self_id && nrec < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
          bool dup = false;
          for (size_t k = 0; k < nrec; k++) {
            if (recips[k] == uid) { dup = true; break; }
          }
          if (!dup) recips[nrec++] = uid;
        }
        if (!comma) break;
        p = comma + 1;
      }
    }
    ui->new_dm_recipient_count = 0;
    if (nrec == 0) return;
    if (kind == UI_MODAL_ADD_DM_RECIPIENT) {
      for (size_t i = 0; i < nrec; i++) {
        ccord_admin_request req;
        ccord_error err;
        if (ccord_admin_add_dm_recipient(&req, ui->modal_target_id, recips[i], &err) == CCORD_OK) {
          ui_admin_submit(ui, &req);
        } else {
          ui_notice(ui, "Couldn't add member: %s", err.message);
        }
      }
      return;
    }
    const char *gname = ccord_text_edit_cstr(&ui->modal_edit2);
    if (nrec == 1u && gname[0] == '\0') {
      ui_open_dm_with_user(ui, recips[0]);
      return;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_create_group_dm(&req, recips, nrec, gname[0] ? gname : nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't create Group DM: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (kind == UI_MODAL_CONFIRM) {   // the text field is an optional audit-log reason (or exact name for delete-guild)
    if (ui->confirm_op == CCORD_ADMIN_DELETE_GUILD && strcmp(name_buf, ui->confirm_name) != 0) {
      ui->modal_kind = UI_MODAL_CONFIRM;
      ui_notice(ui, "Server name did not match \"%s\".", ui->confirm_name);
      return;
    }
    confirm_run(ui, name_buf);
    return;
  }
  if (kind == UI_MODAL_FOLDER_SETTINGS) {
    ccord_id fid = ui->modal_target_id;
    if (fid == CCORD_ID_NONE || !ui->d.store) return;
    uint32_t color = 0u;
    if (!parse_folder_hex_color(ccord_text_edit_cstr(&ui->modal_edit2), &color)) {
      ui->modal_kind = UI_MODAL_FOLDER_SETTINGS;
      ui->modal_focus_field = 1;
      ui_notice(ui, "Folder color must be #RRGGBB");
      return;
    }
    ccord_guild_folder_item items[CCORD_MAX_GUILDS];
    size_t ng = ccord_store_guild_count(ui->d.store);
    if (ng > CCORD_MAX_GUILDS) ng = CCORD_MAX_GUILDS;
    size_t count = 0;
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *gd = ccord_store_guild_at(ui->d.store, gi);
      if (!gd) continue;
      ccord_guild_folder_item it_f = {.guild_id = gd->id};
      if (gd->folder_id == fid) {
        it_f.folder_id = fid;
        it_f.folder_color = color;
        snprintf(it_f.folder_name, sizeof it_f.folder_name, "%.63s", name_buf);
      } else {
        it_f.folder_id = gd->folder_id;
        it_f.folder_color = gd->folder_color;
        snprintf(it_f.folder_name, sizeof it_f.folder_name, "%s", gd->folder_name);
      }
      items[count++] = it_f;
    }
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_set_guild_folders(&req, items, count, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't save folder settings: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
  if (kind == UI_MODAL_ADD_SERVER) {
    ccord_admin_request req;
    ccord_error err;
    if (ui->modal_type_sel == 0) {
      if (ccord_admin_accept_invite(&req, name_buf, &err) != CCORD_OK) {
        ui->modal_kind = UI_MODAL_ADD_SERVER;
        ui_notice(ui, "Couldn't join server: %s", err.message);
        return;
      }
      if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
      ui->discovery_open = false;
      ui_compute_layout(ui);
      ui_admin_submit(ui, &req);
      return;
    }
    if (ccord_admin_create_guild(&req, name_buf, &err) != CCORD_OK) {
      ui->modal_kind = UI_MODAL_ADD_SERVER;
      ui_notice(ui, "Couldn't create server: %s", err.message);
      return;
    }
    ui->discovery_open = false;
    ui_compute_layout(ui);
    ui_admin_submit(ui, &req);
    return;
  }
  if (vlen == 0 && kind != UI_MODAL_CHANGE_NICK && kind != UI_MODAL_USER_NOTE && kind != UI_MODAL_FRIEND_NICK) return;

  if (kind == UI_MODAL_FRIEND_NICK) {
    ui_set_friend_nickname(ui, ui->modal_target_id, val ? val : "");
    return;
  }

  if (kind == UI_MODAL_ADD_FRIEND) {
    ccord_id uid = resolve_user_token(ui, val, vlen);
    if (uid == CCORD_ID_NONE) {
      ui_notice(ui, "Could not find user \"%s\"", val);
      return;
    }
    ui_set_user_relationship(ui, uid, CCORD_RELATIONSHIP_FRIEND);
    return;
  }

  // Everything below asks Discord. Nothing is applied locally: the change shows up when Discord confirms it (the
  // gateway event, or the REST answer), and a refusal becomes a notice that carries Discord's own reason.
  ccord_error err;
  ccord_admin_request req;
  ccord_id sel_g = ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE;
  ccord_id gid = ui->modal_guild_id != CCORD_ID_NONE ? ui->modal_guild_id : sel_g;

  if (kind == UI_MODAL_CREATE_CHANNEL) {
    uint8_t type = ui->modal_type_sel == 1 ? 2u : 0u;
    if (ccord_admin_create_channel(&req, gid, name_buf, type, ui->modal_target_id, nullptr, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't create the channel: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }

  if (kind == UI_MODAL_EDIT_CHANNEL) {
    const ccord_channel *existing = ccord_store_channel(ui->d.store, ui->modal_target_id);
    if (!existing) return;
    if (existing->name && strcmp(existing->name, name_buf) == 0) return;   // nothing changed: no request
    ccord_admin_channel_edit edit = CCORD_ADMIN_CHANNEL_EDIT_INIT;
    edit.name = name_buf;
    if (ccord_admin_edit_channel(&req, existing->guild_id, existing->id, &edit, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't edit the channel: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }

  if (kind == UI_MODAL_CHANGE_NICK) {
    ccord_id self = ccord_store_self_id(ui->d.store);
    ccord_id uid = ui->modal_target_id != CCORD_ID_NONE ? ui->modal_target_id : self;
    const ccord_member *existing = ccord_store_find_member(ui->d.store, gid, uid);
    if (existing && strcmp(existing->nick, name_buf) == 0) return;
    if (ccord_admin_set_nick(&req, gid, uid, self, name_buf, nullptr, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't change the nickname: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }

  if (kind == UI_MODAL_USER_NOTE) {
    if (vlen > CCORD_USER_NOTE_MAX_BYTES) {
      ui_notice(ui, "Note is too long (max %d characters)", CCORD_USER_NOTE_MAX_BYTES);
      return;
    }
    if (ccord_admin_set_user_note(&req, ui->modal_target_id, val ? val : "", &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't save the note: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    ui_notice(ui, "Note saved");
    return;
  }

  if (kind == UI_MODAL_CREATE_THREAD) {
    ccord_id parent = ui->modal_target_id != CCORD_ID_NONE ? ui->modal_target_id : ccord_store_selected_channel(ui->d.store);
    ccord_id msg_id = ui->modal_msg_id;
    ui->modal_msg_id = CCORD_ID_NONE;
    ccord_status st = (msg_id != CCORD_ID_NONE)
                        ? ccord_admin_create_thread_from_message(&req, gid, parent, msg_id, name_buf, &err)
                        : ccord_admin_create_thread(&req, gid, parent, name_buf, &err);
    if (st != CCORD_OK) {
      ui_notice(ui, "Couldn't create the thread: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    return;
  }
}

static void submit_select_dropdown(ccord_ui *ui, const char *const *vals, size_t nvals)
{
  if (!ui || !ui->select_target) return;
  uint8_t min_v = ui->select_target->min_values;
  uint8_t max_v = ui->select_target->max_values > 0 ? ui->select_target->max_values : 1u;
  if (nvals < (size_t)min_v) {
    ui_notice(ui, "Select at least %u option%s", (unsigned)min_v, min_v == 1u ? "" : "s");
    return;
  }
  if (nvals > (size_t)max_v) {
    ui_notice(ui, "You can select at most %u option%s", (unsigned)max_v, max_v == 1u ? "" : "s");
    return;
  }
  ui_select_target target = *ui->select_target;
  ui->select_open = false;
  ui->dirty = true;
  if (!ui->d.session) {
    ui_close_select_dropdown(ui);
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_interaction_request req = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = target.application_id,
    .channel_id = target.channel_id,
    .message_id = target.message_id,
    .message_flags = target.message_flags,
    .component_type = (uint8_t)target.kind,
    .custom_id = target.custom_id,
    .values = vals,
    .value_count = nvals,
  };
  ccord_error err;
  if (ccord_session_send_interaction(ui->d.session, &req, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't submit selection: %s", err.message);
  }
  ui_close_select_dropdown(ui);
}

static void submit_interaction_modal(ccord_ui *ui)
{
  if (!ui || ui->modal_kind != UI_MODAL_INTERACTION || !ui->inter_modal || !ui->inter_modal_edits) return;
  for (size_t i = 0; i < ui->inter_modal->field_count; i++) {
    const ccord_modal_field *fld = &ui->inter_modal->fields[i];
    size_t len = ui->inter_modal_edits[i].len;
    const char *lbl = fld->label[0] ? fld->label : "Field";
    if (fld->required && len == 0) {
      ui->inter_modal_focus = i;
      ui_notice(ui, "%s is required", lbl);
      ui->dirty = true;
      return;
    }
    if (len > 0 && fld->min_length > 0 && len < (size_t)fld->min_length) {
      ui->inter_modal_focus = i;
      ui_notice(ui, "%s must be at least %u characters", lbl, (unsigned)fld->min_length);
      ui->dirty = true;
      return;
    }
    if (fld->max_length > 0 && len > (size_t)fld->max_length) {
      ui->inter_modal_focus = i;
      ui_notice(ui, "%s must be at most %u characters", lbl, (unsigned)fld->max_length);
      ui->dirty = true;
      return;
    }
  }
  const char *fids[CCORD_RICH_MAX_COMPONENT_ROWS];
  const char *fvals[CCORD_RICH_MAX_COMPONENT_ROWS];
  for (size_t i = 0; i < ui->inter_modal->field_count; i++) {
    fids[i] = ui->inter_modal->fields[i].custom_id;
    fvals[i] = ccord_text_edit_cstr(&ui->inter_modal_edits[i]);
  }
  ccord_interaction_request req = {
    .type = CCORD_INTERACTION_MODAL_SUBMIT,
    .application_id = ui->inter_modal->application_id,
    .guild_id = ui->inter_modal->guild_id,
    .channel_id = ui->inter_modal->channel_id != CCORD_ID_NONE
                    ? ui->inter_modal->channel_id
                    : ccord_store_selected_channel(ui->d.store),
    .custom_id = ui->inter_modal->custom_id,
    .modal_id = ui->inter_modal->id,
    .field_ids = fids,
    .field_values = fvals,
    .field_count = ui->inter_modal->field_count,
  };
  ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_send_interaction(ui->d.session, &req, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't submit form: %s", err.message);
  }
}

bool ui_overlays_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev) return false;

  // M5 voice surfaces own every event while their borrowed modal is open.
  if (ui_call_prompt_active(ui)) return ui_call_prompt_handle(ui, ev);
  if (ui_dave_modal_active(ui)) return ui_dave_modal_handle(ui, ev);
  if (ui_scheduled_events_active(ui)) return ui_scheduled_events_handle(ui, ev);
  if (ui_forward_picker_active(ui)) return ui_forward_picker_handle(ui, ev);
  if (ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION) return ui_member_verification_handle(ui, ev);
  if (ui_activity_shelf_active(ui)) return ui_activity_shelf_handle(ui, ev);

  if (ui->lightbox_open) {
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui_close_lightbox(ui);
        return true;
      }
      if (ev->key == CCORD_KEY_LEFT && ui->lightbox_urls && ui->lightbox_count > 1u) {
        ui->lightbox_index = (ui->lightbox_index + ui->lightbox_count - 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RIGHT && ui->lightbox_urls && ui->lightbox_count > 1u) {
        ui->lightbox_index = (ui->lightbox_index + 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_TEXT) {
      if (ev->text[0] == '+' || ev->text[0] == '=') {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom * 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      } else if (ev->text[0] == '-') {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom / 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      } else if (ev->text[0] == '0') {
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_WHEEL) {
      if (ev->wheel > 0.0f) {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom * 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      } else if (ev->wheel < 0.0f) {
        ui->lightbox_zoom = clampf(ui->lightbox_zoom / 1.25f, 0.5f, 4.0f);
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->lightbox_open_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_open_btn)) {
        (void)ui_activate_link(ui, ui->lightbox_url);
        return true;
      }
      if (ui->lightbox_zoom_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_zoom_btn)) {
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_prev_btn.w > 0.0f && ui->lightbox_urls && ui->lightbox_count > 1u &&
          ui_in_rect(ev->x, ev->y, ui->lightbox_prev_btn)) {
        ui->lightbox_index = (ui->lightbox_index + ui->lightbox_count - 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_next_btn.w > 0.0f && ui->lightbox_urls && ui->lightbox_count > 1u &&
          ui_in_rect(ev->x, ev->y, ui->lightbox_next_btn)) {
        ui->lightbox_index = (ui->lightbox_index + 1u) % ui->lightbox_count;
        ui_url_copy(ui->lightbox_url, sizeof ui->lightbox_url, ui->lightbox_urls[ui->lightbox_index]);
        ui->lightbox_zoom = 1.0f;
        ui->dirty = true;
        return true;
      }
      if (ui->lightbox_close_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_close_btn)) {
        ui_close_lightbox(ui);
        return true;
      }
      if (ui->lightbox_img_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->lightbox_img_rect)) {
        if (ev->clicks >= 2) {
          ui->lightbox_zoom = fabsf(ui->lightbox_zoom - 1.0f) < 0.05f ? 2.0f : 1.0f;
          ui->dirty = true;
        }
        return true;
      }
      ui_close_lightbox(ui);
      return true;
    }
    return true;
  }

  if (ui->quick_switcher_open) {
    populate_quick_switcher(ui);
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui->quick_switcher_open = false;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_DOWN && ui->nquick_switcher_items > 0) {
        ui->quick_switcher_sel = (ui->quick_switcher_sel + 1) % ui->nquick_switcher_items;
        ccord_scroller_cancel(&ui->quick_switcher_glide);   // the selection's follow-scroll must not fight a wheel glide
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_UP && ui->nquick_switcher_items > 0) {
        ui->quick_switcher_sel = (ui->quick_switcher_sel + ui->nquick_switcher_items - 1) % ui->nquick_switcher_items;
        ccord_scroller_cancel(&ui->quick_switcher_glide);   // the selection's follow-scroll must not fight a wheel glide
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN || ev->key == CCORD_KEY_TAB) {
        activate_quick_switcher_item(ui, ui->quick_switcher_sel);
        return true;
      }
      if (ev->key == CCORD_KEY_BACKSPACE) {
        ccord_text_edit_backspace(&ui->quick_switcher_edit);
        ui_quick_switcher_reset_list(ui);
        ui->dirty = true;
        return true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(&ui->quick_switcher_edit, ev->text, strlen(ev->text), &err);
      ui_quick_switcher_reset_list(ui);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_WHEEL) {
      ccord_rect m = ui_quick_switcher_rect(ui);
      ccord_rect lview = {m.x + 12.0f, m.y + 66.0f, m.w - 24.0f, m.h - 76.0f};
      float content_h = (float)ui->nquick_switcher_items * 30.0f;
      float max_s = maxf(content_h - lview.h, 0.0f);
      ccord_scroller_scroll(&ui->quick_switcher_glide, &ui->quick_switcher_scroll, -ev->wheel * 30.0f, max_s,
                            ui_wheel_direct(ui, ev));
      ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      ccord_rect m = ui_quick_switcher_rect(ui);
      if (!ui_in_rect(ev->x, ev->y, m)) {
        ui->quick_switcher_open = false;
        ui->dirty = true;
        return true;
      }
      for (size_t i = 0; i < ui->nquick_switcher_items; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->quick_switcher_items[i].rect)) {
          activate_quick_switcher_item(ui, i);
          return true;
        }
      }
      return true;
    }
    return true;
  }

  if (ui->select_open) {
    if (!ui->select_target || !ui->select_opts) {
      ui_close_select_dropdown(ui);
      return true;
    }
    bool is_multi = ui->select_target->max_values > 1u;
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui_close_select_dropdown(ui);
      } else if (ev->key == CCORD_KEY_RETURN && is_multi) {
        const char *vals[CCORD_RICH_MAX_SELECT_OPTIONS];
        size_t nvals = 0;
        for (size_t i = 0; i < ui->select_opt_count; i++) {
          if (ui->select_opts[i].selected) vals[nvals++] = ui->select_opts[i].value;
        }
        submit_select_dropdown(ui, vals, nvals);
      }
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->select_dropdown_rect.w <= 0.0f || !ui_in_rect(ev->x, ev->y, ui->select_dropdown_rect)) {
        ui_close_select_dropdown(ui);
        return true;
      }
      if (is_multi && ui->select_confirm_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->select_confirm_btn)) {
        const char *vals[CCORD_RICH_MAX_SELECT_OPTIONS];
        size_t nvals = 0;
        for (size_t i = 0; i < ui->select_opt_count; i++) {
          if (ui->select_opts[i].selected) vals[nvals++] = ui->select_opts[i].value;
        }
        submit_select_dropdown(ui, vals, nvals);
        return true;
      }
      for (size_t i = 0; i < ui->select_opt_count; i++) {
        if (ui_in_rect(ev->x, ev->y, ui->select_opt_rects[i])) {
          if (is_multi) {
            if (!ui->select_opts[i].selected) {
              size_t cur = 0;
              for (size_t k = 0; k < ui->select_opt_count; k++) {
                if (ui->select_opts[k].selected) cur++;
              }
              if (cur >= (size_t)ui->select_target->max_values) {
                ui_notice(ui, "You can select at most %u options", (unsigned)ui->select_target->max_values);
                return true;
              }
            }
            ui->select_opts[i].selected = !ui->select_opts[i].selected;
            ui->dirty = true;
          } else {
            if (ui->select_opts[i].selected && ui->select_target->min_values == 0u) {
              ui->select_opts[i].selected = false;
              submit_select_dropdown(ui, nullptr, 0);
            } else {
              for (size_t k = 0; k < ui->select_opt_count; k++) ui->select_opts[k].selected = (k == i);
              const char *v = ui->select_opts[i].value;
              submit_select_dropdown(ui, &v, 1u);
            }
          }
          return true;
        }
      }
      return true;
    }
    return true;
  }

  if (ui->modal_kind == UI_MODAL_INTERACTION) {
    if (!ui->inter_modal || !ui->inter_modal_edits) {
      ui->modal_kind = UI_MODAL_NONE;
      ui->dirty = true;
      return true;
    }
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    size_t nf = ui->inter_modal->field_count;
    size_t focus = nf > 0 && ui->inter_modal_focus < nf ? ui->inter_modal_focus : 0;
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_TAB && nf > 0) {
        ui->inter_modal_focus = shift ? (focus + nf - 1u) % nf : (focus + 1u) % nf;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        if (nf > 0 && ui->inter_modal->fields[focus].style == 2 && !ctrl) {
          ccord_error ierr;
          (void)ccord_text_edit_insert(&ui->inter_modal_edits[focus], "\n", 1u, &ierr);
          ui->dirty = true;
        } else {
          submit_interaction_modal(ui);
        }
        return true;
      }
      if (nf > 0) {
        ccord_text_edit *ed = &ui->inter_modal_edits[focus];
        switch (ev->key) {
          case CCORD_KEY_BACKSPACE:
            if (ctrl) ccord_text_edit_backspace_word(ed);
            else ccord_text_edit_backspace(ed);
            break;
          case CCORD_KEY_DELETE:
            if (ctrl) ccord_text_edit_delete_word(ed);
            else ccord_text_edit_delete(ed);
            break;
          case CCORD_KEY_LEFT: ccord_text_edit_move(ed, ctrl ? -3 : -1, shift); break;
          case CCORD_KEY_RIGHT: ccord_text_edit_move(ed, ctrl ? 3 : 1, shift); break;
          case CCORD_KEY_HOME: ccord_text_edit_move(ed, -2, shift); break;
          case CCORD_KEY_END: ccord_text_edit_move(ed, 2, shift); break;
          case CCORD_KEY_A: if (ctrl) ccord_text_edit_select_all(ed); break;
          case CCORD_KEY_V:
            if (ctrl && ui->d.host.clipboard_get) {
              char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
              if (clip) {
                ccord_error perr;
                (void)ccord_text_edit_insert(ed, clip, strlen(clip), &perr);
                ccord_free(clip);
              }
            }
            break;
          default: break;
        }
        ui->dirty = true;
      }
      return true;
    }
    if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0' && nf > 0) {
      ccord_error ierr;
      (void)ccord_text_edit_insert(&ui->inter_modal_edits[focus], ev->text, strlen(ev->text), &ierr);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->inter_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inter_modal_box)) {
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui->inter_modal_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_cancel)) {
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui->inter_modal_ok.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_ok)) {
        submit_interaction_modal(ui);
        return true;
      }
      for (size_t i = 0; i < nf; i++) {
        if (ui->inter_modal_field_rects[i].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_field_rects[i])) {
          ui->inter_modal_focus = i;
          ui->dirty = true;
          return true;
        }
      }
      return true;
    }
    return true;
  }

  if (ui->modal_kind != UI_MODAL_NONE) {
    bool two_field = (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS ||
                      ui->modal_kind == UI_MODAL_NEW_DM);
    ccord_text_edit *ed = (two_field && ui->modal_focus_field == 1)
                            ? &ui->modal_edit2
                            : &ui->modal_edit1;
    bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    if (ev->type == CCORD_IN_WHEEL && ui->modal_kind == UI_MODAL_USER_PROFILE) {
      float step = 42.0f;
      ccord_scroller_scroll(&ui->profile_modal_glide, &ui->profile_modal_scroll, -ev->wheel * step,
                            ui->profile_modal_max, ui_wheel_direct(ui, ev));
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_KEY_DOWN) {
      if (ev->key == CCORD_KEY_ESCAPE) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER && ui->d.session) {
          ccord_session_clear_invite_preview(ui->d.session);
        }
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_TAB && two_field) {
        ui->modal_focus_field = ui->modal_focus_field == 1 ? 0 : 1;
        ui->dirty = true;
        return true;
      }
      if (ev->key == CCORD_KEY_RETURN) {
        submit_modal(ui);
        return true;
      }
      if (ui->modal_kind == UI_MODAL_CONFIRM && (ev->key == CCORD_KEY_LEFT || ev->key == CCORD_KEY_RIGHT)) {
        size_t n = 0;
        (void)confirm_choices(ui->confirm_op, &n);
        if (n > 0) {
          int next = ui->modal_type_sel + (ev->key == CCORD_KEY_RIGHT ? 1 : -1);
          if (next >= 0 && (size_t)next < n) ui->modal_type_sel = next;
          ui->dirty = true;
        }
        return true;
      }
      switch (ev->key) {
        case CCORD_KEY_BACKSPACE:
          if (ctrl) ccord_text_edit_backspace_word(ed);
          else ccord_text_edit_backspace(ed);
          break;
        case CCORD_KEY_DELETE:
          if (ctrl) ccord_text_edit_delete_word(ed);
          else ccord_text_edit_delete(ed);
          break;
        case CCORD_KEY_LEFT: ccord_text_edit_move(ed, ctrl ? -3 : -1, shift); break;
        case CCORD_KEY_RIGHT: ccord_text_edit_move(ed, ctrl ? 3 : 1, shift); break;
        case CCORD_KEY_HOME: ccord_text_edit_move(ed, -2, shift); break;
        case CCORD_KEY_END: ccord_text_edit_move(ed, 2, shift); break;
        case CCORD_KEY_A: if (ctrl) ccord_text_edit_select_all(ed); break;
        case CCORD_KEY_V:
          if (ctrl && ui->d.host.clipboard_get) {
            char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
            if (clip) {
              ccord_error perr;
              (void)ccord_text_edit_insert(ed, clip, strlen(clip), &perr);
              ccord_free(clip);
            }
          }
          break;
        default: break;
      }
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_TEXT && ev->text[0] != '\0') {
      ccord_error err;
      (void)ccord_text_edit_insert(ed, ev->text, strlen(ev->text), &err);
      ui->dirty = true;
      return true;
    }
    if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
      if (ui->modal_kind == UI_MODAL_USER_PROFILE) {
        if (ui->profile_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->profile_modal_box)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->profile_modal_close_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_close_rect)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->profile_modal_msg_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_msg_rect)) {
          ccord_id uid = ui->modal_target_id;
          ui->modal_kind = UI_MODAL_NONE;
          ui_open_dm_with_user(ui, uid);
          ui->dirty = true;
          return true;
        }
        for (int t = 0; t < 3; t++) {
          if (ui->profile_modal_tab_rects[t].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_tab_rects[t])) {
            if (ui->profile_modal_tab != t) {
              ui->profile_modal_tab = t;
              ui->profile_modal_scroll = 0.0f;
              ui->dirty = true;
            }
            return true;
          }
        }
        if (ui->profile_modal_tab == 0) {
          if (ui->profile_modal_note_save_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->profile_modal_note_save_rect)) {
            save_profile_modal_note(ui);
            return true;
          }
          for (size_t ci = 0; ci < ui->nprofile_modal_conn_hits; ci++) {
            if (ui_in_rect(ev->x, ev->y, ui->profile_modal_conn_hits[ci].rect) &&
                ui->profile_modal_conn_hits[ci].url[0] != '\0') {
              (void)ui_open_url(ui, ui->profile_modal_conn_hits[ci].url, "Connection");
              return true;
            }
          }
        } else if (ui->profile_modal_tab == 1) {
          for (size_t i = 0; i < ui->nprofile_modal_mutual_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->profile_modal_mutual_hits[i].rect)) {
              ccord_id mgid = ui->profile_modal_mutual_hits[i].id;
              ui->modal_kind = UI_MODAL_NONE;
              ui->discovery_open = false;
              ui->dm_friends_view = false;
              ui->dm_requests_view = false;
              ui->stage_channel = CCORD_ID_NONE;
              ui_compute_layout(ui);
              ccord_error s_err;
              if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, mgid, &s_err);
              else (void)ccord_store_select_guild(ui->d.store, mgid, &s_err);
              ui_ensure_caches(ui);
              ui_sync_messages(ui);
              ui_save_current_location(ui);
              ui->dirty = true;
              return true;
            }
          }
        } else if (ui->profile_modal_tab == 2) {
          for (size_t i = 0; i < ui->nprofile_modal_mutual_hits; i++) {
            if (ui_in_rect(ev->x, ev->y, ui->profile_modal_mutual_hits[i].rect)) {
              ccord_id fuid = ui->profile_modal_mutual_hits[i].id;
              ui->modal_kind = UI_MODAL_NONE;
              ui_open_dm_with_user(ui, fuid);
              ui->dirty = true;
              return true;
            }
          }
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_NEW_DM || ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT) {
        if (ui->inter_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inter_modal_box)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->inter_modal_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_cancel)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->modal_ok_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_ok_rect)) {
          submit_modal(ui);
          return true;
        }
        if (ui->modal_input_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_input_rect)) {
          ui->modal_focus_field = 0;
          ui->dirty = true;
          return true;
        }
        if (ui->modal_kind == UI_MODAL_NEW_DM && ui->modal_input2_rect.w > 0.0f &&
            ui_in_rect(ev->x, ev->y, ui->modal_input2_rect)) {
          ui->modal_focus_field = 1;
          ui->dirty = true;
          return true;
        }
        const ccord_channel *ech = (ui->modal_kind == UI_MODAL_ADD_DM_RECIPIENT)
                                     ? ccord_store_channel(ui->d.store, ui->modal_target_id)
                                     : nullptr;
        size_t existing = (ech && ech->recipient_count <= CCORD_MAX_DM_RECIPIENTS) ? ech->recipient_count : 0u;
        size_t max_add = existing < CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX ? (CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX - existing) : 0u;
        for (size_t i = 0; i < ui->nnew_dm_friend_hits; i++) {
          if (ui_in_rect(ev->x, ev->y, ui->new_dm_friend_hits[i].rect)) {
            new_dm_toggle_recipient(ui, ui->new_dm_friend_hits[i].user_id, max_add);
            return true;
          }
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_EDIT_GROUP_DM) {
        if (ui->inter_modal_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, ui->inter_modal_box)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->inter_modal_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->inter_modal_cancel)) {
          ui->modal_kind = UI_MODAL_NONE;
          ui->dirty = true;
          return true;
        }
        if (ui->modal_ok_rect.w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_ok_rect)) {
          submit_modal(ui);
          return true;
        }
        if (ui->modal_tab_rects[0].w > 0.0f && ui_in_rect(ev->x, ev->y, ui->modal_tab_rects[0])) {
          ui->modal_type_sel = (ui->modal_type_sel == 1) ? 0 : 1;
          ui->dirty = true;
          return true;
        }
        return true;
      }
      modal_geom mg = modal_layout(ui);
      if (!ui_in_rect(ev->x, ev->y, mg.box)) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER && ui->d.session) {
          ccord_session_clear_invite_preview(ui->d.session);
        }
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui_in_rect(ev->x, ev->y, mg.ok)) {
        submit_modal(ui);
        return true;
      }
      if (mg.cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, mg.cancel)) {
        if (ui->modal_kind == UI_MODAL_ADD_SERVER && ui->d.session) {
          ccord_session_clear_invite_preview(ui->d.session);
        }
        ui->modal_kind = UI_MODAL_NONE;
        ui->dirty = true;
        return true;
      }
      if (ui->modal_kind == UI_MODAL_ADD_SERVER) {
        for (int t = 0; t < 2; t++) {
          if (ui_in_rect(ev->x, ev->y, mg.tabs[t])) {
            if (ui->modal_type_sel != t) {
              ui->modal_type_sel = t;
              ccord_text_edit_clear(&ui->modal_edit1);
              if (ui->d.session) ccord_session_clear_invite_preview(ui->d.session);
              ui->dirty = true;
            }
            return true;
          }
        }
        if (ui->modal_type_sel == 0 && mg.preview_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, mg.preview_btn)) {
          ccord_admin_request req;
          ccord_error err;
          if (ccord_admin_preview_invite(&req, ccord_text_edit_cstr(&ui->modal_edit1), &err) != CCORD_OK) {
            ui_notice(ui, "Invalid invite: %s", err.message);
          } else {
            ui_admin_submit(ui, &req);
          }
          ui->dirty = true;
          return true;
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_FOLDER_SETTINGS) {
        if (ui_in_rect(ev->x, ev->y, mg.input)) {
          ui->modal_focus_field = 0;
          ui->dirty = true;
          return true;
        }
        if (ui_in_rect(ev->x, ev->y, mg.input2)) {
          ui->modal_focus_field = 1;
          ui->dirty = true;
          return true;
        }
        for (size_t i = 0; i < mg.nchips; i++) {
          if (ui_in_rect(ev->x, ev->y, mg.chip[i])) {
            ui->modal_type_sel = (int)i;
            ccord_text_edit_clear(&ui->modal_edit2);
            if (k_folder_color_presets[i] != 0) {
              char hex[16];
              ccord_error herr;
              int hn = snprintf(hex, sizeof hex, "#%06X", (unsigned)k_folder_color_presets[i]);
              if (hn > 0) (void)ccord_text_edit_insert(&ui->modal_edit2, hex, (size_t)hn, &herr);
            }
            ui->dirty = true;
            return true;
          }
        }
        return true;
      }
      if (ui->modal_kind == UI_MODAL_CONFIRM) {
        for (size_t i = 0; i < mg.nchips; i++) {
          if (ui_in_rect(ev->x, ev->y, mg.chip[i])) {
            ui->modal_type_sel = (int)i;
            ui->dirty = true;
            break;
          }
        }
      }
      return true;
    }
    return true;
  }

  return false;
}
