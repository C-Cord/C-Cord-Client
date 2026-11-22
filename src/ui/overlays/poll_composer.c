// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Poll composer modal: question, 2..10 answers, duration, "Allow multiple answers", Post / Cancel.
//
// One composer exists at a time, so its state lives here (allocated when the modal opens, freed when it closes) instead of
// in ccord_ui. Post builds a ccord_poll_create from the trimmed, non-empty texts and sends it through the session
// (ccord_session_send_poll -> POST /channels/{id}/messages {"poll":...}). A call that fails keeps the modal open with the
// error under the form; a refusal Discord sends after the request was queued comes back through ui_poll_composer_on_refused, which reopens
// the form as posted (the posted form is kept as g_posted until confirmed or refused).
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/utf8.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define PC_MAX_ANSWERS CCORD_RICH_MAX_POLL_ANSWERS
#define PC_START_ANSWERS 2u
#define PC_NDUR 7u
#define PC_DEFAULT_DUR 3u   // 24 hours
#define PC_TEXT_BYTES 4u    // a code point is at most 4 UTF-8 bytes

static const struct {
  const char *label;
  uint32_t hours;
} k_durations[PC_NDUR] = {
  {"1 hour", 1}, {"4 hours", 4}, {"8 hours", 8}, {"24 hours", 24}, {"3 days", 72}, {"1 week", 168}, {"2 weeks", 336},
};

typedef struct pc_state {
  ccord_text_edit question;
  ccord_text_edit answers[PC_MAX_ANSWERS];
  size_t nanswers;
  size_t focus;   // 0 = question, 1..nanswers = answers
  size_t dur;
  bool multi;
  ccord_id channel;   // channel a refused poll was first posted to (repost target); 0 = the selected channel
  bool refused;       // reopened after Discord refused the accepted poll
  char target[160];   // "Posting to ..." line for the channel it will post to; empty when that is the selected channel
  char error[200];
  // texts handed to the transport: trimmed copies of the non-empty fields, valid until the next build
  char qtext[CCORD_POLL_QUESTION_MAX * PC_TEXT_BYTES + 1];
  char atext[PC_MAX_ANSWERS][CCORD_POLL_ANSWER_MAX * PC_TEXT_BYTES + 1];
  const char *aptr[PC_MAX_ANSWERS];
  // geometry of the last draw (input and the verify slice read it)
  ccord_rect box, body_view, question_rect, answer_rect[PC_MAX_ANSWERS], remove_rect[PC_MAX_ANSWERS], add_rect, multi_rect,
    dur_rect[PC_NDUR], post_rect, cancel_rect;
  float scroll, max_scroll, content_h;
} pc_state;

static pc_state *g_pc;
// The last poll posted, kept (one only) until Discord confirms it or the user gives up: a refusal after acceptance reopens it.
static pc_state *g_posted;
static size_t g_live_states;

// ---- state --------------------------------------------------------------------------------------------------------

static void pc_free(pc_state *s)
{
  if (!s) return;
  g_live_states--;
  ccord_text_edit_free(&s->question);
  for (size_t i = 0; i < PC_MAX_ANSWERS; i++) ccord_text_edit_free(&s->answers[i]);
  ccord_free(s);
}

static pc_state *pc_new(void)
{
  pc_state *s = ccord_calloc(1, sizeof *s);
  if (!s) return nullptr;
  g_live_states++;
  ccord_text_edit_init(&s->question, CCORD_POLL_QUESTION_MAX * PC_TEXT_BYTES);
  for (size_t i = 0; i < PC_MAX_ANSWERS; i++) ccord_text_edit_init(&s->answers[i], CCORD_POLL_ANSWER_MAX * PC_TEXT_BYTES);
  s->nanswers = PC_START_ANSWERS;
  s->dur = PC_DEFAULT_DUR;
  return s;
}

static size_t pc_cps(const char *s, size_t len)
{
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char)s[i] & 0xC0u) != 0x80u) n++;
  }
  return n;
}

static ccord_text_edit *pc_field(pc_state *s, size_t idx) { return idx == 0 ? &s->question : &s->answers[idx - 1u]; }

static size_t pc_limit(size_t idx) { return idx == 0 ? CCORD_POLL_QUESTION_MAX : CCORD_POLL_ANSWER_MAX; }

// Copies `src` trimmed of ASCII whitespace at both ends into `dst` (cap bytes); returns the trimmed length.
static size_t pc_trim_copy(char *dst, size_t cap, const char *src, size_t len)
{
  size_t b = 0;
  while (b < len && (src[b] == ' ' || src[b] == '\t' || src[b] == '\n' || src[b] == '\r')) b++;
  while (len > b && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\n' || src[len - 1] == '\r')) len--;
  size_t n = len - b;
  if (n >= cap) n = cap - 1u;
  memcpy(dst, src + b, n);
  dst[n] = '\0';
  return n;
}

// Fills `out` from the form: the trimmed question and the non-empty answers in order. Returns false when the question is empty or fewer
// than CCORD_POLL_MIN_ANSWERS answers are filled in (the Post button stays disabled).
static bool pc_request(pc_state *s, ccord_poll_create *out)
{
  const ccord_text_edit *q = &s->question;
  size_t qn = pc_trim_copy(s->qtext, sizeof s->qtext, q->data ? q->data : "", q->len);
  size_t n = 0;
  for (size_t i = 0; i < s->nanswers; i++) {
    const ccord_text_edit *a = &s->answers[i];
    if (pc_trim_copy(s->atext[n], sizeof s->atext[n], a->data ? a->data : "", a->len) == 0) continue;
    s->aptr[n] = s->atext[n];
    n++;
  }
  *out = (ccord_poll_create){
    .question = s->qtext,
    .answers = s->aptr,
    .answer_count = n,
    .duration_hours = k_durations[s->dur].hours,
    .allow_multiselect = s->multi,
  };
  return qn > 0 && n >= CCORD_POLL_MIN_ANSWERS;
}

static bool pc_can_post(pc_state *s)
{
  ccord_poll_create req;
  return pc_request(s, &req);
}

// ---- open / close -------------------------------------------------------------------------------------------------

void ui_poll_composer_open(ccord_ui *ui)
{
  if (!ui) return;
  if (!g_pc) g_pc = pc_new();
  if (!g_pc) return;
  ui->poll_open = true;
  ui->dirty = true;
}

void ui_poll_composer_close(ccord_ui *ui)
{
  if (!ui) return;
  ui->poll_open = false;
  pc_free(g_pc);
  g_pc = nullptr;
  ui->dirty = true;
}

bool ui_poll_composer_is_open(const ccord_ui *ui) { return ui && ui->poll_open && g_pc; }

// ccord_ui_destroy: frees the open form and the kept draft.
void ui_poll_composer_destroy(ccord_ui *ui)
{
  if (ui) ui->poll_open = false;
  pc_free(g_pc);
  g_pc = nullptr;
  pc_free(g_posted);
  g_posted = nullptr;
}

size_t ui_poll_composer_live_states(void) { return g_live_states; }

// A poll Discord refused after accepting the request: reopens the form as posted, with `reason` under it. False (nothing
// reopened, the draft dropped) when there is no draft or the user already has another form open; the notice still shows.
bool ui_poll_composer_on_refused(ccord_ui *ui, const char *reason)
{
  if (!ui || !g_posted) return false;
  if (g_pc) {
    pc_free(g_posted);
    g_posted = nullptr;
    return false;
  }
  g_pc = g_posted;
  g_posted = nullptr;
  g_pc->refused = true;
  (void)snprintf(g_pc->error, sizeof g_pc->error, "%s", reason && reason[0] ? reason : "Discord refused the poll.");
  ui->poll_open = true;
  ui->dirty = true;
  return true;
}

// Discord's own copy of the poll arrived (MESSAGE_CREATE by us in `channel`): the draft is no longer needed.
void ui_poll_composer_on_created(ccord_ui *ui, ccord_id channel_id)
{
  (void)ui;
  if (!g_posted || g_posted->channel != channel_id) return;
  pc_free(g_posted);
  g_posted = nullptr;
}

// ---- post ---------------------------------------------------------------------------------------------------------

static void pc_set_error(pc_state *s, const char *msg) { (void)snprintf(s->error, sizeof s->error, "%s", msg ? msg : ""); }

static void pc_post(ccord_ui *ui)
{
  pc_state *s = g_pc;
  ccord_poll_create req;
  if (!pc_request(s, &req)) {
    pc_set_error(s, "Add a question and at least two answers.");
    ui->dirty = true;
    return;
  }
  const ccord_channel *ch = ui_selected_channel(ui);
  ccord_id target = s->channel != CCORD_ID_NONE ? s->channel : (ch ? ch->id : CCORD_ID_NONE);   // a repost keeps its channel
  if (!ui->d.session || target == CCORD_ID_NONE) {
    pc_set_error(s, "Open a channel to post the poll in.");
    ui->dirty = true;
    return;
  }
  ccord_error err;
  ccord_error_clear(&err);
  if (ccord_session_send_poll(ui->d.session, target, &req, &err) != CCORD_OK) {
    pc_set_error(s, err.message[0] ? err.message : "Could not post the poll.");
    ui->dirty = true;
    return;
  }
  // Accepted: keep the form as the one last-posted draft in case Discord refuses it later.
  s->channel = target;
  s->error[0] = '\0';
  pc_free(g_posted);
  g_posted = s;
  g_pc = nullptr;
  ui->poll_open = false;
  ui->dirty = true;
}

static void pc_add_answer(pc_state *s)
{
  if (s->nanswers >= PC_MAX_ANSWERS) return;
  ccord_text_edit_clear(&s->answers[s->nanswers]);
  s->nanswers++;
  s->focus = s->nanswers;
}

static void pc_remove_answer(pc_state *s, size_t i)
{
  if (s->nanswers <= PC_START_ANSWERS || i >= s->nanswers) return;
  ccord_text_edit gone = s->answers[i];
  for (size_t k = i; k + 1u < s->nanswers; k++) s->answers[k] = s->answers[k + 1u];
  s->answers[s->nanswers - 1u] = gone;   // keep the buffer, now unused, at the end
  ccord_text_edit_clear(&s->answers[s->nanswers - 1u]);
  s->nanswers--;
  if (s->focus > s->nanswers) s->focus = s->nanswers;
  else if (s->focus > i + 1u) s->focus--;
}

// ---- layout & draw ------------------------------------------------------------------------------------------------

static bool pc_hover(const ccord_ui *ui, ccord_rect r) { return ui->mouse_in && ui_in_rect(ui->mx, ui->my, r); }

// Names the channel a poll will be posted to, when that is not the selected one or the form is a reopened refusal.
static void pc_target_text(const ccord_ui *ui, pc_state *s)
{
  s->target[0] = '\0';
  if (s->channel == CCORD_ID_NONE) return;
  if (!s->refused && s->channel == ccord_store_selected_channel(ui->d.store)) return;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, s->channel);
  if (!ch) {
    (void)snprintf(s->target, sizeof s->target, "Posting to the channel it was first posted in");
  } else if (ch->guild_id == CCORD_DM_GUILD_ID) {
    (void)snprintf(s->target, sizeof s->target, "Posting to %s", ui_dm_display_name(ui, ch));
  } else {
    (void)snprintf(s->target, sizeof s->target, "Posting to #%s", ch->name ? ch->name : "");
  }
}

static void pc_layout(ccord_ui *ui, ccord_renderer *r, pc_state *s)
{
  ccord_text_style over = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  float label_h = ui_line_h(ui, over.size) + UIS_SP_1;
  ccord_text_style tit = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  float title_h = UIS_SP_5 + ui_line_h(ui, tit.size) + UIS_SP_3;
  pc_target_text(ui, s);
  float est_w = uis_modal_box(ui, UIS_MODAL_W_MD, 200.0f).w - 2.0f * UIS_SP_5;
  bool wrap_dur = ((est_w - UIS_SP_1 * (float)(PC_NDUR - 1u)) / (float)PC_NDUR) < 48.0f;
  float dur_h = wrap_dur ? (UIS_H_BTN_SM * 2.0f + UIS_SP_1) : UIS_H_BTN_SM;
  float foot_extra = ui_line_h(ui, M(font_size_small)) + UIS_SP_2
                     + (s->target[0] ? ui_line_h(ui, M(font_size_small)) + UIS_SP_2 : 0.0f)
                     + UIS_SP_5 + UIS_H_BTN + UIS_SP_5;
  float body_fixed = (label_h + UIS_H_INPUT + UIS_SP_3)   // question
                     + label_h                            // answers label
                     + UIS_H_BTN_SM + UIS_SP_3            // add answer
                     + (label_h + dur_h + UIS_SP_3)       // duration
                     + UIS_H_BTN + UIS_SP_2;              // toggle
  float maxh = ui->win_h - 2.0f * UIS_SP_8;
  float row_h = UIS_H_INPUT;
  size_t n = s->nanswers;
  if (!ui->mobile_layout) {
    float per = (maxh - (title_h + body_fixed + foot_extra)) / (float)n - UIS_SP_1;
    if (per < row_h) row_h = per < 24.0f ? 24.0f : per;
  }
  float body_h = body_fixed + (float)n * (row_h + UIS_SP_1);
  float h = title_h + body_h + foot_extra;
  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_MD, h);
  s->box = box;
  s->post_rect = uis_modal_footer_button(box, 0, 96.0f);
  s->cancel_rect = uis_modal_footer_button(box, 1, 96.0f);

  float body_bottom = s->post_rect.y - UIS_SP_3
                      - (s->error[0] ? ui_line_h(ui, M(font_size_small)) + UIS_SP_2 : 0.0f)
                      - (s->target[0] ? ui_line_h(ui, M(font_size_small)) + UIS_SP_2 : 0.0f);
  float x = box.x + UIS_SP_5, w = box.w - 2.0f * UIS_SP_5;
  s->body_view = (ccord_rect){box.x + UIS_SP_2, box.y + title_h, box.w - 2.0f * UIS_SP_2,
                              fmaxf(body_bottom - (box.y + title_h), 0.0f)};
  s->content_h = body_h;
  s->max_scroll = body_h > s->body_view.h ? (body_h - s->body_view.h) : 0.0f;
  s->scroll = fminf(fmaxf(s->scroll, 0.0f), s->max_scroll);

  float y = box.y + title_h - s->scroll + label_h;
  s->question_rect = (ccord_rect){x, y, w, UIS_H_INPUT};
  y += UIS_H_INPUT + UIS_SP_3 + label_h;
  for (size_t i = 0; i < PC_MAX_ANSWERS; i++) {
    s->answer_rect[i] = s->remove_rect[i] = (ccord_rect){0, 0, 0, 0};
  }
  for (size_t i = 0; i < n; i++) {
    s->answer_rect[i] = (ccord_rect){x, y, w - row_h - UIS_SP_1, row_h};
    s->remove_rect[i] = (ccord_rect){x + w - row_h, y, row_h, row_h};
    y += row_h + UIS_SP_1;
  }
  float aw = uis_label_width(ui, r, "+ Add Answer");
  s->add_rect = (ccord_rect){x, y, aw, UIS_H_BTN_SM};
  y += UIS_H_BTN_SM + UIS_SP_3 + label_h;
  float gap = UIS_SP_1;
  if (wrap_dur) {
    size_t r0 = 4u, r1 = PC_NDUR - r0;
    float cw0 = (w - gap * (float)(r0 - 1u)) / (float)r0;
    float cw1 = (w - gap * (float)(r1 - 1u)) / (float)r1;
    for (size_t i = 0; i < r0; i++) {
      s->dur_rect[i] = (ccord_rect){x + (float)i * (cw0 + gap), y, cw0, UIS_H_BTN_SM};
    }
    for (size_t i = 0; i < r1; i++) {
      s->dur_rect[r0 + i] = (ccord_rect){x + (float)i * (cw1 + gap), y + UIS_H_BTN_SM + gap, cw1, UIS_H_BTN_SM};
    }
  } else {
    float cw = (w - gap * (float)(PC_NDUR - 1u)) / (float)PC_NDUR;
    for (size_t i = 0; i < PC_NDUR; i++) {
      s->dur_rect[i] = (ccord_rect){x + (float)i * (cw + gap), y, cw, UIS_H_BTN_SM};
    }
  }
  y += dur_h + UIS_SP_3;
  s->multi_rect = (ccord_rect){x, y, w, UIS_H_BTN};
}

static void pc_draw_label(ccord_ui *ui, ccord_renderer *r, float x, float y, const char *text)
{
  ccord_text_style over = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw(r, text, strlen(text), x, y, &over, 0.0f);
}

void ui_poll_composer_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui_poll_composer_is_open(ui)) return;
  pc_state *s = g_pc;
  ui_popup_begin(ui, r, "pollcomposer", 0, UI_MODAL_MS, UIS_SLIDE_MODAL);
  pc_layout(ui, r, s);
  ccord_rect box = s->box;
  uis_modal_frame(ui, r, box);
  (void)uis_modal_title(ui, r, box, "Create Poll");

  ccord_text_style over = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  float label_h = ui_line_h(ui, over.size) + UIS_SP_1;
  float x = box.x + UIS_SP_5, w = box.w - 2.0f * UIS_SP_5;

  ccord_push_clip(r, s->body_view);
  // question
  float ly = s->question_rect.y - label_h;
  pc_draw_label(ui, r, x, ly, "QUESTION");
  char cnt[24];
  (void)snprintf(cnt, sizeof cnt, "%zu/%u", pc_cps(s->question.data ? s->question.data : "", s->question.len),
                 CCORD_POLL_QUESTION_MAX);
  ccord_vec2 cm = ccord_text_measure(r, cnt, strlen(cnt), &over, 0.0f);
  ccord_text_draw(r, cnt, strlen(cnt), x + w - cm.x, ly, &over, 0.0f);
  (void)uis_input(ui, r, s->question_rect, &s->question, "What question do you want to ask?",
                  (uis_state){.focused = s->focus == 0, .hover = pc_hover(ui, s->question_rect)}, false, "pollq", 0);

  // answers
  pc_draw_label(ui, r, x, s->answer_rect[0].y - label_h, "ANSWERS");
  for (size_t i = 0; i < s->nanswers; i++) {
    char ph[32];
    (void)snprintf(ph, sizeof ph, "Answer %zu", i + 1u);
    (void)uis_input(ui, r, s->answer_rect[i], &s->answers[i], ph,
                    (uis_state){.focused = s->focus == i + 1u, .hover = pc_hover(ui, s->answer_rect[i])}, false, "polla", i);
    bool can_remove = s->nanswers > PC_START_ANSWERS;
    ccord_rect rr = s->remove_rect[i];
    ccord_color gc = uis_icon_button(ui, r, rr, (uis_state){.hover = can_remove && pc_hover(ui, rr), .disabled = !can_remove},
                                     "pollrm", i);
    ccord_text_style gs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), gc);
    ccord_vec2 gm = ccord_text_measure(r, "x", 1, &gs, 0.0f);
    ccord_text_draw(r, "x", 1, rr.x + (rr.w - gm.x) * 0.5f, rr.y + (rr.h - ui_line_h(ui, gs.size)) * 0.5f, &gs, 0.0f);
  }
  uis_button(ui, r, s->add_rect, "+ Add Answer", UIS_BTN_LINK, UIS_BTN_S_SM,
             (uis_state){.hover = pc_hover(ui, s->add_rect), .disabled = s->nanswers >= PC_MAX_ANSWERS}, "polladd", 0);

  // duration + multiple answers
  pc_draw_label(ui, r, x, s->dur_rect[0].y - label_h, "DURATION");
  for (size_t i = 0; i < PC_NDUR; i++) {
    uis_chip(ui, r, s->dur_rect[i], k_durations[i].label,
             (uis_state){.hover = pc_hover(ui, s->dur_rect[i]), .selected = i == s->dur}, "polldur", i);
  }
  ccord_text_style body = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_header));
  ccord_text_draw(r, "Allow multiple answers", 22, x, s->multi_rect.y + (s->multi_rect.h - ui_line_h(ui, body.size)) * 0.5f,
                  &body, 0.0f);
  uis_toggle(ui, r, s->multi_rect, s->multi, (uis_state){.hover = pc_hover(ui, s->multi_rect)}, "pollmulti", 0);
  ccord_pop_clip(r);
  if (s->max_scroll > 0.0f) {
    (void)ui_draw_list_scrollbar(ui, r, s->body_view, s->content_h, s->scroll, "pollscroll", 0);
  }

  if (s->error[0]) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(danger));
    float ey = s->post_rect.y - UIS_SP_2 - ui_line_h(ui, es.size) - UIS_SP_3;
    ccord_text_draw_ellipsis(r, s->error, strlen(s->error), x, ey, w, &es);
  }
  if (s->target[0]) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    float ty = s->post_rect.y - UIS_SP_2 - ui_line_h(ui, ts.size) - UIS_SP_3;
    if (s->error[0]) ty -= ui_line_h(ui, ts.size) + UIS_SP_2;   // sits above the error line
    ccord_text_draw_ellipsis(r, s->target, strlen(s->target), x, ty, w, &ts);
  }

  uis_button(ui, r, s->cancel_rect, "Cancel", UIS_BTN_LINK, UIS_BTN_S_MD, (uis_state){.hover = pc_hover(ui, s->cancel_rect)},
             "pollcancel", 0);
  uis_button(ui, r, s->post_rect, "Post", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = pc_hover(ui, s->post_rect), .disabled = !pc_can_post(s)}, "pollpost", 0);
  ui_popup_end(r);
}

// ---- input --------------------------------------------------------------------------------------------------------

// Inserts typed or pasted text into the focused field, cutting it at the field's character limit (a selection being
// replaced does not count towards it).
static void pc_insert(pc_state *s, const char *text, size_t len)
{
  ccord_text_edit *ed = pc_field(s, s->focus);
  size_t lo = ed->caret < ed->anchor ? ed->caret : ed->anchor, hi = ed->caret < ed->anchor ? ed->anchor : ed->caret;
  size_t have = pc_cps(ed->data ? ed->data : "", ed->len) - pc_cps(ed->data ? ed->data + lo : "", hi - lo);
  size_t room = pc_limit(s->focus) > have ? pc_limit(s->focus) - have : 0;
  size_t keep = 0, cps = 0;
  while (keep < len) {
    if (((unsigned char)text[keep] & 0xC0u) != 0x80u) {
      if (cps == room) break;
      cps++;
    }
    keep++;
  }
  if (keep == 0) return;
  ccord_error err;
  (void)ccord_text_edit_insert(ed, text, keep, &err);
}

static void pc_focus_step(pc_state *s, int dir)
{
  size_t total = s->nanswers + 1u;
  s->focus = (s->focus + (dir > 0 ? 1u : total - 1u)) % total;
}

static void pc_key(ccord_ui *ui, pc_state *s, const ccord_input_event *ev)
{
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  ccord_text_edit *ed = pc_field(s, s->focus);
  switch (ev->key) {
    case CCORD_KEY_ESCAPE: ui_poll_composer_close(ui); return;
    case CCORD_KEY_TAB: pc_focus_step(s, shift ? -1 : 1); break;
    case CCORD_KEY_RETURN:
      if (ctrl) {
        pc_post(ui);
        return;
      }
      if (s->focus == s->nanswers) pc_add_answer(s);   // Enter in the last answer starts the next one (until the limit)
      else pc_focus_step(s, 1);
      break;
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
    case CCORD_KEY_A:
      if (ctrl) ccord_text_edit_select_all(ed);
      break;
    case CCORD_KEY_V:
      if (ctrl && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          pc_insert(s, clip, strlen(clip));
          ccord_free(clip);
        }
      }
      break;
    default: break;
  }
  ui->dirty = true;
}

static void pc_click(ccord_ui *ui, pc_state *s, float x, float y)
{
  if (ui_in_rect(x, y, s->cancel_rect)) {
    ui->touch_text_focused = false;
    ui_poll_composer_close(ui);
    return;
  }
  if (ui_in_rect(x, y, s->post_rect)) {
    ui->touch_text_focused = false;
    if (pc_can_post(s)) pc_post(ui);
    return;
  }
  if (s->body_view.h > 0.0f && !ui_in_rect(x, y, s->body_view)) return;
  bool focused_input = false;
  if (ui_in_rect(x, y, s->question_rect)) {
    s->focus = 0;
    focused_input = true;
  } else if (ui_in_rect(x, y, s->add_rect)) {
    pc_add_answer(s);
  } else if (ui_in_rect(x, y, s->multi_rect)) {
    s->multi = !s->multi;
  } else {
    for (size_t i = 0; i < PC_NDUR; i++) {
      if (ui_in_rect(x, y, s->dur_rect[i])) s->dur = i;
    }
    for (size_t i = 0; i < s->nanswers; i++) {
      if (ui_in_rect(x, y, s->answer_rect[i])) {
        s->focus = i + 1u;
        focused_input = true;
      } else if (ui_in_rect(x, y, s->remove_rect[i])) {
        pc_remove_answer(s, i);
        break;
      }
    }
  }
  if (ui->touch_mode) {
    ui->touch_text_focused = focused_input;
    if (!focused_input && ui->d.host.dismiss_keyboard) ui->d.host.dismiss_keyboard(ui->d.host.ud);
  }
  s->error[0] = '\0';
  ui->dirty = true;
}

bool ui_poll_composer_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui_poll_composer_is_open(ui)) return false;
  pc_state *s = g_pc;
  switch (ev->type) {
    case CCORD_IN_KEY_DOWN:
      pc_key(ui, s, ev);
      return true;
    case CCORD_IN_TEXT:
      if (ev->text[0] != '\0') {
        pc_insert(s, ev->text, strlen(ev->text));
        s->error[0] = '\0';
        ui->dirty = true;
      }
      return true;
    case CCORD_IN_MOUSE_DOWN:
      if (ev->button == 1) pc_click(ui, s, ev->x, ev->y);
      return true;
    case CCORD_IN_MOUSE_UP:
      return true;
    case CCORD_IN_WHEEL:
      if (s->max_scroll > 0.0f) {
        s->scroll = fminf(fmaxf(s->scroll - ev->wheel * 36.0f, 0.0f), s->max_scroll);
        ui->dirty = true;
      }
      return true;
    default:
      return false;
  }
}

// ---- verify-slice seams -------------------------------------------------------------------------------------------

void ui_poll_composer_debug_get(const ccord_ui *ui, ui_poll_composer_debug *out)
{
  if (!out) return;
  *out = (ui_poll_composer_debug){0};
  if (!ui_poll_composer_is_open(ui)) return;
  pc_state *s = g_pc;
  out->open = true;
  out->answer_count = s->nanswers;
  out->focus = s->focus;
  out->post_enabled = pc_can_post(s);
  out->multi = s->multi;
  out->duration_hours = k_durations[s->dur].hours;
  (void)snprintf(out->error, sizeof out->error, "%s", s->error);
  (void)snprintf(out->target, sizeof out->target, "%s", s->target);
  out->question = s->question_rect;
  out->add = s->add_rect;
  out->multi_toggle = s->multi_rect;
  out->post = s->post_rect;
  out->cancel = s->cancel_rect;
  for (size_t i = 0; i < s->nanswers; i++) {
    out->answers[i] = s->answer_rect[i];
    out->remove[i] = s->remove_rect[i];
  }
  for (size_t i = 0; i < PC_NDUR; i++) out->duration[i] = s->dur_rect[i];
}

ccord_status ui_poll_composer_build_body(const ccord_ui *ui, ccord_buf *out, ccord_error *err)
{
  if (!ui_poll_composer_is_open(ui)) return ccord_error_set(err, CCORD_ERR_INVALID, "poll composer is closed");
  ccord_poll_create req;
  (void)pc_request(g_pc, &req);
  return ccord_poll_build_body(&req, out, err);
}
