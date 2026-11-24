// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Composer live formatting. The composer buffer is never touched: this file reads it, decides which markdown markers
// are real by running the posted-message tokenizer (ui_md_next, the one place that pairs markers: closed, not
// escaped, not inside code or a URL) in composer mode, and lays the result out itself as styled pieces. Wrapping, caret, hit-testing, selection and
// the composer height all read the same measured pieces that get drawn, so bold and smaller code runs (which have
// other advances than the regular font) can never drift from the caret.
#include <math.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>

#include "ui_internal.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define CF_MAX_TEXT 2048u                 // the composer holds at most CCORD_SEND_MAX_BYTES (2000)
#define CF_MAX_PIECES (2u * CF_MAX_TEXT + 64u)   // a piece covers >= 1 byte and a word is split at most once per byte
#define CF_MAX_LINES (2u * CF_MAX_TEXT + 64u)
#define CF_QUOTE_INDENT 12.0f
#define CF_TRACE_MAX 256u

typedef enum cf_kind : uint8_t { CF_PLAIN, CF_QUOTE_LINE, CF_FENCE_LINE } cf_kind;

typedef struct cf_piece {
  uint16_t start, len;
  uint16_t flags;
  float x, w;
  ccord_text_style st;
} cf_piece;

typedef struct cf_vline {
  uint16_t start, end;   // bytes; end is the newline, or where the next visual line starts
  uint32_t first, count; // pieces
  float x0;
  cf_kind kind;
} cf_vline;

typedef struct cf_ll {
  uint16_t start, end;
  cf_kind kind;
} cf_ll;

typedef struct cf_wseg {
  uint16_t start, end;
  float w;
} cf_wseg;

typedef struct cf_state {
  bool valid;
  size_t len;
  float wrap, size;
  float probe[5];
  ccord_color colors[7];
  uint64_t lists_rev;
  ccord_id guild_id;
  ccord_id channel_id;
  float pitch, fh;
  char text[CF_MAX_TEXT + 1];
  uint16_t fl[CF_MAX_TEXT];
  bool lit_span[CF_MAX_TEXT];
  cf_ll ll[CF_MAX_TEXT + 2];
  size_t nll;
  cf_piece pieces[CF_MAX_PIECES];
  size_t npieces;
  cf_vline vl[CF_MAX_LINES];
  size_t nvl;
  cf_wseg wseg[CF_MAX_TEXT];
  ui_cf_trace_piece trace[CF_TRACE_MAX];
  size_t ntrace;
} cf_state;

static cf_state *g_cf;
static uint32_t g_cf_passes;
static size_t g_cf_nvl;

static struct {
  bool valid;
  const ccord_renderer *r;
  const ccord_theme *theme;
  float size, zoom, rw, rh;
  int fb_fonts, sys_fonts;
  float fh, pitch;
  float probe[5];
} g_cf_fm;

#define G (*g_cf)

void ui_composer_format_free(void)
{
  ccord_free(g_cf);
  g_cf = nullptr;
  g_cf_nvl = 0;
  g_cf_fm.valid = false;
}

static float cf_maxf(float a, float b) { return a > b ? a : b; }

// ---------------------------------------------------------------- which markers are real

static bool cf_space(char c) { return c == ' ' || c == '\t'; }

static uint16_t cf_state_flags(const ui_md_iter *it)
{
  if (it->code) return UI_CF_CODE;
  uint16_t f = 0;
  if (it->bold) f |= UI_CF_BOLD;
  if (it->italic) f |= UI_CF_ITALIC;
  if (it->underline) f |= UI_CF_UNDER;
  if (it->strikethrough) f |= UI_CF_STRIKE;
  if (it->spoiler) f |= UI_CF_SPOILER;
  return f;
}

// Per-byte flags for one logical line [a, b): token bytes carry the style of the span, the bytes between tokens
// (the markers the tokenizer swallowed) carry the span style plus UI_CF_MARKER.
static void cf_style_line(const ccord_ui *ui, size_t a, size_t b, uint16_t extra)
{
  if (b <= a) return;
  ui_md_iter it;
  ui_md_iter_init(&it, G.text + a, b - a);
  it.composer = true;
  ui_md_token tok;
  size_t prev_end = a;
  uint16_t prev_state = 0;
  while (ui_md_next(&it, &tok)) {
    size_t ts = (size_t)(tok.text - G.text), te = ts + tok.len;
    if (ts < prev_end || te > b || te < ts) break;   // never happens for plain text; keeps a bad token from corrupting flags
    uint16_t st = cf_state_flags(&it);
    for (size_t k = prev_end; k < ts; k++) G.fl[k] = (uint16_t)(extra | UI_CF_MARKER | prev_state | st);
    for (size_t k = ts; k < te; k++) {
      G.fl[k] = (uint16_t)(extra | st);
      if (tok.literal_span) G.lit_span[k] = true;
    }
    prev_end = te;
    prev_state = st;
  }
  uint16_t st = (uint16_t)(prev_state | cf_state_flags(&it));
  for (size_t k = prev_end; k < b; k++) G.fl[k] = (uint16_t)(extra | UI_CF_MARKER | st);

  if (ui && ui->d.store) {
    for (size_t i = a; i < b;) {
      if ((G.fl[i] & (UI_CF_CODE | UI_CF_MARKER | UI_CF_BLOCK)) || G.lit_span[i]) {
        i++;
        continue;
      }
      if (G.text[i] == '@' || G.text[i] == '#') {
        size_t mlen = ui_composer_match_mention(ui, G.text + a, b - a, i - a);
        if (mlen > 0 && i + mlen <= b) {
          bool clean = true;
          for (size_t k = i; k < i + mlen; k++) {
            if ((G.fl[k] & (UI_CF_CODE | UI_CF_MARKER | UI_CF_BLOCK)) || G.lit_span[k]) {
              clean = false;
              break;
            }
          }
          if (clean) {
            for (size_t k = i; k < i + mlen; k++) G.fl[k] |= UI_CF_MENTION;
            i += mlen;
            continue;
          }
        }
      }
      i++;
    }
  }
}

// Blocks: fenced code (needs a closing fence), `> ` quotes and `>>> ` multi-line quotes; everything else is inline.
static void cf_block_pass(const ccord_ui *ui, const char *t, size_t len)
{
  G.nll = 0;
  bool fenced = false, quote_multi = false;
  size_t pos = 0;
  while (pos <= len) {
    size_t end = pos;
    while (end < len && t[end] != '\n') end++;
    size_t ll = end - pos;
    bool fence = ll >= 3 && memcmp(t + pos, "```", 3) == 0;
    cf_kind kind = CF_PLAIN;
    size_t prefix = 0;
    if (fenced) {
      kind = CF_FENCE_LINE;
      if (fence) fenced = false;
    } else if (fence) {
      bool one_line = false;   // ```code``` on one line is inline code
      for (size_t k = pos + 3; k + 3 <= end; k++) {
        if (memcmp(t + k, "```", 3) == 0) one_line = true;
      }
      bool closed = false;
      for (size_t p2 = end + 1; p2 < len && !one_line && !closed;) {
        size_t e2 = p2;
        while (e2 < len && t[e2] != '\n') e2++;
        if (e2 - p2 >= 3 && memcmp(t + p2, "```", 3) == 0) closed = true;
        p2 = e2 + 1;
      }
      if (closed) {
        kind = CF_FENCE_LINE;
        fenced = true;
      }
    }
    if (kind == CF_PLAIN) {
      if (quote_multi) {
        kind = CF_QUOTE_LINE;
      } else if (ll >= 2 && t[pos] == '>' && t[pos + 1] == ' ') {
        kind = CF_QUOTE_LINE;
        prefix = 2;
      } else if (ll >= 4 && memcmp(t + pos, ">>> ", 4) == 0) {
        kind = CF_QUOTE_LINE;
        prefix = 4;
        quote_multi = true;
      }
    }
    G.ll[G.nll++] = (cf_ll){.start = (uint16_t)pos, .end = (uint16_t)end, .kind = kind};
    if (kind == CF_FENCE_LINE) {
      bool marker_line = fence;
      for (size_t k = pos; k < end; ) {
        uint8_t b = (uint8_t)t[k];
        size_t char_len = 1;
        if (b >= 0xC0) {
          if ((b & 0xE0) == 0xC0) char_len = 2;
          else if ((b & 0xF0) == 0xE0) char_len = 3;
          else if ((b & 0xF8) == 0xF0) char_len = 4;
        }
        if (k + char_len > end) char_len = end - k;
        for (size_t step = 0; step < char_len; step++) {
          G.fl[k + step] = (uint16_t)(UI_CF_BLOCK | UI_CF_CODE | (marker_line ? UI_CF_MARKER : 0u));
        }
        k += char_len;
      }
    } else {
      size_t from = pos;
      uint16_t extra = kind == CF_QUOTE_LINE ? UI_CF_QUOTE : 0u;
      for (; from < pos + prefix; from++) G.fl[from] = (uint16_t)(UI_CF_QUOTE | UI_CF_MARKER);
      cf_style_line(ui, from, end, extra);
    }
    if (end >= len) break;
    pos = end + 1;
  }
}

// ---------------------------------------------------------------- layout

static ccord_text_style cf_piece_style(const ccord_ui *ui, uint16_t fl)
{
  float size = M(font_size_base);
  ccord_color col = COL(text_normal);
  ccord_font_style font = CCORD_FONT_REGULAR;
  if (fl & UI_CF_CODE) {
    font = CCORD_FONT_MONO;
    size = cf_maxf(size - 1.0f, 10.0f);
    col = COL(text_header);
  } else {
    if ((fl & UI_CF_BOLD) && (fl & UI_CF_ITALIC)) font = CCORD_FONT_BOLD_ITALIC;
    else if (fl & UI_CF_BOLD) font = CCORD_FONT_BOLD;
    else if (fl & UI_CF_ITALIC) font = CCORD_FONT_ITALIC;
    if (fl & UI_CF_MENTION) {
      font = CCORD_FONT_BOLD;
      col = COL(text_link);
    }
  }
  if (fl & UI_CF_MARKER) col = COL(text_muted);
  return ui_style(ui, font, size, col);
}

static void cf_add_piece(ccord_renderer *r, const char *t, size_t s, size_t e, uint16_t fl, float x, float w_known,
                         const ccord_ui *ui)
{
  if (G.npieces >= CF_MAX_PIECES) return;
  cf_piece *p = &G.pieces[G.npieces++];
  p->start = (uint16_t)s;
  p->len = (uint16_t)(e - s);
  p->flags = fl;
  p->x = x;
  p->st = cf_piece_style(ui, fl);
  p->w = w_known >= 0.0f ? w_known : ccord_text_measure(r, t + s, e - s, &p->st, 0.0f).x;
  G.vl[G.nvl - 1].count++;
}

static void cf_new_vline(size_t start, float x0, cf_kind kind)
{
  if (G.nvl >= CF_MAX_LINES) return;
  G.vl[G.nvl++] = (cf_vline){.start = (uint16_t)start, .end = (uint16_t)start, .first = (uint32_t)G.npieces, .count = 0,
                             .x0 = x0, .kind = kind};
}

// Largest byte count (on a code point boundary; 0 when not even one code point) of text[s, e) that fits `room`.
// The caller has checked that the whole range does not fit.
static size_t cf_fit(ccord_renderer *r, const char *t, size_t s, size_t e, const ccord_text_style *st, float room)
{
  size_t lo = 0, hi = e - s;   // lo fits, hi does not
  while (hi - lo > 1) {
    size_t mid = lo + (hi - lo) / 2;
    while (mid > lo && ((unsigned char)t[s + mid] & 0xC0) == 0x80) mid--;
    if (mid <= lo) mid = ccord_utf8_next(t + s, hi, lo);
    if (mid <= lo || mid >= hi) break;
    if (ccord_text_measure(r, t + s, mid, st, 0.0f).x <= room) lo = mid;
    else hi = mid;
  }
  return lo;
}

static void cf_layout(ccord_ui *ui, ccord_renderer *r, const char *t, size_t len, float wrap)
{
  g_cf_passes++;
  G.nll = G.npieces = G.nvl = 0;
  memset(G.fl, 0, len * sizeof G.fl[0]);
  memset(G.lit_span, 0, len * sizeof G.lit_span[0]);
  cf_block_pass(ui, t, len);
  for (size_t li = 0; li < G.nll; li++) {
    const cf_ll *ll = &G.ll[li];
    float indent = ll->kind == CF_QUOTE_LINE ? CF_QUOTE_INDENT : 0.0f;
    float avail = cf_maxf(wrap - indent, 20.0f);
    float cx = 0.0f;
    cf_new_vline(ll->start, indent, ll->kind);
    size_t i = ll->start;
    while (i < ll->end) {
      if (cf_space(t[i])) {
        size_t j = i;
        while (j < ll->end && cf_space(t[j]) && G.fl[j] == G.fl[i]) j++;
        cf_add_piece(r, t, i, j, G.fl[i], indent + cx, -1.0f, ui);
        cx += G.pieces[G.npieces - 1].w;
        i = j;
        continue;
      }
      size_t nseg = 0;
      float ww = 0.0f;
      for (size_t k = i; k < ll->end && !cf_space(t[k]);) {
        size_t m = k;
        // A marker is drawn one character at a time: the font may join `||` or `__` into one glyph with no
        // boundary between the two, and the caret needs one.
        if (G.fl[k] & UI_CF_MARKER) m = k + 1;
        else while (m < ll->end && !cf_space(t[m]) && G.fl[m] == G.fl[k]) m++;
        ccord_text_style st = cf_piece_style(ui, G.fl[k]);
        float w = ccord_text_measure(r, t + k, m - k, &st, 0.0f).x;
        G.wseg[nseg++] = (cf_wseg){.start = (uint16_t)k, .end = (uint16_t)m, .w = w};
        ww += w;
        k = m;
      }
      size_t word_end = G.wseg[nseg - 1].end;
      if (G.vl[G.nvl - 1].count > 0 && cx + ww > avail) {
        G.vl[G.nvl - 1].end = (uint16_t)i;
        cf_new_vline(i, indent, ll->kind);
        cx = 0.0f;
      }
      if (ww <= avail || G.nvl >= CF_MAX_LINES) {
        for (size_t s = 0; s < nseg; s++) {
          cf_add_piece(r, t, G.wseg[s].start, G.wseg[s].end, G.fl[G.wseg[s].start], indent + cx, G.wseg[s].w, ui);
          cx += G.wseg[s].w;
        }
      } else {
        // One word wider than the line: break it between characters.
        for (size_t s = 0; s < nseg; s++) {
          size_t a = G.wseg[s].start, e = G.wseg[s].end;
          uint16_t fl = G.fl[a];
          ccord_text_style st = cf_piece_style(ui, fl);
          while (a < e) {
            if (G.nvl >= CF_MAX_LINES) {   // cannot happen (a line takes >= 1 byte); never loop
              cf_add_piece(r, t, a, e, fl, indent + cx, -1.0f, ui);
              a = e;
              break;
            }
            float w = ccord_text_measure(r, t + a, e - a, &st, 0.0f).x;
            if (cx + w <= avail) {
              cf_add_piece(r, t, a, e, fl, indent + cx, w, ui);
              cx += w;
              a = e;
              break;
            }
            size_t k = cf_fit(r, t, a, e, &st, avail - cx);
            if (k == 0 && G.vl[G.nvl - 1].count > 0) {
              G.vl[G.nvl - 1].end = (uint16_t)a;
              cf_new_vline(a, indent, ll->kind);
              cx = 0.0f;
              continue;
            }
            if (k == 0) k = ccord_utf8_next(t + a, e - a, 0);   // a single glyph wider than the line still takes a line
            cf_add_piece(r, t, a, a + k, fl, indent + cx, -1.0f, ui);
            cx += G.pieces[G.npieces - 1].w;
            a += k;
            if (a < e) {
              G.vl[G.nvl - 1].end = (uint16_t)a;
              cf_new_vline(a, indent, ll->kind);
              cx = 0.0f;
            }
          }
        }
      }
      i = word_end;
    }
    G.vl[G.nvl - 1].end = ll->end;
  }
}

static void cf_ensure_metrics(const ccord_ui *ui, ccord_renderer *r, float size)
{
  float zoom = ccord_render_get_zoom(r);
  ccord_vec2 rsz = ccord_render_size(r);
  int fb = ccord_render_loaded_fallback_font_count(r);
  int sys = ccord_render_system_font_count(r);
  if (g_cf_fm.valid && g_cf_fm.r == r && g_cf_fm.theme == ui->theme && g_cf_fm.size == size &&
      g_cf_fm.zoom == zoom && g_cf_fm.rw == rsz.x && g_cf_fm.rh == rsz.y &&
      g_cf_fm.fb_fonts == fb && g_cf_fm.sys_fonts == sys) {
    return;
  }
  ccord_text_style reg = ui_style(ui, CCORD_FONT_REGULAR, size, COL(text_normal));
  ccord_text_style bold = ui_style(ui, CCORD_FONT_BOLD, size, COL(text_normal));
  ccord_text_style code = ui_style(ui, CCORD_FONT_MONO, cf_maxf(size - 1.0f, 10.0f), COL(text_normal));
  float fh = ccord_text_measure(r, "A", 1, &reg, 0.0f).y;
  float pitch = ccord_text_caret_rect(r, "A\nA", 3, &reg, 0.0f, 2).y;
  if (pitch < 1.0f) pitch = fh;
  g_cf_fm.valid = true;
  g_cf_fm.r = r;
  g_cf_fm.theme = ui->theme;
  g_cf_fm.size = size;
  g_cf_fm.zoom = zoom;
  g_cf_fm.rw = rsz.x;
  g_cf_fm.rh = rsz.y;
  g_cf_fm.fb_fonts = fb;
  g_cf_fm.sys_fonts = sys;
  g_cf_fm.fh = fh;
  g_cf_fm.pitch = pitch;
  g_cf_fm.probe[0] = ccord_text_measure(r, "Mg", 2, &reg, 0.0f).x;
  g_cf_fm.probe[1] = ccord_text_measure(r, "Mg", 2, &bold, 0.0f).x;
  g_cf_fm.probe[2] = ccord_text_measure(r, "Mg", 2, &code, 0.0f).x;
  g_cf_fm.probe[3] = fh;
  g_cf_fm.probe[4] = pitch;
}

// Brings the cached layout up to date. Returns false when empty (or OOM), avoiding allocating the 245 KiB
// layout state while the composer is empty. Font metrics are probed only when the (renderer, theme, size,
// zoom, viewport, font-set) epoch changes, and store invalidation tracks lists_revision (ignoring chat traffic).
static bool cf_ensure(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap)
{
  if (!text) len = 0;
  if (len > CF_MAX_TEXT) {
    len = CF_MAX_TEXT;
    while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) len--;
  }
  float size = M(font_size_base);
  cf_ensure_metrics(ui, r, size);
  if (len == 0) {
    g_cf_nvl = 1;
    if (g_cf) {
      ccord_free(g_cf);
      g_cf = nullptr;
    }
    return false;
  }
  if (!g_cf) {
    g_cf = ccord_calloc(1, sizeof *g_cf);
    if (!g_cf) return false;
  }
  ccord_color colors[7] = {COL(text_normal), COL(text_secondary), COL(text_header), COL(text_muted), COL(bg_tertiary),
                           COL(brand), COL(text_link)};
  uint64_t lists_rev = ui->d.store ? ccord_store_lists_revision(ui->d.store) : 0;
  ccord_id guild_id = ui->d.store ? ccord_store_selected_guild(ui->d.store) : CCORD_ID_NONE;
  ccord_id channel_id = ui->d.store ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE;
  if (G.valid && G.len == len && G.wrap == wrap && G.size == size && G.lists_rev == lists_rev &&
      G.guild_id == guild_id && G.channel_id == channel_id &&
      memcmp(G.probe, g_cf_fm.probe, sizeof G.probe) == 0 &&
      memcmp(G.colors, colors, sizeof colors) == 0 && memcmp(G.text, text, len) == 0) {
    return true;
  }
  memcpy(G.text, text, len);
  G.text[len] = '\0';
  G.len = len;
  G.wrap = wrap;
  G.size = size;
  G.lists_rev = lists_rev;
  G.guild_id = guild_id;
  G.channel_id = channel_id;
  memcpy(G.probe, g_cf_fm.probe, sizeof G.probe);
  memcpy(G.colors, colors, sizeof colors);
  G.fh = g_cf_fm.fh;
  G.pitch = g_cf_fm.pitch;
  G.valid = true;
  cf_layout(ui, r, G.text, len, wrap);
  g_cf_nvl = G.nvl;
  return true;
}

// ---------------------------------------------------------------- queries

static float cf_x_in_line(ccord_renderer *r, const cf_vline *v, size_t pos)
{
  for (size_t k = 0; k < v->count; k++) {
    const cf_piece *p = &G.pieces[v->first + k];
    if (pos < p->start) return p->x;
    if (pos < (size_t)p->start + p->len) {
      if (pos == p->start) return p->x;
      return p->x + ccord_text_caret_rect(r, G.text + p->start, p->len, &p->st, 0.0f, pos - p->start).x;
    }
  }
  if (v->count > 0) {
    const cf_piece *p = &G.pieces[v->first + v->count - 1];
    return p->x + p->w;
  }
  return v->x0;
}

float ui_composer_format_height(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap)
{
  if (!cf_ensure(ui, r, text, len, wrap)) return g_cf_fm.fh;
  return (float)(G.nvl > 0 ? G.nvl - 1 : 0) * G.pitch + G.fh;
}

ccord_rect ui_composer_format_caret(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, size_t pos)
{
  if (!cf_ensure(ui, r, text, len, wrap)) return (ccord_rect){0.0f, 0.0f, 1.0f, g_cf_fm.fh};
  if (pos > G.len) pos = G.len;
  size_t vi = 0;
  for (size_t k = 0; k < G.nvl; k++) {
    if (G.vl[k].start <= pos) vi = k;
    else break;
  }
  return (ccord_rect){cf_x_in_line(r, &G.vl[vi], pos), (float)vi * G.pitch, 1.0f, G.fh};
}

size_t ui_composer_format_hit(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, float x, float y)
{
  if (!cf_ensure(ui, r, text, len, wrap) || G.len == 0) return 0;
  float fv = y / G.pitch;
  size_t vi = fv <= 0.0f ? 0 : (size_t)fv;
  if (vi >= G.nvl) vi = G.nvl - 1;
  const cf_vline *v = &G.vl[vi];
  for (size_t k = 0; k < v->count; k++) {
    const cf_piece *p = &G.pieces[v->first + k];
    if (x < p->x + p->w || k + 1 == v->count) {
      if (x >= p->x + p->w) return (size_t)p->start + p->len;
      size_t off = ccord_text_hit_test(r, G.text + p->start, p->len, &p->st, 0.0f, x - p->x, G.fh * 0.5f);
      return (size_t)p->start + off;
    }
  }
  return v->end;
}

size_t ui_composer_format_selection(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, size_t from,
                                    size_t to, ui_cf_sel_rect *out, size_t cap)
{
  if (!cf_ensure(ui, r, text, len, wrap)) return 0;
  if (from > to) {
    size_t tmp = from;
    from = to;
    to = tmp;
  }
  if (to > G.len) to = G.len;
  size_t n = 0;
  if (from >= to) return 0;
  for (size_t vi = 0; vi < G.nvl; vi++) {
    const cf_vline *v = &G.vl[vi];
    if (to <= v->start || from > v->end) continue;
    size_t lo = from > v->start ? from : v->start;
    size_t hi = to < v->end ? to : v->end;
    float y = (float)vi * G.pitch;
    float x0 = cf_x_in_line(r, v, lo);
    float x1 = hi > lo ? cf_x_in_line(r, v, hi) : x0;
    if (hi > lo && n < cap) out[n++] = (ui_cf_sel_rect){{x0, y, x1 - x0, G.fh}, false};
    bool hard = v->end < G.len && G.text[v->end] == '\n';
    if (hard && to > v->end && from <= v->end && n < cap) {   // the line break itself is selected: a short stub, as in a browser
      out[n++] = (ui_cf_sel_rect){{x1, y, M(font_size_base) * 0.3f, G.fh}, true};
    }
  }
  return n;
}

// ---------------------------------------------------------------- drawing

static ccord_color cf_piece_bg(const ccord_ui *ui, uint16_t fl, bool *has)
{
  *has = false;
  if (fl & UI_CF_BLOCK) return COL(bg_tertiary);
  if (fl & UI_CF_CODE) {
    *has = true;
    return COL(bg_tertiary);
  }
  if (fl & UI_CF_MENTION) {
    *has = true;
    return ui_alpha(COL(brand), 0.22f);
  }
  if (fl & UI_CF_SPOILER) {
    *has = true;
    return ui_mix(COL(bg_tertiary), COL(brand), 0.18f);
  }
  return COL(bg_tertiary);
}

void ui_composer_format_draw(ccord_ui *ui, ccord_renderer *r, const char *text, size_t len, float wrap, float x, float y,
                             size_t sel_from, size_t sel_to, float view_top, float view_bottom)
{
  if (!cf_ensure(ui, r, text, len, wrap)) {
    if (g_cf) G.ntrace = 0;
    return;
  }
  G.ntrace = 0;
  if (G.len == 0) return;
  float lh = G.pitch;
  // Block backdrops (fenced code) and the quote bar, one rect per run of lines.
  for (size_t vi = 0; vi < G.nvl;) {
    cf_kind kind = G.vl[vi].kind;
    size_t vj = vi;
    while (vj < G.nvl && G.vl[vj].kind == kind) vj++;
    float top = y + (float)vi * lh, h = (float)(vj - vi) * lh;
    if (kind != CF_PLAIN && (top - y) + h >= view_top && (top - y) <= view_bottom) {
      if (kind == CF_FENCE_LINE) ccord_draw_rrect(r, (ccord_rect){x - 4.0f, top, wrap + 8.0f, h}, 4.0f, COL(bg_tertiary));
      else ccord_draw_rect(r, (ccord_rect){x + 1.0f, top + 1.0f, 3.0f, h - 2.0f}, COL(brand));
    }
    vi = vj;
  }
  // Selection under the text.
  if (sel_from != sel_to) {
    ui_cf_sel_rect rects[64];
    size_t n = ui_composer_format_selection(ui, r, text, len, wrap, sel_from, sel_to, rects, 64);
    for (size_t k = 0; k < n; k++) {
      ccord_rect rc = rects[k].rc;
      rc.h = lh;
      rc.x += x;
      rc.y += y;
      ccord_draw_rect(r, rc, COL(selection));
    }
  }
  for (size_t vi = 0; vi < G.nvl; vi++) {
    const cf_vline *v = &G.vl[vi];
    float ly = (float)vi * lh;
    if (ly + lh < view_top || ly > view_bottom) continue;
    // Backgrounds of inline code, mentions and spoilers, merged over consecutive pieces so no seams show.
    for (size_t k = 0; k < v->count;) {
      bool has = false;
      ccord_color bg = cf_piece_bg(ui, G.pieces[v->first + k].flags, &has);
      uint16_t kind_flags = G.pieces[v->first + k].flags & (UI_CF_CODE | UI_CF_SPOILER | UI_CF_MENTION);
      if (!has) {
        k++;
        continue;
      }
      size_t m = k;
      while (m + 1 < v->count) {
        const cf_piece *nx = &G.pieces[v->first + m + 1];
        if ((nx->flags & (UI_CF_CODE | UI_CF_SPOILER | UI_CF_MENTION)) != kind_flags) break;
        m++;
      }
      const cf_piece *p0 = &G.pieces[v->first + k], *p1 = &G.pieces[v->first + m];
      ccord_draw_rrect(r, (ccord_rect){x + p0->x, y + ly + 1.0f, p1->x + p1->w - p0->x, lh - 2.0f}, 3.0f, bg);
      k = m + 1;
    }
    for (size_t k = 0; k < v->count; k++) {
      const cf_piece *p = &G.pieces[v->first + k];
      ccord_text_style st = p->st;
      if ((p->flags & UI_CF_SPOILER) && !(p->flags & (UI_CF_MARKER | UI_CF_MENTION))) st.color = COL(text_normal);   // readable on the tint
      if (!cf_space(G.text[p->start])) ccord_text_draw(r, G.text + p->start, p->len, x + p->x, y + ly, &st, 0.0f);
      if (!(p->flags & UI_CF_MARKER)) {
        if (p->flags & UI_CF_UNDER) ccord_draw_rect(r, (ccord_rect){x + p->x, y + ly + lh - 2.0f, p->w, 1.0f}, st.color);
        if (p->flags & UI_CF_STRIKE) ccord_draw_rect(r, (ccord_rect){x + p->x, y + ly + lh * 0.52f, p->w, 1.0f}, st.color);
      }
      if (G.ntrace < CF_TRACE_MAX) {
        bool has = false;
        ccord_color bg = cf_piece_bg(ui, p->flags, &has);
        G.trace[G.ntrace++] = (ui_cf_trace_piece){.start = p->start, .len = p->len, .x = p->x, .y = ly, .w = p->w, .st = st,
                                                  .flags = p->flags, .bg = bg, .has_bg = has || (p->flags & UI_CF_BLOCK)};
      }
    }
  }
}

uint32_t ui_composer_format_passes(void) { return g_cf_passes; }

size_t ui_composer_format_trace(ui_cf_trace_piece *out, size_t cap)
{
  if (!g_cf) return 0;
  size_t n = G.ntrace < cap ? G.ntrace : cap;
  if (out) memcpy(out, G.trace, n * sizeof *out);
  return G.ntrace;
}

size_t ui_composer_format_line_count(void) { return g_cf ? G.nvl : g_cf_nvl; }
