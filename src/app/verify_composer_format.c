// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice composer-format: the composer styles markdown while typing (src/ui/composer_format.c).
//
// Everything goes through the real UI input path (CCORD_IN_TEXT, key and mouse events) over the mock transport, and the
// slice reads back what the draw produced (ui_composer_format_trace: each run exactly as it was handed to
// ccord_text_draw). It checks the styled spans and their dimmed markers, that unclosed or escaped markers style nothing,
// that the buffer and the sent message are byte-identical to what was typed, that caret, IME rect, mouse hit-testing and
// selection agree with the drawn glyphs once the text wraps, fenced code and quote blocks, and that nothing is restyled on
// a frame where text, width and theme are unchanged. It saves docs/screenshots/composer-format.png: the styled composer
// under the same text posted. A posted-vs-composer section sends samples through the mock and compares the styled
// characters of the posted row (ui->dbg_md_runs) with the composer's.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ui_internal.h"

#define CF_SHOT "docs/screenshots/composer-format.png"

typedef struct cf_slice {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  ui_cf_trace_piece tr[256];
  size_t ntr;
} cf_slice;

static int cf_fail(const char *name, const char *detail)
{
  printf("composer-format: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define CF_CHECK(m, name, cond, detail)             \
  do {                                              \
    if (!(cond)) return cf_fail((name), (detail));  \
    (m)->checks++;                                  \
  } while (0)

static void cf_type(cf_slice *m, const char *text)
{
  size_t left = strlen(text);
  while (left > 0) {
    ccord_input_event ev = {.type = CCORD_IN_TEXT};
    size_t take = left < sizeof ev.text - 1u ? left : sizeof ev.text - 1u;
    while (take < left && take > 1 && ((uint8_t)text[take] & 0xC0u) == 0x80u) take--;
    memcpy(ev.text, text, take);
    m->c->input(m->c->ud, &ev);
    text += take;
    left -= take;
  }
}

static void cf_key(cf_slice *m, ccord_key key, uint8_t mods)
{
  ccord_input_event dn = {.type = CCORD_IN_KEY_DOWN, .key = key, .mods = mods};
  m->c->input(m->c->ud, &dn);
  m->c->frames(m->c->ud, 2);
}

static void cf_clear(cf_slice *m)
{
  if (m->c->ui->autocomplete_open) cf_key(m, CCORD_KEY_ESCAPE, 0);
  cf_key(m, CCORD_KEY_A, CCORD_MOD_CTRL);
  cf_key(m, CCORD_KEY_BACKSPACE, 0);
}

// Types `text` (a newline is a Shift+Enter, as a user would) and renders the composer.
static void cf_enter_text(cf_slice *m, const char *text)
{
  cf_clear(m);
  while (*text) {
    const char *nl = strchr(text, '\n');
    size_t n = nl ? (size_t)(nl - text) : strlen(text);
    char chunk[512];
    if (n >= sizeof chunk) n = sizeof chunk - 1u;
    memcpy(chunk, text, n);
    chunk[n] = '\0';
    cf_type(m, chunk);
    text += n;
    if (*text == '\n') {
      cf_key(m, CCORD_KEY_RETURN, CCORD_MOD_SHIFT);
      text++;
    }
  }
  m->c->frames(m->c->ud, 3);
  m->ntr = ui_composer_format_trace(m->tr, 256);
  if (m->ntr > 256) m->ntr = 256;
}

static const ui_cf_trace_piece *cf_piece_at(const cf_slice *m, size_t at)
{
  for (size_t i = 0; i < m->ntr; i++) {
    if (at >= m->tr[i].start && at < m->tr[i].start + m->tr[i].len) return &m->tr[i];
  }
  return nullptr;
}

static const ui_cf_trace_piece *cf_piece_of(const cf_slice *m, const char *src, const char *needle)
{
  const char *p = strstr(src, needle);
  return p ? cf_piece_at(m, (size_t)(p - src)) : nullptr;
}

static bool cf_same_color(ccord_color a, ccord_color b) { return memcmp(&a, &b, sizeof a) == 0; }

static bool cf_dim(const ccord_ui *ui, const ui_cf_trace_piece *p)
{
  return p && (p->flags & UI_CF_MARKER) && cf_same_color(p->st.color, ui->theme->c.text_muted);
}

static bool cf_buffer_is(const ccord_ui *ui, const char *text)
{
  return ui->edit.len == strlen(text) && memcmp(ccord_text_edit_cstr(&ui->edit), text, ui->edit.len) == 0;
}

// The wrap width and text origin the composer draws with (ui_draw_composer): reproduced here so the slice does not
// trust the draw code's own numbers.
static float cf_wrap(const ccord_ui *ui)
{
  const ccord_theme_metrics *mm = &ui->theme->m;
  float w = ui->layout.composer.w - 2.0f * mm->composer_margin_x - 2.0f * mm->composer_pad_x - 28.0f - 128.0f;
  return w > 20.0f ? w : 20.0f;
}

static ccord_vec2 cf_origin(const ccord_ui *ui)
{
  ccord_rect box = ui_composer_box(ui);
  const ccord_theme_metrics *mm = &ui->theme->m;
  return (ccord_vec2){box.x + mm->composer_pad_x + 28.0f, box.y + mm->composer_pad_y - ui->composer_scroll};
}

static void cf_click(cf_slice *m, float x, float y, uint8_t mods)
{
  ccord_input_event mv = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  m->c->input(m->c->ud, &mv);
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y, .mods = mods};
  m->c->input(m->c->ud, &dn);
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y, .mods = mods};
  m->c->input(m->c->ud, &up);
  m->c->frames(m->c->ud, 2);
}

static const char k_spans[] = "**b** *i* __u__ ~~s~~ `c` ||sp||";

static int cf_check_spans(cf_slice *m)
{
  const ccord_ui *ui = m->c->ui;
  cf_enter_text(m, k_spans);
  CF_CHECK(m, "buffer is the typed text", cf_buffer_is(ui, k_spans), "the composer buffer changed");
  CF_CHECK(m, "composer painted pieces", m->ntr > 0, "no styled pieces were drawn by the composer");
  const ui_cf_trace_piece *b = cf_piece_of(m, k_spans, "b** ");
  CF_CHECK(m, "bold span", b && (b->flags & UI_CF_BOLD) && !(b->flags & UI_CF_MARKER) && b->st.font == CCORD_FONT_BOLD,
           "b is not drawn bold");
  const ui_cf_trace_piece *i = cf_piece_of(m, k_spans, "i*");
  CF_CHECK(m, "italic span", i && (i->flags & UI_CF_ITALIC) && !(i->flags & UI_CF_MARKER) &&
                                 !cf_same_color(i->st.color, ui->theme->c.text_normal),
           "i is not drawn italic");
  const ui_cf_trace_piece *u = cf_piece_of(m, k_spans, "u__");
  CF_CHECK(m, "underline span", u && (u->flags & UI_CF_UNDER) && !(u->flags & UI_CF_MARKER), "u is not underlined");
  const ui_cf_trace_piece *s = cf_piece_of(m, k_spans, "s~~");
  CF_CHECK(m, "strike span", s && (s->flags & UI_CF_STRIKE) && !(s->flags & UI_CF_MARKER), "s is not struck through");
  const ui_cf_trace_piece *c = cf_piece_of(m, k_spans, "c`");
  CF_CHECK(m, "code span", c && (c->flags & UI_CF_CODE) && c->has_bg && c->st.size < ui->theme->m.font_size_base,
           "c has no code style and background");
  const ui_cf_trace_piece *sp = cf_piece_of(m, k_spans, "sp||");
  CF_CHECK(m, "spoiler span", sp && (sp->flags & UI_CF_SPOILER) && sp->has_bg &&
                                  cf_same_color(sp->st.color, ui->theme->c.text_normal),
           "sp has no tinted background or its text is not readable");
  // Markers: all twelve are still in the text and drawn dimmed.
  static const size_t mpos[] = {0, 3, 6, 8, 10, 13, 16, 19, 22, 24, 26, 30};
  for (size_t k = 0; k < sizeof mpos / sizeof mpos[0]; k++) {
    CF_CHECK(m, "marker dimmed", cf_dim(ui, cf_piece_at(m, mpos[k])), "a marker is not drawn dimmed");
  }
  size_t covered = 0;
  for (size_t k = 0; k < m->ntr; k++) covered += m->tr[k].len;
  CF_CHECK(m, "every byte drawn once", covered == strlen(k_spans), "the pieces do not cover the typed text exactly once");
  return 0;
}

static int cf_check_unstyled(cf_slice *m, const char *text, const char *name)
{
  cf_enter_text(m, text);
  CF_CHECK(m, name, m->ntr > 0 && cf_buffer_is(m->c->ui, text), "nothing drawn");
  bool plain = true;
  for (size_t k = 0; k < m->ntr; k++) {
    if (m->tr[k].flags != 0 || m->tr[k].has_bg || m->tr[k].st.font != CCORD_FONT_REGULAR) plain = false;
  }
  CF_CHECK(m, name, plain, "markers without a partner were styled");
  return 0;
}

static int cf_check_wrap(cf_slice *m)
{
  ccord_ui *ui = m->c->ui;
  static const char k_long[] =
    "alpha **bold words in the composer** beta `some inline code` gamma ~~struck out words~~ delta ||secret stuff|| "
    "epsilon __underlined line of words__ and *italic text that goes on* then thiswordisverylong **boldlongwordwithoutspaces** "
    "zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega alpha **beta** gamma "
    "delta `epsilon` zeta ~~eta~~ theta ||iota|| kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega";
  cf_enter_text(m, k_long);
  size_t len = strlen(k_long);
  CF_CHECK(m, "long text buffer", cf_buffer_is(ui, k_long), "the composer buffer changed");
  CF_CHECK(m, "long text wrapped", ui_composer_format_line_count() >= 2, "the long text did not wrap onto 2+ lines");
  float wrap = cf_wrap(ui);
  const char *text = ccord_text_edit_cstr(&ui->edit);
  // Every drawn run advanced the line by the width of its text in the style it was painted with, and runs touch.
  const char *dtext = ccord_text_edit_cstr(&ui->edit);
  for (size_t k = 0; k < m->ntr; k++) {
    float want = ccord_text_measure(m->c->renderer, dtext + m->tr[k].start, m->tr[k].len, &m->tr[k].st, 0.0f).x;
    CF_CHECK(m, "run width is the drawn font's", fabsf(m->tr[k].w - want) < 0.6f, "a run was laid out with another font than it is drawn with");
    if (k > 0 && m->tr[k].y == m->tr[k - 1].y && m->tr[k].start == m->tr[k - 1].start + m->tr[k - 1].len) {
      CF_CHECK(m, "runs touch", fabsf(m->tr[k].x - (m->tr[k - 1].x + m->tr[k - 1].w)) < 0.01f, "a gap or overlap between runs");
    }
  }
  // Mouse hit-testing through the real input path, caret and IME rect: every few offsets plus all style edges.
  size_t step = 5;
  size_t clicked = 0;
  for (size_t pos = 0; pos <= len; pos += (pos + step > len && pos < len) ? len - pos : step) {
    ccord_vec2 o = cf_origin(ui);
    ccord_rect cr = ui_composer_format_caret(ui, m->c->renderer, text, len, wrap, pos);
    cf_click(m, o.x + cr.x + 0.3f, o.y + cr.y + cr.h * 0.5f, 0);
    if (ui->edit.caret != pos || ui->edit.anchor != pos) {
      char why[128];
      snprintf(why, sizeof why, "click at the caret of offset %zu landed on %zu", pos, ui->edit.caret);
      return cf_fail("click hits the glyph", why);
    }
    m->checks++;
    // The IME candidate rect follows the drawn caret.
    ccord_vec2 o2 = cf_origin(ui);
    ccord_rect cr2 = ui_composer_format_caret(ui, m->c->renderer, text, len, wrap, pos);
    bool ime_ok = fabsf(ui->text_area.x - (o2.x + cr2.x)) < 0.6f && fabsf(ui->text_area.y - (o2.y + cr2.y)) < 0.6f;
    CF_CHECK(m, "IME rect at the caret", ime_ok, "the IME rect is not at the drawn caret");
    clicked++;
    if (pos == len) break;
  }
  CF_CHECK(m, "clicked many offsets", clicked > 20, "too few offsets were clicked");
  // Glyph boundary of the drawn run: x measured with the run's own recorded font.
  size_t bold_at = (size_t)(strstr(k_long, "bold words") - k_long) + 8;   // inside the bold span
  const ui_cf_trace_piece *bp = cf_piece_at(m, bold_at);
  CF_CHECK(m, "bold run drawn bold", bp && bp->st.font == CCORD_FONT_BOLD, "the run holding the offset is not drawn bold");
  if (bp) {
    ccord_rect cr = ui_composer_format_caret(ui, m->c->renderer, text, len, wrap, bold_at);
    float want = bp->x + ccord_text_measure(m->c->renderer, text + bp->start, bold_at - bp->start, &bp->st, 0.0f).x;
    CF_CHECK(m, "caret on the bold glyph edge", fabsf(cr.x - want) <= 1.6f && fabsf(cr.y - bp->y) < 0.6f,
             "the caret is not at the glyph boundary of the bold run");
  }
  // Selection by shift+click: its rects start and end on glyph boundaries.
  size_t from = (size_t)(strstr(k_long, "beta") - k_long), to = (size_t)(strstr(k_long, "epsilon __") - k_long);
  ccord_vec2 o = cf_origin(ui);
  ccord_rect ca = ui_composer_format_caret(ui, m->c->renderer, text, len, wrap, from);
  cf_click(m, o.x + ca.x + 0.3f, o.y + ca.y + ca.h * 0.5f, 0);
  o = cf_origin(ui);
  ccord_rect cb = ui_composer_format_caret(ui, m->c->renderer, text, len, wrap, to);
  cf_click(m, o.x + cb.x + 0.3f, o.y + cb.y + cb.h * 0.5f, CCORD_MOD_SHIFT);
  CF_CHECK(m, "shift-click selects", ui->edit.anchor == from && ui->edit.caret == to, "shift+click did not select the range");
  ui_cf_sel_rect rects[64];
  size_t n = ui_composer_format_selection(ui, m->c->renderer, text, len, wrap, from, to, rects, 64);
  CF_CHECK(m, "selection rects", n >= 2, "a selection over wrapped lines gives one rect per line");
  ccord_rect first = ui_composer_format_caret(ui, m->c->renderer, text, len, wrap, from);
  CF_CHECK(m, "selection starts at the first glyph", fabsf(rects[0].rc.x - first.x) < 0.51f && fabsf(rects[0].rc.y - first.y) < 0.51f,
           "the first selection rect does not start at the first selected glyph");
  ccord_rect last = ui_composer_format_caret(ui, m->c->renderer, text, len, wrap, to);
  size_t lastn = n;
  while (lastn > 0 && rects[lastn - 1].stub) lastn--;
  CF_CHECK(m, "selection ends at the last glyph", lastn > 0 && fabsf(rects[lastn - 1].rc.x + rects[lastn - 1].rc.w - last.x) < 0.51f &&
                                                     fabsf(rects[lastn - 1].rc.y - last.y) < 0.51f,
           "the last selection rect does not end at the last selected glyph");
  m->c->frames(m->c->ud, 2);
  CF_CHECK(m, "buffer survived clicking", cf_buffer_is(ui, k_long), "clicking changed the buffer");
  return 0;
}

static int cf_check_blocks(cf_slice *m)
{
  static const char src[] = "```\ncode **x**\n```\n> quote *i*\nplain";
  cf_enter_text(m, src);
  CF_CHECK(m, "block buffer", cf_buffer_is(m->c->ui, src), "the composer buffer changed");
  const ui_cf_trace_piece *open = cf_piece_at(m, 0);
  CF_CHECK(m, "fence marker line", open && (open->flags & UI_CF_BLOCK) && (open->flags & UI_CF_MARKER) && open->has_bg,
           "the opening fence is not dimmed on a code background");
  const ui_cf_trace_piece *code = cf_piece_of(m, src, "code");
  CF_CHECK(m, "fenced code line", code && (code->flags & UI_CF_BLOCK) && (code->flags & UI_CF_CODE) && code->has_bg &&
                                      code->st.size < m->c->ui->theme->m.font_size_base,
           "a code line has no code style and background");
  const ui_cf_trace_piece *star = cf_piece_of(m, src, "**x");
  CF_CHECK(m, "no markdown inside a fence", star && !(star->flags & UI_CF_BOLD), "markers inside a code block were styled");
  const ui_cf_trace_piece *qm = cf_piece_of(m, src, "> quote");
  const ui_cf_trace_piece *qt = cf_piece_of(m, src, "quote");
  CF_CHECK(m, "quote marker dimmed", cf_dim(m->c->ui, qm) && (qm->flags & UI_CF_QUOTE), "the > marker is not dimmed");
  CF_CHECK(m, "quote line indented", qt && (qt->flags & UI_CF_QUOTE) && qt->x >= 12.0f, "a quote line has no bar space");
  const ui_cf_trace_piece *qi = cf_piece_of(m, src, "i*\n");
  CF_CHECK(m, "inline style inside a quote", qi && (qi->flags & UI_CF_ITALIC), "styling inside a quote was lost");
  const ui_cf_trace_piece *pl = cf_piece_of(m, src, "plain");
  CF_CHECK(m, "plain line unchanged", pl && pl->flags == 0 && pl->x == 0.0f, "a plain line was indented or styled");
  if (cf_check_unstyled(m, "```\nabc", "unclosed fence")) return 1;
  return 0;
}

static int cf_check_cache(cf_slice *m)
{
  cf_enter_text(m, "one **two** three");
  uint32_t before = ui_composer_format_passes();
  m->c->frames(m->c->ud, 6);
  CF_CHECK(m, "no restyle on idle frames", ui_composer_format_passes() == before,
           "the composer was laid out again on frames where nothing changed");
  cf_type(m, "!");
  m->c->frames(m->c->ud, 2);
  CF_CHECK(m, "restyle after typing", ui_composer_format_passes() > before, "a typed character did not relayout");
  return 0;
}

// ---- posted vs composer: the same text must get the same styled characters in both places.

#define CF_SEQ_MAX 256
typedef struct cf_seq {
  char ch[CF_SEQ_MAX];
  unsigned fl[CF_SEQ_MAX];
  size_t n;
} cf_seq;

static void cf_seq_add(cf_seq *q, char c, unsigned fl, bool drop_backslash)
{
  if (c == ' ' || c == '\t' || c == '\n' || (drop_backslash && c == '\\') || q->n >= CF_SEQ_MAX) return;
  q->ch[q->n] = c;
  q->fl[q->n] = fl;
  q->n++;
}

// What the composer drew for `src` (dimmed marker bytes are not text, a code span is flagged as code only).
static cf_seq cf_composer_seq(const cf_slice *m, const char *src, bool drop_backslash)
{
  cf_seq q = {0};
  const unsigned mask = UI_CF_BOLD | UI_CF_ITALIC | UI_CF_UNDER | UI_CF_STRIKE | UI_CF_CODE | UI_CF_SPOILER;
  for (size_t i = 0; i < m->ntr; i++) {
    const ui_cf_trace_piece *p = &m->tr[i];
    unsigned fl = p->flags & mask;
    if (fl & UI_CF_CODE) fl = UI_CF_CODE;
    if (p->flags & UI_CF_MARKER) continue;
    for (size_t k = 0; k < p->len; k++) cf_seq_add(&q, src[p->start + k], fl, drop_backslash);
  }
  return q;
}

// What the posted row drew: every non-space token the walker handed to the draw, in order.
static cf_seq cf_posted_seq(const ccord_ui *ui)
{
  cf_seq q = {0};
  for (size_t i = 0; i < ui->ndbg_md_runs; i++) {
    for (const char *c = ui->dbg_md_runs[i].text; *c; c++) cf_seq_add(&q, *c, ui->dbg_md_runs[i].flags, false);
  }
  return q;
}

static int cf_check_posted_one(cf_slice *m, const char *src, bool has_escape, const char *want_text)
{
  ccord_ui *ui = m->c->ui;
  char name[160];
  cf_enter_text(m, src);
  cf_seq comp = cf_composer_seq(m, src, has_escape);
  const ccord_id ch = ccord_store_selected_channel(m->c->store);
  size_t n0 = ccord_store_message_count(m->c->store, ch);
  cf_key(m, CCORD_KEY_RETURN, 0);
  const ccord_message *msg = nullptr;
  for (int i = 0; i < 40; i++) {
    m->c->frames(m->c->ud, 2);
    size_t n = ccord_store_message_count(m->c->store, ch);
    msg = n > n0 ? ccord_store_message_at(m->c->store, ch, n - 1) : nullptr;
    if (msg && msg->send_state == CCORD_MSG_SEND_CONFIRMED && msg->content && strcmp(msg->content, src) == 0) break;
    msg = nullptr;
  }
  snprintf(name, sizeof name, "posted '%.40s' arrives", src);
  CF_CHECK(m, name, msg != nullptr, "the message never came back from the mock");
  ui->dbg_md_msg = msg->id;
  m->c->frames(m->c->ud, 3);
  cf_seq post = cf_posted_seq(ui);
  ui->dbg_md_msg = CCORD_ID_NONE;
  bool same = post.n == comp.n && post.n > 0;
  for (size_t i = 0; same && i < post.n; i++) same = post.ch[i] == comp.ch[i] && post.fl[i] == comp.fl[i];
  snprintf(name, sizeof name, "posted '%.40s' styles like the composer", src);
  CF_CHECK(m, name, same, "the posted row's styled characters differ from the composer's");
  if (want_text) {
    char got[CF_SEQ_MAX + 1];
    for (size_t i = 0; i < post.n; i++) got[i] = post.ch[i];
    got[post.n] = '\0';
    snprintf(name, sizeof name, "posted '%.40s' draws '%s'", src, want_text);
    CF_CHECK(m, name, strcmp(got, want_text) == 0, got);
  }
  return 0;
}

static int cf_check_posted(cf_slice *m)
{
  struct { const char *src; bool escape; const char *want; unsigned any; } k[] = {
    {"**abc", false, "**abc", 0},
    {"*abc", false, "*abc", 0},
    {"\\*abc\\*", true, "*abc*", 0},
    {"_abc_", false, "abc", UI_CF_ITALIC},
    {"snake_case_name", false, "snake_case_name", 0},
    {"**b** *i* __u__ ~~s~~ `c` ||p||", false, "bius" "cp", UI_CF_BOLD | UI_CF_ITALIC | UI_CF_UNDER | UI_CF_STRIKE | UI_CF_CODE | UI_CF_SPOILER},
    {"go https://x.com/a_b_/__c__ now", false, "gohttps://x.com/a_b_/__c__now", 0},
    {"**ok** and *open ~~done~~ __x", false, "okand*open" "done__x", UI_CF_BOLD | UI_CF_STRIKE},
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
    if (cf_check_posted_one(m, k[i].src, k[i].escape, k[i].want)) return 1;
    // the samples that must style something do: posted text has the expected kinds of run
    if (k[i].any) {
      cf_seq p = cf_composer_seq(m, k[i].src, k[i].escape);
      unsigned seen = 0;
      for (size_t j = 0; j < p.n; j++) seen |= p.fl[j];
      CF_CHECK(m, "styled sample really is styled", (seen & k[i].any) == k[i].any, "the sample drew without its styles");
    }
  }
  return 0;
}

static int cf_shot(cf_slice *m)
{
  ccord_ui *ui = m->c->ui;
  static const char k_post[] = "Typing **bold** *italic* __under__ ~~strike~~ `code` ||spoiler|| in one line\n> a quote line\n```\nint x = 1;\n```";
  const ccord_id ch = ccord_store_selected_channel(m->c->store);
  size_t n0 = ccord_store_message_count(m->c->store, ch);
  cf_enter_text(m, k_post);
  cf_key(m, CCORD_KEY_RETURN, 0);
  bool posted = false;
  for (int i = 0; i < 40 && !posted; i++) {
    m->c->frames(m->c->ud, 2);
    size_t n = ccord_store_message_count(m->c->store, ch);
    const ccord_message *msg = n > n0 ? ccord_store_message_at(m->c->store, ch, n - 1) : nullptr;
    posted = msg && msg->send_state == CCORD_MSG_SEND_CONFIRMED && msg->content && strcmp(msg->content, k_post) == 0;
  }
  CF_CHECK(m, "sent bytes are the typed text", posted, "the message handed to the session differs from the typed text");
  CF_CHECK(m, "composer empty after send", ui->edit.len == 0, "the composer kept text after sending");
  cf_enter_text(m, k_post);
  CF_CHECK(m, "retyped buffer", cf_buffer_is(ui, k_post), "the composer buffer changed");
  m->c->frames(m->c->ud, 4);

  struct stat sb;
  const char *path = (stat("docs/screenshots", &sb) == 0 && S_ISDIR(sb.st_mode)) ? CF_SHOT : "composer-format.png";
  const char *envp = getenv("CCORD_COMPOSER_FORMAT_SHOT");
  if (envp && envp[0]) path = envp;
  ccord_render_begin(m->c->renderer, (ccord_color){0x31, 0x33, 0x38, 255});
  ccord_ui_draw(ui, m->c->renderer);
  ccord_error err;
  ccord_status st = ccord_render_screenshot(m->c->renderer, path, &err);
  ccord_render_end(m->c->renderer);
  CF_CHECK(m, "screenshot written", st == CCORD_OK, err.message);
  printf("composer-format: screenshot %s\n", path);
  return 0;
}

int ccord_verify_composer_format_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  cf_slice m = {.c = ctx};
  ctx->frames(ctx->ud, 4);
  if (ccord_store_selected_channel(ctx->store) == CCORD_ID_NONE) return cf_fail("setup", "no channel is selected");
  int rc = cf_check_spans(&m);
  if (!rc) rc = cf_check_unstyled(&m, "**abc", "unclosed bold");
  if (!rc) rc = cf_check_unstyled(&m, "\\*abc\\*", "escaped markers");
  if (!rc) rc = cf_check_unstyled(&m, "snake_case_name 2 * 3 * 4 ~~x ||y `z", "stray markers");
  if (!rc) rc = cf_check_wrap(&m);
  if (!rc) rc = cf_check_blocks(&m);
  if (!rc) rc = cf_check_cache(&m);
  if (!rc) rc = cf_check_posted(&m);
  if (!rc) rc = cf_shot(&m);
  if (rc) return rc;
  cf_clear(&m);
  printf("composer-format: PASS checks=%d\n", m.checks);
  fflush(stdout);
  return 0;
}
