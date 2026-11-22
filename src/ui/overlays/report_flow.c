// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Report flow modal: "Report Message" / "Report User".
//
// The report questions come from Discord (GET /reporting/menu/{type}), so nothing about them is hard-coded here: the overlay
// asks the session for the menu, parses it (ccord_report_menu_parse) and walks the node tree. A node shows its header, its
// child rows, and its elements (checkboxes, a dropdown, free text, text, links, a preview, the breadcrumbs), then a footer with
// Back / Close / the node's button. Submit sends the path and the answers; Discord's refusal brings the form back at the node it
// was sent from with Discord's reason under it and every answer still in place.
//
// One flow exists at a time, so its state lives here (allocated on open, freed on close and in ccord_ui_destroy), as the
// poll composer's does. Phases: loading -> node -> submitting -> sent, or load failed (Retry).
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/model/report.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define RF_MAX_SELS 64u
#define RF_MAX_ITEMS 160u
#define RF_MAX_VALUES 64u      // element names one report may carry (ccord_report_build_body caps at 64)
#define RF_MAX_CHOICES 64u     // choices of one element that can be ticked (a 64 bit set)
#define RF_DEFAULT_LIMIT 800u  // free text without a character_limit
#define RF_NAME_BYTES 64u
#define RF_BTN_W 88.0f
#define RF_HELP_URL "https://discord.com/safety/360044103651-reporting-abusive-behavior-to-discord"
#define RF_OUT_OF_DATE "This report form is out of date. Close it and try again."

enum { RF_LOADING = 0, RF_NODE = 1, RF_SUBMITTING = 2, RF_SENT = 3, RF_LOAD_FAILED = 4 };
enum { RFS_HEADER, RFS_SUB, RFS_INFO, RFS_BODY, RFS_LABEL };   // text styles

typedef enum rf_item_kind : uint8_t {
  RFI_TEXT, RFI_CHILD, RFI_CHECK, RFI_OPTION, RFI_EDIT, RFI_LINK, RFI_PREVIEW, RFI_BLOCK,
} rf_item_kind;

// What the user chose on one element of one node; kept while the node is on the path (Back drops the popped node's).
typedef struct rf_sel {
  uint32_t node, el;
  uint64_t bits;        // checkbox: ticked choices
  int pick;             // dropdown: chosen choice, -1 none
  bool is_text;
  size_t limit;         // free text: characters
  ccord_text_edit ed;
} rf_sel;

// One drawn row of the current node. rect is in window coordinates (scroll applied) after the last layout.
typedef struct rf_item {
  rf_item_kind kind;
  uint8_t style;
  bool on;
  size_t idx;           // child / choice index
  uint32_t el;          // element index on the node
  const char *text, *text2;
  ccord_rect rect;
} rf_item;

typedef struct rf_state {
  ui_report_target target;
  ccord_report_menu menu;
  bool have_menu;
  int phase;
  uint32_t path[CCORD_REPORT_MAX_PATH];
  char names[CCORD_REPORT_MAX_PATH][RF_NAME_BYTES];   // the child row clicked to enter path[i] ("" for the root / a Next)
  size_t path_len;
  uint32_t cur;          // the node shown: the path's tail, or the success node once sent
  bool builtin_sent;     // sent, and the menu has no success node
  rf_sel *sels[RF_MAX_SELS];
  size_t nsel;
  int focus;             // index in sels of the focused free-text field, -1 none
  float scroll, content_h, view_h;
  char error[200];
  char crumb[512], preview[320], author[96], blocklabel[128];
  bool click_pending;    // a click in a text field, turned into a caret position at the next draw (needs the renderer)
  float click_x, click_y;
  rf_item items[RF_MAX_ITEMS];
  size_t nitems;
  ccord_rect box, content, primary, back, close, retry, help;
  const char *primary_label;
} rf_state;

static rf_state *g_rf;
static size_t g_live_states;

// ---- state --------------------------------------------------------------------------------------------------------

static void rf_free_sels(rf_state *s)
{
  for (size_t i = 0; i < s->nsel; i++) {
    ccord_text_edit_free(&s->sels[i]->ed);
    ccord_free(s->sels[i]);
    s->sels[i] = nullptr;
  }
  s->nsel = 0;
  s->focus = -1;
}

static void rf_free(rf_state *s)
{
  if (!s) return;
  g_live_states--;
  rf_free_sels(s);
  if (s->have_menu) ccord_report_menu_free(&s->menu);
  ccord_free(s);
}

static rf_state *rf_new(const ui_report_target *t)
{
  rf_state *s = ccord_calloc(1, sizeof *s);
  if (!s) return nullptr;
  g_live_states++;
  s->target = *t;
  s->focus = -1;
  s->phase = RF_LOADING;
  return s;
}

static rf_sel *rf_sel_find(const rf_state *s, uint32_t node, uint32_t el)
{
  for (size_t i = 0; i < s->nsel; i++) {
    if (s->sels[i]->node == node && s->sels[i]->el == el) return s->sels[i];
  }
  return nullptr;
}

static int rf_sel_index(const rf_state *s, const rf_sel *sel)
{
  for (size_t i = 0; i < s->nsel; i++) {
    if (s->sels[i] == sel) return (int)i;
  }
  return -1;
}

static rf_sel *rf_sel_make(rf_state *s, uint32_t node, uint32_t el, const ccord_report_element *e)
{
  rf_sel *sel = rf_sel_find(s, node, el);
  if (sel) return sel;
  if (s->nsel >= RF_MAX_SELS) return nullptr;
  sel = ccord_calloc(1, sizeof *sel);
  if (!sel) return nullptr;
  sel->node = node;
  sel->el = el;
  sel->pick = -1;
  if (e->kind == CCORD_REPORT_EL_FREE_TEXT) {
    sel->is_text = true;
    sel->limit = e->character_limit ? e->character_limit : RF_DEFAULT_LIMIT;
    ccord_text_edit_init(&sel->ed, sel->limit * 4u);
  }
  s->sels[s->nsel++] = sel;
  return sel;
}

// Drops what was chosen on `node` (Back leaves it).
static void rf_sel_drop_node(rf_state *s, uint32_t node)
{
  size_t w = 0;
  for (size_t i = 0; i < s->nsel; i++) {
    rf_sel *sel = s->sels[i];
    if (sel->node == node) {
      ccord_text_edit_free(&sel->ed);
      ccord_free(sel);
    } else {
      s->sels[w++] = sel;
    }
  }
  for (size_t i = w; i < s->nsel; i++) s->sels[i] = nullptr;
  s->nsel = w;
  s->focus = -1;
}

static const ccord_report_node *rf_node(const rf_state *s) { return s->have_menu ? ccord_report_menu_node(&s->menu, s->cur) : nullptr; }

static size_t rf_cps(const char *s, size_t len)
{
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char)s[i] & 0xC0u) != 0x80u) n++;
  }
  return n;
}

static void rf_set_error(rf_state *s, const char *msg) { (void)snprintf(s->error, sizeof s->error, "%s", msg ? msg : ""); }

// The name shown for a user; "this user" when the store does not know them.
static void rf_user_name(const ccord_ui *ui, ccord_id id, char *out, size_t cap)
{
  const ccord_user *u = ui->d.store && id != CCORD_ID_NONE ? ccord_store_user(ui->d.store, id) : nullptr;
  const char *n = u && u->name && u->name[0] ? u->name : (u && u->handle && u->handle[0] ? u->handle : "this user");
  (void)snprintf(out, cap, "%s", n);
}

// Who "Block" applies to: the reported user, or a message's author.
static ccord_id rf_block_target(const ccord_ui *ui, const rf_state *s)
{
  if (s->target.type == CCORD_REPORT_USER) return s->target.user_id;
  if (s->target.user_id != CCORD_ID_NONE) return s->target.user_id;
  const ccord_message *m = ui->d.store ? ccord_store_find_message(ui->d.store, s->target.channel_id, s->target.message_id) : nullptr;
  return m ? m->author_id : CCORD_ID_NONE;
}

// ---- walking the menu ---------------------------------------------------------------------------------------------

static void rf_submit(ccord_ui *ui, rf_state *s);

// Moves to node `id`, remembering `name` (the row clicked) for the breadcrumbs. False, path unchanged and an error shown,
// when the node is not in the menu.
static bool rf_enter(ccord_ui *ui, rf_state *s, uint32_t id, const char *name)
{
  const ccord_report_node *n = s->have_menu ? ccord_report_menu_node(&s->menu, id) : nullptr;
  if (!n) {
    rf_set_error(s, RF_OUT_OF_DATE);
    return false;
  }
  if (s->path_len >= CCORD_REPORT_MAX_PATH) {
    rf_set_error(s, "This report has too many steps. Close it and try again.");
    return false;
  }
  s->path[s->path_len] = id;
  (void)snprintf(s->names[s->path_len], RF_NAME_BYTES, "%s", name ? name : "");
  s->path_len++;
  s->cur = id;
  s->phase = RF_NODE;
  s->error[0] = '\0';
  s->scroll = 0.0f;
  s->focus = -1;
  for (size_t k = 0; k < n->element_count; k++) {
    const ccord_report_element *e = &n->elements[k];
    if (e->kind == CCORD_REPORT_EL_CHECKBOX || e->kind == CCORD_REPORT_EL_DROPDOWN || e->kind == CCORD_REPORT_EL_FREE_TEXT) {
      rf_sel *sel = rf_sel_make(s, id, (uint32_t)k, e);
      if (sel && sel->is_text && s->focus < 0) s->focus = rf_sel_index(s, sel);
    }
  }
  ui->dirty = true;
  if (n->auto_submit) rf_submit(ui, s);
  return true;
}

static void rf_back(ccord_ui *ui, rf_state *s)
{
  if (s->phase != RF_NODE || s->path_len < 2) return;
  rf_sel_drop_node(s, s->path[s->path_len - 1u]);
  s->path_len--;
  s->cur = s->path[s->path_len - 1u];
  s->error[0] = '\0';
  s->scroll = 0.0f;
  const ccord_report_node *n = rf_node(s);
  for (size_t k = 0; n && k < n->element_count && s->focus < 0; k++) {
    rf_sel *sel = rf_sel_find(s, s->cur, (uint32_t)k);
    if (sel && sel->is_text) s->focus = rf_sel_index(s, sel);
  }
  ui->dirty = true;
}

// ---- submit -------------------------------------------------------------------------------------------------------

typedef struct rf_req {
  ccord_report_value vals[RF_MAX_VALUES];
  const char *pool[RF_MAX_VALUES][RF_MAX_CHOICES];
} rf_req;

static bool rf_blank(const char *s)
{
  for (; *s; s++) {
    if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return false;
  }
  return true;
}

// Gathers the answers of every node on the path (later nodes override the same name), builds the request and hands it to the
// session. An error from the call stays on the node as the form's error.
static void rf_submit(ccord_ui *ui, rf_state *s)
{
  if (!ui->d.session || !s->have_menu) return;
  rf_req *rq = ccord_calloc(1, sizeof *rq);
  if (!rq) {
    rf_set_error(s, "Out of memory.");
    return;
  }
  size_t nvals = 0;
  for (size_t i = 0; i < s->path_len; i++) {
    const ccord_report_node *n = ccord_report_menu_node(&s->menu, s->path[i]);
    if (!n) continue;
    for (size_t k = 0; k < n->element_count; k++) {
      const ccord_report_element *e = &n->elements[k];
      if (!e->should_submit || !e->name) continue;
      const rf_sel *sel = rf_sel_find(s, n->id, (uint32_t)k);
      if (!sel) continue;
      const char *vals[RF_MAX_CHOICES];
      size_t nv = 0;
      if (e->kind == CCORD_REPORT_EL_CHECKBOX) {
        for (size_t c = 0; c < e->choice_count && c < RF_MAX_CHOICES; c++) {
          if ((sel->bits >> c) & 1u) vals[nv++] = e->choices[c].value;
        }
      } else if (e->kind == CCORD_REPORT_EL_DROPDOWN) {
        if (sel->pick >= 0 && (size_t)sel->pick < e->choice_count) vals[nv++] = e->choices[sel->pick].value;
      } else if (e->kind == CCORD_REPORT_EL_FREE_TEXT) {
        const char *t = ccord_text_edit_cstr(&sel->ed);
        if (t && !rf_blank(t)) vals[nv++] = t;
      }
      if (nv == 0) continue;
      size_t slot = nvals;
      for (size_t v = 0; v < nvals; v++) {
        if (strcmp(rq->vals[v].name, e->name) == 0) {
          slot = v;
          break;
        }
      }
      if (slot == nvals) {
        if (nvals >= RF_MAX_VALUES) continue;
        nvals++;
      }
      for (size_t v = 0; v < nv; v++) rq->pool[slot][v] = vals[v];
      rq->vals[slot] = (ccord_report_value){.name = e->name, .values = rq->pool[slot], .value_count = nv};
    }
  }
  ccord_report_request req = {
    .type = s->target.type,
    .version = s->menu.version,
    .variant = s->menu.variant,
    .language = s->menu.language[0] ? s->menu.language : nullptr,
    .breadcrumbs = s->path,
    .breadcrumb_count = s->path_len,
    .elements = rq->vals,
    .element_count = nvals,
    .channel_id = s->target.type == CCORD_REPORT_MESSAGE ? s->target.channel_id : CCORD_ID_NONE,
    .message_id = s->target.type == CCORD_REPORT_MESSAGE ? s->target.message_id : CCORD_ID_NONE,
    .reported_user_id = s->target.type == CCORD_REPORT_USER ? s->target.user_id : CCORD_ID_NONE,
  };
  ccord_error err;
  ccord_error_clear(&err);
  if (ccord_session_report_submit(ui->d.session, &req, &err) != CCORD_OK) {
    rf_set_error(s, err.message[0] ? err.message : "Could not send the report.");
  } else {
    s->error[0] = '\0';
    s->phase = RF_SUBMITTING;
  }
  ccord_free(rq);
  ui->dirty = true;
}

static void rf_request_menu(ccord_ui *ui, rf_state *s)
{
  ccord_error err;
  ccord_error_clear(&err);
  if (ccord_session_report_menu_request(ui->d.session, s->target.type, &err) != CCORD_OK) {
    s->phase = RF_LOAD_FAILED;
    rf_set_error(s, err.message[0] ? err.message : "Could not load the report options.");
  } else {
    s->phase = RF_LOADING;
    s->error[0] = '\0';
  }
  ui->dirty = true;
}

// ---- open / close / events ----------------------------------------------------------------------------------------

void ui_report_flow_open(ccord_ui *ui, const ui_report_target *t)
{
  if (!ui || !t || g_rf) return;   // a second open while one is open is ignored
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  if (ccord_session_is_stale(ui->d.session)) {
    ui_notice(ui, "Reconnecting - reporting is available in a moment");
    return;
  }
  ccord_error err;
  ccord_error_clear(&err);
  if (ccord_session_report_menu_request(ui->d.session, t->type, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't open the report form: %s", err.message);
    return;
  }
  g_rf = rf_new(t);
  if (!g_rf) return;
  ui->report_open = true;
  ui->dirty = true;
}

void ui_report_flow_close(ccord_ui *ui)
{
  if (!ui) return;
  ui->report_open = false;
  rf_free(g_rf);
  g_rf = nullptr;
  ui->dirty = true;
}

bool ui_report_flow_is_open(const ccord_ui *ui) { return ui && ui->report_open && g_rf; }

void ui_report_flow_destroy(ccord_ui *ui)
{
  if (ui) ui->report_open = false;
  rf_free(g_rf);
  g_rf = nullptr;
}

size_t ui_report_flow_live_states(void) { return g_live_states; }

static void rf_on_menu(ccord_ui *ui, rf_state *s, const ccord_report_event *e)
{
  ccord_report_menu menu;
  ccord_error err;
  ccord_error_clear(&err);
  if (!e->menu_json || ccord_report_menu_parse(&menu, e->menu_json, e->menu_len, &err) != CCORD_OK) {
    s->phase = RF_LOAD_FAILED;
    rf_set_error(s, "Discord sent a report form C-Cord couldn't read.");
    ui->dirty = true;
    return;
  }
  if (s->have_menu) ccord_report_menu_free(&s->menu);
  rf_free_sels(s);
  s->menu = menu;
  s->have_menu = true;
  s->path_len = 0;
  if (!rf_enter(ui, s, s->menu.root, "")) s->phase = RF_LOAD_FAILED;
}

static void rf_on_sent(ccord_ui *ui, rf_state *s)
{
  s->phase = RF_SENT;
  s->error[0] = '\0';
  s->scroll = 0.0f;
  s->focus = -1;
  s->builtin_sent = !(s->have_menu && ccord_report_menu_node(&s->menu, s->menu.success));
  if (!s->builtin_sent) s->cur = s->menu.success;
  ui->dirty = true;
}

// Called every frame from ui_drain_modals: a menu or a "sent" answer from the session. Events are consumed even with no flow
// open (the session keeps only a few).
void ui_report_flow_pump(ccord_ui *ui)
{
  if (!ui || !ui->d.session) return;
  ccord_report_event e;
  while (ccord_session_pop_report_event(ui->d.session, &e)) {
    rf_state *s = g_rf;
    if (e.kind == CCORD_REPORT_EV_MENU) {
      if (s && s->phase == RF_LOADING && s->target.type == e.type) rf_on_menu(ui, s, &e);
    } else {
      ui_notice(ui, "Report sent. Thanks for helping keep Discord safe.");
      if (s && s->phase == RF_SUBMITTING) rf_on_sent(ui, s);
    }
    ccord_report_event_free(&e);
  }
}

// A report call Discord refused after it was queued (called from ui_drain_failures). True when the failure was a report one.
bool ui_report_flow_on_failure(ccord_ui *ui, const ccord_action_failure *f)
{
  if (!ui || !f || (f->action != CCORD_ACTION_REPORT && f->action != CCORD_ACTION_REPORT_MENU)) return false;
  rf_state *s = g_rf;
  if (f->action == CCORD_ACTION_REPORT) {
    ui_notice(ui, "Couldn't send report: %s", f->reason);
    if (s && s->phase == RF_SUBMITTING) {   // back to the node it was sent from; the answers are untouched
      s->phase = RF_NODE;
      rf_set_error(s, f->reason);
    }
  } else {
    ui_notice(ui, "Couldn't load the report form: %s", f->reason);
    if (s && s->phase == RF_LOADING) {
      s->phase = RF_LOAD_FAILED;
      rf_set_error(s, f->reason);
    }
  }
  ui->dirty = true;
  return true;
}

// ---- buttons ------------------------------------------------------------------------------------------------------

static bool rf_any_checked(const rf_state *s, const ccord_report_node *n)
{
  for (size_t k = 0; k < n->element_count; k++) {
    const rf_sel *sel = rf_sel_find(s, n->id, (uint32_t)k);
    if (n->elements[k].kind == CCORD_REPORT_EL_CHECKBOX && sel && sel->bits) return true;
  }
  return false;
}

// Next / Submit are disabled only while the node wants a ticked box and none is ticked.
static bool rf_primary_enabled(const rf_state *s)
{
  if (s->phase == RF_SENT || s->phase == RF_LOAD_FAILED) return true;
  if (s->phase != RF_NODE) return false;
  const ccord_report_node *n = rf_node(s);
  if (!n || n->button == CCORD_REPORT_BTN_NONE) return false;
  if ((n->button == CCORD_REPORT_BTN_NEXT || n->button == CCORD_REPORT_BTN_SUBMIT) && n->multi_select_required)
    return rf_any_checked(s, n);
  return true;
}

static void rf_primary(ccord_ui *ui, rf_state *s)
{
  if (!rf_primary_enabled(s)) return;
  if (s->phase == RF_LOAD_FAILED) {
    rf_request_menu(ui, s);
    return;
  }
  if (s->phase == RF_SENT) {
    ui_report_flow_close(ui);
    return;
  }
  const ccord_report_node *n = rf_node(s);
  if (!n) return;
  switch (n->button) {
    case CCORD_REPORT_BTN_NEXT:
      if (n->has_button_target) (void)rf_enter(ui, s, n->button_target, "");
      else rf_set_error(s, RF_OUT_OF_DATE);
      break;
    case CCORD_REPORT_BTN_SUBMIT: rf_submit(ui, s); break;
    case CCORD_REPORT_BTN_DONE:
    case CCORD_REPORT_BTN_CANCEL: ui_report_flow_close(ui); return;
    default: break;
  }
  ui->dirty = true;
}

// ---- layout & draw ------------------------------------------------------------------------------------------------

typedef struct rf_lay {
  ccord_ui *ui;
  ccord_renderer *r;
  rf_state *s;
  float w, y;   // item width; the next item's y in content coordinates
} rf_lay;

static ccord_text_style rf_style(const ccord_ui *ui, int style)
{
  switch (style) {
    case RFS_HEADER: return ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
    case RFS_SUB: return ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
    case RFS_INFO: return ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    case RFS_LABEL: return ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_muted));
    default: return ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  }
}

static rf_item *rf_add(rf_lay *L, rf_item_kind kind, float h, float gap)
{
  rf_state *s = L->s;
  if (s->nitems >= RF_MAX_ITEMS) return nullptr;
  rf_item *it = &s->items[s->nitems++];
  *it = (rf_item){.kind = kind, .rect = {0.0f, L->y, L->w, h}};
  L->y += h + gap;
  return it;
}

static void rf_add_text(rf_lay *L, int style, const char *text, float gap)
{
  if (!text || !text[0]) return;
  ccord_text_style st = rf_style(L->ui, style);
  float h = ccord_text_measure(L->r, text, strlen(text), &st, L->w).y;
  rf_item *it = rf_add(L, RFI_TEXT, h, gap);
  if (it) {
    it->style = (uint8_t)style;
    it->text = text;
  }
}

// "Spam > Phishing": the rows clicked on the way, newest last.
static void rf_build_crumb(rf_state *s)
{
  s->crumb[0] = '\0';
  size_t len = 0;
  for (size_t i = 0; i < s->path_len; i++) {
    if (!s->names[i][0]) continue;
    int n = snprintf(s->crumb + len, sizeof s->crumb - len, "%s%s", len ? " \xE2\x80\xBA " : "", s->names[i]);
    if (n < 0 || (size_t)n >= sizeof s->crumb - len) break;
    len += (size_t)n;
  }
}

// The reported message's author and its first 200 bytes (read-only, never sent).
static void rf_build_message_preview(const ccord_ui *ui, rf_state *s)
{
  s->author[0] = s->preview[0] = '\0';
  const ccord_message *m = ui->d.store ? ccord_store_find_message(ui->d.store, s->target.channel_id, s->target.message_id) : nullptr;
  if (!m) return;
  rf_user_name(ui, m->author_id, s->author, sizeof s->author);
  const char *c = m->content ? m->content : "";
  size_t n = strlen(c), cut = n < 200u ? n : 200u;
  while (cut > 0 && cut < n && ((unsigned char)c[cut] & 0xC0u) == 0x80u) cut--;
  (void)snprintf(s->preview, sizeof s->preview, "%.*s%s", (int)cut, c, cut < n ? "\xE2\x80\xA6" : "");
}

static void rf_build_elements(rf_lay *L, const ccord_report_node *n)
{
  rf_state *s = L->s;
  ccord_ui *ui = L->ui;
  for (size_t k = 0; k < n->element_count; k++) {
    const ccord_report_element *e = &n->elements[k];
    switch (e->kind) {
      case CCORD_REPORT_EL_TEXT:
        rf_add_text(L, RFS_LABEL, e->title, UIS_SP_1);
        rf_add_text(L, RFS_BODY, e->body, UIS_SP_3);
        break;
      case CCORD_REPORT_EL_CHECKBOX:
      case CCORD_REPORT_EL_DROPDOWN: {
        const rf_sel *sel = rf_sel_find(s, n->id, (uint32_t)k);
        rf_add_text(L, RFS_LABEL, e->title, UIS_SP_1);
        rf_add_text(L, RFS_INFO, e->subtitle, UIS_SP_2);
        for (size_t c = 0; c < e->choice_count && c < RF_MAX_CHOICES; c++) {
          bool check = e->kind == CCORD_REPORT_EL_CHECKBOX;
          ccord_text_style ds = rf_style(ui, RFS_INFO);
          bool has_desc = check && e->choices[c].description && e->choices[c].description[0];
          float h = UIS_H_BTN + (has_desc ? ccord_text_measure(L->r, e->choices[c].description, strlen(e->choices[c].description), &ds,
                                                                 L->w - 2.0f * UIS_SP_2 - 26.0f).y : 0.0f);
          rf_item *it = rf_add(L, check ? RFI_CHECK : RFI_OPTION, h, 0.0f);
          if (!it) break;
          it->el = (uint32_t)k;
          it->idx = c;
          it->text = e->choices[c].label ? e->choices[c].label : e->choices[c].value;
          it->text2 = has_desc ? e->choices[c].description : nullptr;
          it->on = sel && (check ? ((sel->bits >> c) & 1u) != 0u : sel->pick == (int)c);
        }
        L->y += UIS_SP_3;
        break;
      }
      case CCORD_REPORT_EL_FREE_TEXT: {
        rf_add_text(L, RFS_LABEL, e->title && e->title[0] ? e->title : "Anything else you'd like to add?", UIS_SP_1);
        rf_add_text(L, RFS_INFO, e->subtitle, UIS_SP_1);
        float rows = e->rows >= 2u ? (e->rows > 8u ? 8.0f : (float)e->rows) : 3.0f;
        rf_item *it = rf_add(L, RFI_EDIT, rows * ui_line_h(ui, M(font_size_base)) + 2.0f * UIS_SP_2, UIS_SP_3);
        if (it) {
          it->el = (uint32_t)k;
          it->text = e->placeholder && e->placeholder[0] ? e->placeholder : "Add details (optional)";
        }
        break;
      }
      case CCORD_REPORT_EL_EXTERNAL_LINK:
        if (e->url && strncmp(e->url, "https://", 8) == 0) {   // never open anything but https
          rf_item *it = rf_add(L, RFI_LINK, UIS_H_BTN_SM, UIS_SP_2);
          if (it) {
            it->text = e->link_text && e->link_text[0] ? e->link_text : e->url;
            it->text2 = e->url;
          }
        }
        break;
      case CCORD_REPORT_EL_BREADCRUMBS:
        rf_build_crumb(s);
        if (s->crumb[0]) rf_add_text(L, RFS_INFO, s->crumb, UIS_SP_3);
        break;
      case CCORD_REPORT_EL_MESSAGE_PREVIEW:
        if (s->target.type == CCORD_REPORT_MESSAGE) {
          rf_build_message_preview(ui, s);
          if (s->author[0]) {
            ccord_text_style bs = rf_style(ui, RFS_BODY);
            float th = ccord_text_measure(L->r, s->preview[0] ? s->preview : " ", strlen(s->preview[0] ? s->preview : " "), &bs,
                                          L->w - 2.0f * UIS_SP_3).y;
            rf_item *it = rf_add(L, RFI_PREVIEW, UIS_SP_2 + ui_line_h(ui, bs.size) + th + UIS_SP_2, UIS_SP_3);
            if (it) {
              it->text = s->author;
              it->text2 = s->preview;
            }
          }
        }
        break;
      case CCORD_REPORT_EL_USER_PREVIEW:
        if (s->target.type == CCORD_REPORT_USER) {
          rf_user_name(ui, s->target.user_id, s->author, sizeof s->author);
          rf_item *it = rf_add(L, RFI_PREVIEW, UIS_H_BTN + UIS_SP_2, UIS_SP_3);
          if (it) {
            it->text = s->author;
            it->text2 = nullptr;
          }
        }
        break;
      case CCORD_REPORT_EL_BLOCK_USERS:
        if (s->phase == RF_SENT) {   // the only "more you can do" action: Block the author / the reported user
          ccord_id who = rf_block_target(ui, s);
          if (who != CCORD_ID_NONE) {
            char nm[96];
            rf_user_name(ui, who, nm, sizeof nm);
            bool blocked = ccord_store_relationship(ui->d.store, who) == CCORD_RELATIONSHIP_BLOCKED;
            (void)snprintf(s->blocklabel, sizeof s->blocklabel, "%s %s", blocked ? "Blocked" : "Block", nm);
            rf_item *it = rf_add(L, RFI_BLOCK, UIS_H_BTN, UIS_SP_2);
            if (it) {
              it->text = s->blocklabel;
              it->on = blocked;
            }
          }
        }
        break;
      default: break;   // success / fail / unknown: nothing to show
    }
  }
}

// Fills s->items for the current phase and node, in content coordinates, and sets content_h.
static void rf_build_items(rf_lay *L)
{
  rf_state *s = L->s;
  s->nitems = 0;
  L->y = 0.0f;
  switch (s->phase) {
    case RF_LOADING: rf_add_text(L, RFS_BODY, "Loading report options\xE2\x80\xA6", 0.0f); break;
    case RF_LOAD_FAILED: rf_add_text(L, RFS_BODY, "The report options could not be loaded.", 0.0f); break;
    default: {
      const ccord_report_node *n = rf_node(s);
      if (!n) {   // sent with no success node in the menu
        rf_add_text(L, RFS_HEADER, "Report sent", UIS_SP_2);
        rf_add_text(L, RFS_BODY, "Thanks for helping keep Discord safe.", 0.0f);
        break;
      }
      rf_add_text(L, RFS_HEADER, n->header, UIS_SP_2);
      rf_add_text(L, RFS_SUB, n->subheader, UIS_SP_2);
      rf_add_text(L, RFS_INFO, n->info, UIS_SP_2);
      if (s->phase != RF_SENT) {
        for (size_t c = 0; c < n->child_count; c++) {
          rf_item *it = rf_add(L, RFI_CHILD, UIS_H_BTN, UIS_SP_1);
          if (!it) break;
          it->idx = c;
          it->text = n->children[c].name;
        }
        if (n->child_count) L->y += UIS_SP_2;
      }
      rf_build_elements(L, n);
    }
  }
  s->content_h = L->y;
}

static const char *rf_primary_label(const rf_state *s)
{
  if (s->phase == RF_SUBMITTING) return "Sending\xE2\x80\xA6";
  if (s->phase == RF_SENT) return "Done";
  if (s->phase == RF_LOAD_FAILED) return "Retry";
  if (s->phase == RF_LOADING) return nullptr;
  const ccord_report_node *n = rf_node(s);
  if (!n) return nullptr;
  switch (n->button) {
    case CCORD_REPORT_BTN_NEXT: return "Next";
    case CCORD_REPORT_BTN_SUBMIT: return "Submit";
    case CCORD_REPORT_BTN_DONE: return "Done";
    case CCORD_REPORT_BTN_CANCEL: return "Close";
    default: return nullptr;
  }
}

static void rf_layout(ccord_ui *ui, ccord_renderer *r, rf_state *s)
{
  float box_w = uis_modal_box(ui, UIS_MODAL_W_MD, 100.0f).w;
  rf_lay L = {.ui = ui, .r = r, .s = s, .w = box_w - 2.0f * UIS_SP_5 - UIS_SP_2};
  rf_build_items(&L);

  ccord_text_style tit = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  float title_h = UIS_SP_5 + ui_line_h(ui, tit.size) + UIS_SP_3;
  ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(danger));
  float err_h = s->error[0] ? ccord_text_measure(r, s->error, strlen(s->error), &es, box_w - 2.0f * UIS_SP_5).y + UIS_SP_2 : 0.0f;
  float hw = uis_label_width(ui, r, "How reporting works");
  s->primary_label = rf_primary_label(s);
  const ccord_report_node *cn_pre = rf_node(s);
  bool closes_pre = s->phase == RF_SENT ||
                    (s->phase == RF_NODE && cn_pre &&
                     (cn_pre->button == CCORD_REPORT_BTN_DONE || cn_pre->button == CCORD_REPORT_BTN_CANCEL));
  int nbtns = (s->primary_label ? 1 : 0) + ((s->phase == RF_NODE && s->path_len > 1) ? 1 : 0) + (!closes_pre ? 1 : 0);
  float btns_w = nbtns > 0 ? ((float)nbtns * RF_BTN_W + (float)(nbtns - 1) * UIS_SP_2) : 0.0f;
  bool stack_help = (UIS_SP_5 * 2.0f + hw + UIS_SP_2 + btns_w > box_w);
  float foot_h = UIS_SP_3 + UIS_H_BTN + UIS_SP_5 + (stack_help ? (UIS_H_BTN_SM + UIS_SP_1) : 0.0f);
  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_MD, title_h + s->content_h + err_h + foot_h);
  s->box = box;
  s->view_h = box.h - title_h - err_h - foot_h;
  if (s->view_h < 0.0f) s->view_h = 0.0f;
  float max_scroll = s->content_h > s->view_h ? s->content_h - s->view_h : 0.0f;
  if (s->scroll > max_scroll) s->scroll = max_scroll;
  if (s->scroll < 0.0f) s->scroll = 0.0f;
  float cx = box.x + UIS_SP_5;
  s->content = (ccord_rect){cx, box.y + title_h, L.w, s->view_h};
  for (size_t i = 0; i < s->nitems; i++) {
    s->items[i].rect.x = cx;
    s->items[i].rect.y += s->content.y - s->scroll;
  }

  s->primary = s->back = s->close = s->retry = (ccord_rect){0};
  int slot = 0;
  float bw = RF_BTN_W;
  if (nbtns > 0 && UIS_SP_5 * 2.0f + (float)nbtns * bw + (float)(nbtns - 1) * UIS_SP_2 > box.w) {
    bw = floorf((box.w - UIS_SP_5 * 2.0f - (float)(nbtns - 1) * UIS_SP_2) / (float)nbtns);
    if (bw < 64.0f) bw = 64.0f;
  }
  if (s->primary_label) s->primary = uis_modal_footer_button(box, slot++, bw);
  if (s->phase == RF_NODE && s->path_len > 1) s->back = uis_modal_footer_button(box, slot++, bw);
  if (!closes_pre) s->close = uis_modal_footer_button(box, slot++, bw);
  s->retry = s->phase == RF_LOAD_FAILED ? s->primary : (ccord_rect){0};
  if (stack_help) {
    s->help = (ccord_rect){box.x + UIS_SP_5, box.y + box.h - UIS_SP_5 - UIS_H_BTN - UIS_SP_1 - UIS_H_BTN_SM,
                           fminf(hw, box.w - 2.0f * UIS_SP_5), UIS_H_BTN_SM};
  } else {
    s->help = (ccord_rect){box.x + UIS_SP_5, box.y + box.h - UIS_SP_5 - UIS_H_BTN, hw, UIS_H_BTN};
  }
}

static bool rf_hover(const ccord_ui *ui, const rf_state *s, ccord_rect rc)
{
  (void)s;
  return ui->mouse_in && rc.w > 0.0f && ui_in_rect(ui->mx, ui->my, rc);
}

static void rf_draw_edit(ccord_ui *ui, ccord_renderer *r, rf_state *s, const rf_item *it)
{
  rf_sel *sel = rf_sel_find(s, s->cur, it->el);
  if (!sel) return;
  int idx = rf_sel_index(s, sel);
  bool focused = idx == s->focus;
  if (focused) ccord_draw_rrect(r, (ccord_rect){it->rect.x - 1.0f, it->rect.y - 1.0f, it->rect.w + 2.0f, it->rect.h + 2.0f}, 6.0f, COL(brand));
  ccord_draw_rrect(r, it->rect, 5.0f, COL(bg_input));
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  float tx = it->rect.x + UIS_SP_3, ty = it->rect.y + UIS_SP_2, tw = it->rect.w - 2.0f * UIS_SP_3;
  float inner_h = it->rect.h - 2.0f * UIS_SP_2;
  const char *data = sel->ed.data ? sel->ed.data : "";
  size_t len = sel->ed.len;
  ccord_push_clip(r, (ccord_rect){it->rect.x, it->rect.y + 1.0f, it->rect.w, it->rect.h - 2.0f});
  float off = 0.0f;
  if (len == 0) {
    ccord_text_style ph = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_placeholder));
    ccord_text_draw_ellipsis(r, it->text, strlen(it->text), tx, ty, tw, &ph);
  } else {
    ccord_rect cr = ccord_text_caret_rect(r, data, len, &st, tw, sel->ed.caret);
    if (cr.y + cr.h > inner_h) off = cr.y + cr.h - inner_h;
    if (s->click_pending && focused) {
      sel->ed.caret = sel->ed.anchor = ccord_text_hit_test(r, data, len, &st, tw, s->click_x - tx, s->click_y - ty + off);
      s->click_pending = false;
    }
    if (sel->ed.anchor != sel->ed.caret) {   // selection: one bar per wrapped line
      size_t lo = sel->ed.anchor < sel->ed.caret ? sel->ed.anchor : sel->ed.caret;
      size_t hi = sel->ed.anchor < sel->ed.caret ? sel->ed.caret : sel->ed.anchor;
      ccord_rect a = ccord_text_caret_rect(r, data, len, &st, tw, lo), b = ccord_text_caret_rect(r, data, len, &st, tw, hi);
      if (a.y == b.y) ccord_draw_rect(r, (ccord_rect){tx + a.x, ty + a.y - off, b.x - a.x, a.h}, COL(selection));
    }
    ccord_text_draw(r, data, len, tx, ty - off, &st, tw);
  }
  if (focused) {
    ccord_rect cr = ccord_text_caret_rect(r, data, len, &st, tw, sel->ed.caret);
    ccord_draw_rect(r, (ccord_rect){tx + cr.x, ty + cr.y - off, 1.5f, cr.h > 0.0f ? cr.h : ui_line_h(ui, st.size)}, COL(caret));
  }
  ccord_pop_clip(r);
  char cnt[32];
  (void)snprintf(cnt, sizeof cnt, "%zu/%zu", rf_cps(data, len), sel->limit);
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_vec2 cm = ccord_text_measure(r, cnt, strlen(cnt), &cs, 0.0f);
  ccord_draw_rrect(r, (ccord_rect){it->rect.x + it->rect.w - cm.x - 2.0f * UIS_SP_2, it->rect.y + it->rect.h - cm.y - UIS_SP_1, cm.x + UIS_SP_2, cm.y}, 3.0f,
                   COL(bg_input));
  ccord_text_draw(r, cnt, strlen(cnt), it->rect.x + it->rect.w - cm.x - UIS_SP_2 - 4.0f, it->rect.y + it->rect.h - cm.y - UIS_SP_1, &cs, 0.0f);
}

static void rf_draw_item(ccord_ui *ui, ccord_renderer *r, rf_state *s, const rf_item *it)
{
  bool hover = rf_hover(ui, s, it->rect) && ui_in_rect(ui->mx, ui->my, s->content) && s->phase != RF_SUBMITTING;
  uint64_t id = ((uint64_t)it->el << 16) | it->idx;
  switch (it->kind) {
    case RFI_TEXT: {
      ccord_text_style st = rf_style(ui, it->style);
      ccord_text_draw(r, it->text, strlen(it->text), it->rect.x, it->rect.y, &st, it->rect.w);
      break;
    }
    case RFI_CHILD: {
      uis_list_row_fill(ui, r, it->rect, (uis_state){.hover = hover}, "rfchild", id);
      ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_header));
      ccord_text_draw_ellipsis(r, it->text, strlen(it->text), it->rect.x + UIS_SP_3, it->rect.y + (it->rect.h - ui_line_h(ui, st.size)) * 0.5f,
                               it->rect.w - 3.0f * UIS_SP_3 - 8.0f, &st);
      ccord_vec2 cm = ccord_text_measure(r, "\xE2\x80\xBA", 3, &st, 0.0f);
      ccord_text_draw(r, "\xE2\x80\xBA", 3, it->rect.x + it->rect.w - UIS_SP_3 - cm.x, it->rect.y + (it->rect.h - ui_line_h(ui, st.size)) * 0.5f,
                      &st, 0.0f);
      break;
    }
    case RFI_CHECK: {
      uis_list_row_fill(ui, r, it->rect, (uis_state){.hover = hover}, "rfcheck", id);
      float bs = 18.0f;
      ccord_rect bx = {it->rect.x + UIS_SP_2, it->rect.y + (UIS_H_BTN - bs) * 0.5f, bs, bs};
      ccord_draw_rrect(r, bx, 4.0f, it->on ? COL(brand) : COL(text_muted));
      if (!it->on) ccord_draw_rrect(r, (ccord_rect){bx.x + 2.0f, bx.y + 2.0f, bs - 4.0f, bs - 4.0f}, 3.0f, COL(bg_secondary));
      else {
        ccord_color wc = COL(text_on_brand);
        ccord_draw_line(r, (ccord_vec2){bx.x + 4.0f, bx.y + 9.5f}, (ccord_vec2){bx.x + 7.5f, bx.y + 13.0f}, 2.0f, wc);
        ccord_draw_line(r, (ccord_vec2){bx.x + 7.5f, bx.y + 13.0f}, (ccord_vec2){bx.x + 14.0f, bx.y + 5.5f}, 2.0f, wc);
      }
      float tx = bx.x + bs + UIS_SP_2;
      ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_header));
      ccord_text_draw_ellipsis(r, it->text, strlen(it->text), tx, it->rect.y + (UIS_H_BTN - ui_line_h(ui, st.size)) * 0.5f,
                               it->rect.x + it->rect.w - tx - UIS_SP_2, &st);
      if (it->text2) {
        ccord_text_style ds = rf_style(ui, RFS_INFO);
        ccord_text_draw(r, it->text2, strlen(it->text2), tx, it->rect.y + UIS_H_BTN - UIS_SP_1, &ds, it->rect.x + it->rect.w - tx - UIS_SP_2);
      }
      break;
    }
    case RFI_OPTION:
      uis_radio(ui, r, it->rect, it->text, it->on, (uis_state){.hover = hover}, "rfopt", id);
      break;
    case RFI_EDIT: rf_draw_edit(ui, r, s, it); break;
    case RFI_LINK:
      uis_button(ui, r, it->rect, it->text, UIS_BTN_LINK, UIS_BTN_S_SM, (uis_state){.hover = hover}, "rflink", id);
      break;
    case RFI_PREVIEW: {
      ccord_draw_rrect(r, it->rect, 6.0f, COL(bg_tertiary));
      ccord_text_style nm = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
      ccord_text_style bd = rf_style(ui, RFS_BODY);
      float ty = it->rect.y + (it->text2 ? UIS_SP_2 : (it->rect.h - ui_line_h(ui, nm.size)) * 0.5f);
      ccord_text_draw_ellipsis(r, it->text, strlen(it->text), it->rect.x + UIS_SP_3, ty, it->rect.w - 2.0f * UIS_SP_3, &nm);
      if (it->text2 && it->text2[0]) {
        ccord_text_draw(r, it->text2, strlen(it->text2), it->rect.x + UIS_SP_3, ty + ui_line_h(ui, nm.size), &bd, it->rect.w - 2.0f * UIS_SP_3);
      }
      break;
    }
    case RFI_BLOCK:
      uis_button(ui, r, it->rect, it->text, UIS_BTN_DANGER_SOFT, UIS_BTN_S_MD, (uis_state){.hover = hover, .disabled = it->on}, "rfblock", 0);
      break;
  }
}

void ui_report_flow_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !ui_report_flow_is_open(ui)) return;
  rf_state *s = g_rf;
  ui_popup_begin(ui, r, "reportflow", 0, UI_MODAL_MS, UIS_SLIDE_MODAL);
  rf_layout(ui, r, s);
  ccord_rect box = s->box;
  uis_modal_frame(ui, r, box);
  (void)uis_modal_title(ui, r, box, s->target.type == CCORD_REPORT_USER ? "Report User" : "Report Message");

  ccord_push_clip(r, s->content);
  for (size_t i = 0; i < s->nitems; i++) {
    const rf_item *it = &s->items[i];
    if (it->rect.y + it->rect.h < s->content.y || it->rect.y > s->content.y + s->content.h) continue;
    rf_draw_item(ui, r, s, it);
  }
  ccord_pop_clip(r);
  if (s->content_h > s->view_h && s->view_h > 0.0f) {
    ccord_rect track = {s->content.x + s->content.w + UIS_SP_1, s->content.y, 6.0f, s->view_h};
    float th = s->view_h * s->view_h / s->content_h;
    if (th < 24.0f) th = 24.0f;
    float max_scroll = s->content_h - s->view_h;
    uis_scrollbar(ui, r, track, (ccord_rect){track.x, track.y + (s->view_h - th) * (s->scroll / max_scroll), track.w, th});
  }

  if (s->error[0]) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(danger));
    ccord_text_draw(r, s->error, strlen(s->error), box.x + UIS_SP_5, s->content.y + s->content.h + UIS_SP_1, &es, box.w - 2.0f * UIS_SP_5);
  }
  uis_button(ui, r, s->help, "How reporting works", UIS_BTN_LINK, UIS_BTN_S_SM, (uis_state){.hover = rf_hover(ui, s, s->help)}, "rfhelp", 0);
  if (s->close.w > 0.0f) {
    uis_button(ui, r, s->close, "Close", UIS_BTN_LINK, UIS_BTN_S_MD, (uis_state){.hover = rf_hover(ui, s, s->close)}, "rfclose", 0);
  }
  if (s->back.w > 0.0f) {
    uis_button(ui, r, s->back, "Back", UIS_BTN_SECONDARY, UIS_BTN_S_MD, (uis_state){.hover = rf_hover(ui, s, s->back)}, "rfback", 0);
  }
  if (s->primary.w > 0.0f && s->primary_label) {
    uis_button(ui, r, s->primary, s->primary_label, UIS_BTN_PRIMARY, UIS_BTN_S_MD,
               (uis_state){.hover = rf_hover(ui, s, s->primary), .disabled = !rf_primary_enabled(s)}, "rfprimary", 0);
  }
  ui_popup_end(r);
}

// ---- input --------------------------------------------------------------------------------------------------------

static rf_sel *rf_focused(rf_state *s) { return s->focus >= 0 && (size_t)s->focus < s->nsel ? s->sels[s->focus] : nullptr; }

// Inserts typed or pasted text into the focused field, cut at its character limit (a selection being replaced does not count).
static void rf_insert(rf_state *s, const char *text, size_t len)
{
  rf_sel *sel = rf_focused(s);
  if (!sel) return;
  ccord_text_edit *ed = &sel->ed;
  size_t lo = ed->caret < ed->anchor ? ed->caret : ed->anchor, hi = ed->caret < ed->anchor ? ed->anchor : ed->caret;
  size_t have = rf_cps(ed->data ? ed->data : "", ed->len) - rf_cps(ed->data ? ed->data + lo : "", hi - lo);
  size_t room = sel->limit > have ? sel->limit - have : 0;
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

static void rf_focus_step(rf_state *s, int dir)
{
  size_t n = 0;
  int cur_pos = -1;
  int order[RF_MAX_SELS];
  for (size_t i = 0; i < s->nsel; i++) {
    if (s->sels[i]->is_text && s->sels[i]->node == s->cur) {
      if ((int)i == s->focus) cur_pos = (int)n;
      order[n++] = (int)i;
    }
  }
  if (n == 0) return;
  int next = cur_pos < 0 ? (dir > 0 ? 0 : (int)n - 1) : (int)(((size_t)cur_pos + (dir > 0 ? 1u : n - 1u)) % n);
  s->focus = order[next];
}

static void rf_key(ccord_ui *ui, rf_state *s, const ccord_input_event *ev)
{
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  rf_sel *sel = s->phase == RF_NODE ? rf_focused(s) : nullptr;
  ccord_text_edit *ed = sel ? &sel->ed : nullptr;
  switch (ev->key) {
    case CCORD_KEY_ESCAPE: ui_report_flow_close(ui); return;
    case CCORD_KEY_TAB: rf_focus_step(s, shift ? -1 : 1); break;
    case CCORD_KEY_RETURN:
      if (ed && !ctrl) rf_insert(s, "\n", 1);
      else rf_primary(ui, s);
      if (!g_rf) return;
      break;
    case CCORD_KEY_BACKSPACE:
      if (ed) {
        if (ctrl) ccord_text_edit_backspace_word(ed);
        else ccord_text_edit_backspace(ed);
      }
      break;
    case CCORD_KEY_DELETE:
      if (ed) {
        if (ctrl) ccord_text_edit_delete_word(ed);
        else ccord_text_edit_delete(ed);
      }
      break;
    case CCORD_KEY_LEFT:
      if (ed) ccord_text_edit_move(ed, ctrl ? -3 : -1, shift);
      break;
    case CCORD_KEY_RIGHT:
      if (ed) ccord_text_edit_move(ed, ctrl ? 3 : 1, shift);
      break;
    case CCORD_KEY_HOME:
      if (ed) ccord_text_edit_move(ed, -2, shift);
      break;
    case CCORD_KEY_END:
      if (ed) ccord_text_edit_move(ed, 2, shift);
      break;
    case CCORD_KEY_A:
      if (ed && ctrl) ccord_text_edit_select_all(ed);
      break;
    case CCORD_KEY_V:
      if (ed && ctrl && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          rf_insert(s, clip, strlen(clip));
          ccord_free(clip);
        }
      }
      break;
    default: break;
  }
  ui->dirty = true;
}

static void rf_click(ccord_ui *ui, rf_state *s, float x, float y)
{
  if (s->close.w > 0.0f && ui_in_rect(x, y, s->close)) {
    ui_report_flow_close(ui);
    return;
  }
  if (ui_in_rect(x, y, s->help)) {
    (void)ui_open_url(ui, RF_HELP_URL, "How reporting works");
    return;
  }
  if (s->phase == RF_SUBMITTING) return;   // only Close (and Escape) work while the report is on its way
  s->error[0] = '\0';
  ui->dirty = true;
  if (s->primary.w > 0.0f && ui_in_rect(x, y, s->primary)) {
    rf_primary(ui, s);
    return;
  }
  if (s->back.w > 0.0f && ui_in_rect(x, y, s->back)) {
    rf_back(ui, s);
    return;
  }
  if (!ui_in_rect(x, y, s->content)) return;
  const ccord_report_node *n = rf_node(s);
  for (size_t i = 0; i < s->nitems; i++) {
    const rf_item *it = &s->items[i];
    if (!ui_in_rect(x, y, it->rect)) continue;
    rf_sel *sel = n ? rf_sel_find(s, n->id, it->el) : nullptr;
    switch (it->kind) {
      case RFI_CHILD:
        if (n && it->idx < n->child_count) (void)rf_enter(ui, s, n->children[it->idx].target, n->children[it->idx].name);
        return;
      case RFI_CHECK:
        if (sel) sel->bits ^= 1ull << it->idx;
        return;
      case RFI_OPTION:
        if (sel) sel->pick = sel->pick == (int)it->idx ? -1 : (int)it->idx;
        return;
      case RFI_EDIT:
        if (sel) {
          s->focus = rf_sel_index(s, sel);
          s->click_pending = true;
          s->click_x = x;
          s->click_y = y;
          if (ui->touch_mode) ui->touch_text_focused = true;
        }
        return;
      case RFI_LINK:
        (void)ui_open_url(ui, it->text2, it->text);
        return;
      case RFI_BLOCK:
        if (!it->on) {
          ccord_id who = rf_block_target(ui, s);
          if (who != CCORD_ID_NONE) ui_set_user_relationship(ui, who, CCORD_RELATIONSHIP_BLOCKED);
        }
        return;
      default: return;
    }
  }
}

bool ui_report_flow_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui_report_flow_is_open(ui)) return false;
  rf_state *s = g_rf;
  switch (ev->type) {
    case CCORD_IN_KEY_DOWN: rf_key(ui, s, ev); return true;
    case CCORD_IN_TEXT:
      if (ev->text[0] != '\0' && s->phase == RF_NODE) {
        rf_insert(s, ev->text, strlen(ev->text));
        ui->dirty = true;
      }
      return true;
    case CCORD_IN_MOUSE_DOWN:
      if (ev->button == 1) rf_click(ui, s, ev->x, ev->y);
      return true;
    case CCORD_IN_WHEEL:
      s->scroll -= ev->wheel * 48.0f;   // clamped at the next layout
      if (s->scroll < 0.0f) s->scroll = 0.0f;
      ui->dirty = true;
      return true;
    case CCORD_IN_MOUSE_UP: return true;
    default: return false;
  }
}

// ---- verify-slice seam --------------------------------------------------------------------------------------------

void ui_report_flow_debug_get(const ccord_ui *ui, ui_report_flow_debug *out)
{
  if (!out) return;
  *out = (ui_report_flow_debug){0};
  if (!ui_report_flow_is_open(ui)) return;
  const rf_state *s = g_rf;
  out->open = true;
  out->phase = s->phase;
  out->node_id = s->cur;
  (void)snprintf(out->error, sizeof out->error, "%s", s->error);
  const ccord_report_node *n = rf_node(s);
  if (n && n->header) (void)snprintf(out->header, sizeof out->header, "%s", n->header);
  out->child_count = n ? n->child_count : 0;
  out->path_len = s->path_len;
  for (size_t i = 0; i < s->path_len; i++) out->path[i] = s->path[i];
  out->button_enabled = rf_primary_enabled(s);
  if (s->primary_label) (void)snprintf(out->button_label, sizeof out->button_label, "%s", s->primary_label);
  size_t nc = 0, nk = 0;
  bool text_seen = false;
  for (size_t i = 0; i < s->nitems; i++) {
    const rf_item *it = &s->items[i];
    if (it->kind == RFI_CHILD && nc < 12) out->children[nc++] = it->rect;
    if (it->kind == RFI_CHECK) {
      out->checkbox_count++;
      if (it->on) out->checked_count++;
      if (nk < 12) out->checks[nk++] = it->rect;
    }
    if (it->kind == RFI_EDIT && !text_seen) {
      text_seen = true;
      out->text_rect = it->rect;
      const rf_sel *sel = rf_sel_find(s, s->cur, it->el);
      if (sel && sel->ed.data) (void)snprintf(out->text, sizeof out->text, "%s", sel->ed.data);
    }
    if (it->kind == RFI_BLOCK) {
      out->block = it->rect;
      (void)snprintf(out->block_label, sizeof out->block_label, "%s", it->text);
    }
    if (it->kind == RFI_TEXT && it->style == RFS_INFO && n && s->crumb[0] && strcmp(it->text, s->crumb) == 0) {
      (void)snprintf(out->crumb, sizeof out->crumb, "%s", it->text);
    }
    if (it->kind == RFI_PREVIEW) (void)snprintf(out->preview, sizeof out->preview, "%s", it->text);
  }
  out->button = s->primary;
  out->back = s->back;
  out->close = s->close;
  out->retry = s->retry;
  out->help = s->help;
}
