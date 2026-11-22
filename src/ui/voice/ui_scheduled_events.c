// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Guild Scheduled Events modal + stage lifecycle prompts (Result 3 UI). The modal opens from the
// guild dropdown's "Events" item, lists the guild's events (RSVP / edit / delete) and hosts the
// create/edit form; the stage control row opens the Start Stage topic prompt and the End Stage
// confirm here. Like the M5 voice surfaces, everything borrows the generic modal routing
// (ui->modal_kind = UI_MODAL_SCHEDULED_EVENTS) and intercepts ui_draw_overlays / ui_overlays_handle
// before the legacy modal code. Every Discord change is a session intent or a governed admin
// request: the store only changes when the transport answers.
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/net/admin.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define EV_ROW_H 84.0f
#define EV_CHANNEL_MAX 32u

typedef struct ui_events_state {
  const ccord_ui *owner;
  bool open;
  ui_events_view view;
  ccord_id guild_id;
  // List view.
  float scroll;
  ccord_scroller glide;                // wheel glide of the list (scroll.h)
  ccord_id delete_armed;               // event whose Delete button is waiting for the confirm click
  ccord_rect box, create_btn, close_btn, list_view, list_thumb;
  float list_content_h;
  size_t row_count;
  struct {
    ccord_id event_id;
    ccord_rect row, rsvp, edit, del;
    bool rsvp_on, delete_armed;
  } rows[UI_EVENTS_ROW_MAX];
  // Create / edit form.
  ccord_id edit_id;                    // 0 = creating
  int entity_type;                     // ccord_scheduled_event_entity_type
  size_t channel_sel;                  // index into the eligible stage / voice channels
  int focus;                           // 0 name, 1 location, 2 start, 3 end, 4 description
  ccord_text_edit name, location, start, end, desc;
  bool edits_init;
  char form_error[192];
  ccord_rect f_view, f_name, f_location, f_start, f_end, f_desc, f_type, f_channel, f_save, f_cancel;
  float form_scroll, form_max, form_content_h;
  // Stage prompts.
  ccord_id stage_guild, stage_channel;
  ccord_text_edit stage_topic;
  bool stage_topic_init;
  ccord_rect sp_box, sp_input, sp_ok, sp_cancel;
  ccord_rect ec_box, ec_ok, ec_cancel;
} ui_events_state;

static ui_events_state *g_events;

static void events_free(void)
{
  if (!g_events) return;
  if (g_events->edits_init) {
    ccord_text_edit_free(&g_events->name);
    ccord_text_edit_free(&g_events->location);
    ccord_text_edit_free(&g_events->start);
    ccord_text_edit_free(&g_events->end);
    ccord_text_edit_free(&g_events->desc);
  }
  if (g_events->stage_topic_init) ccord_text_edit_free(&g_events->stage_topic);
  ccord_free(g_events);
  g_events = nullptr;
}

// Owner-tracked lazy heap state (the M5 pattern): a modal left open by a destroyed ui whose address
// got reused heals itself here, and so does one whose borrowed modal kind was taken over.
static ui_events_state *events_state(const ccord_ui *ui, bool create)
{
  if (g_events && (g_events->owner != ui || (g_events->open && ui->modal_kind != UI_MODAL_SCHEDULED_EVENTS))) {
    events_free();
  }
  if (!g_events && create) {
    g_events = ccord_calloc(1, sizeof *g_events);
    if (g_events) g_events->owner = ui;
  }
  return g_events;
}

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------- time input

// Howard Hinnant's civil-date algorithm (same as the wire parsers in discord_json.c / admin.c).
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static bool take_digits(const char *s, size_t len, size_t *i, int n, int *out)
{
  if (*i + (size_t)n > len) return false;
  int v = 0;
  for (int k = 0; k < n; k++) {
    char c = s[*i + (size_t)k];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *i += (size_t)n;
  *out = v;
  return true;
}

// Event time input: ISO 8601 (YYYY-MM-DDTHH:MM[:SS][.fff][Z|±HH:MM]; no zone = UTC) or a relative
// offset from now (+90m, +2h, +1d, +1h30m). Empty or malformed -> false.
static bool parse_event_time(const char *s, int64_t now_ms, int64_t *out)
{
  if (!s || !out) return false;
  while (*s == ' ' || *s == '\t') s++;
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n')) len--;
  if (len == 0) return false;

  if (s[0] == '+' || s[0] == '-') {
    int64_t total = 0;
    size_t i = 1;
    bool any = false;
    while (i < len) {
      while (i < len && s[i] == ' ') i++;
      if (i >= len) break;
      int64_t num = 0;
      size_t start = i;
      while (i < len && s[i] >= '0' && s[i] <= '9') {
        num = num * 10 + (s[i] - '0');
        if (num > 1000000) return false;   // ~3 years of minutes; keeps the math sane
        i++;
      }
      if (i == start || i >= len) return false;
      int64_t unit = 0;
      switch (s[i]) {
        case 'm': unit = 60000; break;
        case 'h': unit = 3600000; break;
        case 'd': unit = 86400000; break;
        default: return false;
      }
      i++;
      total += num * unit;
      any = true;
    }
    if (!any) return false;
    *out = now_ms + (s[0] == '-' ? -total : total);
    return true;
  }

  size_t i = 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (!take_digits(s, len, &i, 4, &y) || i >= len || s[i] != '-') return false;
  i++;
  if (!take_digits(s, len, &i, 2, &mo) || i >= len || s[i] != '-') return false;
  i++;
  if (!take_digits(s, len, &i, 2, &d) || i >= len || (s[i] != 'T' && s[i] != 't' && s[i] != ' ')) return false;
  i++;
  if (!take_digits(s, len, &i, 2, &h) || i >= len || s[i] != ':') return false;
  i++;
  if (!take_digits(s, len, &i, 2, &mi)) return false;
  if (i < len && s[i] == ':') {
    i++;
    if (!take_digits(s, len, &i, 2, &se)) return false;
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return false;
  int frac = 0, fdigits = 0;
  if (i < len && s[i] == '.') {
    i++;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      if (fdigits < 3) {
        frac = frac * 10 + (s[i] - '0');
        fdigits++;
      }
      i++;
    }
    while (fdigits < 3) {
      frac *= 10;
      fdigits++;
    }
  }
  int64_t offset = 0;
  if (i < len) {
    if (s[i] == 'Z' || s[i] == 'z') {
      i++;
    } else if (s[i] == '+' || s[i] == '-') {
      int oh = 0, om = 0;
      size_t j = i + 1;
      if (!take_digits(s, len, &j, 2, &oh)) return false;
      if (j < len && s[j] == ':') j++;
      if (!take_digits(s, len, &j, 2, &om)) return false;
      offset = (int64_t)(oh * 3600 + om * 60);
      if (s[i] == '-') offset = -offset;
      i = j;
    } else {
      return false;
    }
  }
  if (i != len) return false;
  int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)d);
  int64_t secs = days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + se - offset;
  *out = secs * 1000 + frac;
  return true;
}

// ---------------------------------------------------------------- permissions & helpers

static ccord_id events_self(const ccord_ui *ui)
{
  return ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;
}

static bool events_manage(const ccord_ui *ui, ccord_id gid)
{
  return ccord_store_has_permission(ui->d.store, gid, CCORD_ID_NONE, events_self(ui), CCORD_PERM_MANAGE_EVENTS);
}

static bool events_create(const ccord_ui *ui, ccord_id gid)
{
  return ccord_store_has_permission(ui->d.store, gid, CCORD_ID_NONE, events_self(ui),
                                    CCORD_PERM_CREATE_EVENTS | CCORD_PERM_MANAGE_EVENTS);
}

// Discord lets creators with Create Events manage their own events; everything else needs Manage Events.
static bool event_can_manage(const ccord_ui *ui, ccord_id gid, const ccord_scheduled_event *ev)
{
  if (events_manage(ui, gid)) return true;
  ccord_id self = events_self(ui);
  return ev->creator_id != CCORD_ID_NONE && ev->creator_id == self && events_create(ui, gid);
}

static const char *event_status_label(uint8_t status)
{
  switch (status) {
    case CCORD_SCHEDULED_EVENT_SCHEDULED: return "Scheduled";
    case CCORD_SCHEDULED_EVENT_ACTIVE: return "Active";
    case CCORD_SCHEDULED_EVENT_COMPLETED: return "Completed";
    case CCORD_SCHEDULED_EVENT_CANCELED: return "Canceled";
    default: return "Unknown";
  }
}

static ccord_color event_status_color(const ccord_ui *ui, uint8_t status)
{
  switch (status) {
    case CCORD_SCHEDULED_EVENT_ACTIVE: return COL(status_online);
    case CCORD_SCHEDULED_EVENT_CANCELED: return COL(danger);
    case CCORD_SCHEDULED_EVENT_COMPLETED: return COL(text_muted);
    default: return COL(text_secondary);
  }
}

// The guild's stage (entity type 1) or voice (entity type 2) channels, in sidebar order.
static size_t event_channels(const ccord_ui *ui, ccord_id gid, int entity_type, ccord_id *out, size_t cap)
{
  size_t n = 0;
  if (!ui->d.store || entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL) return 0;
  ccord_channel_kind kind = entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE ? CCORD_CHANNEL_STAGE
                                                                                : CCORD_CHANNEL_VOICE;
  size_t nc = ccord_store_channel_count(ui->d.store, gid);
  for (size_t i = 0; i < nc && n < cap; i++) {
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, gid, i);
    if (ch && ch->kind == kind) out[n++] = ch->id;
  }
  return n;
}

static ccord_id form_channel(const ccord_ui *ui, const ui_events_state *st)
{
  ccord_id ids[EV_CHANNEL_MAX];
  size_t n = event_channels(ui, st->guild_id, st->entity_type, ids, EV_CHANNEL_MAX);
  if (n == 0) return CCORD_ID_NONE;
  return ids[st->channel_sel < n ? st->channel_sel : 0];
}

// ---------------------------------------------------------------- open / close

static void events_close(ccord_ui *ui)
{
  events_free();
  if (ui->modal_kind == UI_MODAL_SCHEDULED_EVENTS) ui->modal_kind = UI_MODAL_NONE;
  ui->dirty = true;
}

void ui_scheduled_events_free(ccord_ui *ui)
{
  (void)ui;
  events_free();
}

void ui_scheduled_events_open(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return;
  if (ui->modal_kind != UI_MODAL_NONE) return;
  events_free();
  ui_events_state *st = events_state(ui, true);
  if (!st) return;
  st->open = true;
  st->view = UI_EVENTS_LIST;
  st->guild_id = guild_id;
  ui->modal_kind = UI_MODAL_SCHEDULED_EVENTS;
  ui->dirty = true;
  // Ask for the current list; the answer lands in the store through pump(). The cached rows (if
  // any) stay visible while it is in flight.
  if (ui->d.session) {
    ccord_error err;
    if (ccord_session_request_scheduled_events(ui->d.session, guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't load events: %s", err.message);
    }
  }
}

bool ui_scheduled_events_active(const ccord_ui *ui)
{
  if (!ui || ui->modal_kind != UI_MODAL_SCHEDULED_EVENTS) return false;
  ui_events_state *st = events_state(ui, false);
  return st && st->open;
}

bool ui_scheduled_events_wants_text(const ccord_ui *ui)
{
  if (!ui_scheduled_events_active(ui)) return false;
  ui_events_state *st = events_state(ui, false);
  return st && (st->view == UI_EVENTS_FORM || st->view == UI_EVENTS_STAGE_START);
}

bool ui_scheduled_events_handle_back(ccord_ui *ui)
{
  if (!ui_scheduled_events_active(ui)) return false;
  ui_events_state *st = events_state(ui, false);
  if (st && st->view == UI_EVENTS_FORM) {
    st->view = UI_EVENTS_LIST;
    ui->dirty = true;
    return true;
  }
  ccord_input_event esc = {.type = CCORD_IN_KEY_DOWN, .key = CCORD_KEY_ESCAPE};
  (void)ui_scheduled_events_handle(ui, &esc);
  return true;
}

static void form_open(ccord_ui *ui, ccord_id event_id)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  if (st->edits_init) {
    ccord_text_edit_free(&st->name);
    ccord_text_edit_free(&st->location);
    ccord_text_edit_free(&st->start);
    ccord_text_edit_free(&st->end);
    ccord_text_edit_free(&st->desc);
  }
  ccord_text_edit_init(&st->name, 400u);       // 100 characters, up to 4 bytes each
  ccord_text_edit_init(&st->location, 400u);
  ccord_text_edit_init(&st->start, 64u);
  ccord_text_edit_init(&st->end, 64u);
  ccord_text_edit_init(&st->desc, CCORD_ADMIN_EVENT_DESC_MAX);
  st->edits_init = true;
  st->edit_id = event_id;
  st->focus = 0;
  st->form_error[0] = '\0';
  st->entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE;
  st->channel_sel = 0;
  const ccord_scheduled_event *ev =
      event_id != CCORD_ID_NONE ? ccord_store_find_scheduled_event(ui->d.store, event_id) : nullptr;
  ccord_error err;
  if (ev) {
    st->entity_type = ev->entity_type ? ev->entity_type : CCORD_SCHEDULED_EVENT_STAGE_INSTANCE;
    if (ev->name[0]) (void)ccord_text_edit_insert(&st->name, ev->name, strlen(ev->name), &err);
    if (ev->description[0]) (void)ccord_text_edit_insert(&st->desc, ev->description, strlen(ev->description), &err);
    if (ev->location[0]) (void)ccord_text_edit_insert(&st->location, ev->location, strlen(ev->location), &err);
    char iso[48];
    if (ev->scheduled_start_ms > 0 && ccord_admin_format_iso(ev->scheduled_start_ms, iso, sizeof iso)) {
      (void)ccord_text_edit_insert(&st->start, iso, strlen(iso), &err);
    }
    if (ev->scheduled_end_ms > 0 && ccord_admin_format_iso(ev->scheduled_end_ms, iso, sizeof iso)) {
      (void)ccord_text_edit_insert(&st->end, iso, strlen(iso), &err);
    }
    ccord_id ids[EV_CHANNEL_MAX];
    size_t n = event_channels(ui, st->guild_id, st->entity_type, ids, EV_CHANNEL_MAX);
    for (size_t i = 0; i < n; i++) {
      if (ids[i] == ev->channel_id) {
        st->channel_sel = i;
        break;
      }
    }
  }
  st->view = UI_EVENTS_FORM;
  st->delete_armed = CCORD_ID_NONE;
  ui->dirty = true;
}

static void form_back_to_list(ccord_ui *ui)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  st->view = UI_EVENTS_LIST;
  st->edit_id = CCORD_ID_NONE;
  st->form_error[0] = '\0';
  ui->dirty = true;
}

void ui_stage_start_prompt_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  if (ui->modal_kind != UI_MODAL_NONE) return;
  events_free();
  ui_events_state *st = events_state(ui, true);
  if (!st) return;
  st->open = true;
  st->view = UI_EVENTS_STAGE_START;
  st->guild_id = guild_id;
  st->stage_guild = guild_id;
  st->stage_channel = channel_id;
  ccord_text_edit_init(&st->stage_topic, 480u);   // 120 characters, up to 4 bytes each
  st->stage_topic_init = true;
  ui->modal_kind = UI_MODAL_SCHEDULED_EVENTS;
  ui->dirty = true;
}

void ui_stage_end_confirm_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return;
  if (ui->modal_kind != UI_MODAL_NONE) return;
  events_free();
  ui_events_state *st = events_state(ui, true);
  if (!st) return;
  st->open = true;
  st->view = UI_EVENTS_STAGE_END;
  st->guild_id = guild_id;
  st->stage_guild = guild_id;
  st->stage_channel = channel_id;
  ui->modal_kind = UI_MODAL_SCHEDULED_EVENTS;
  ui->dirty = true;
}

// ---------------------------------------------------------------- shared draw helpers

static void ev_button(ccord_ui *ui, ccord_renderer *r, ccord_rect b, const char *label, uis_btn_kind kind)
{
  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b);
  uis_button(ui, r, b, label, kind, UIS_BTN_S_SM, (uis_state){.hover = hov}, "evbtn", (uint64_t)(uint32_t)b.x * 65536u + (uint32_t)b.y);
}

static void ev_modal_begin(ccord_ui *ui, ccord_renderer *r, ccord_rect *box, float w, float h, const char *title)
{
  ui_events_state *st = events_state(ui, false);
  ui_popup_begin(ui, r, "events", st ? (uint64_t)st->view : 0u, UI_MODAL_MS, 10.0f);
  float mw = fminf(fmaxf(ui->win_w - 64.0f, 300.0f), w);
  float mh = fminf(fmaxf(ui->win_h - 48.0f, 240.0f), h);
  ccord_rect m = uis_modal_box(ui, mw, mh);
  *box = m;
  uis_modal_frame(ui, r, m);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large), COL(text_header));
  ccord_text_draw(r, title, strlen(title), m.x + 20.0f, m.y + 18.0f, &ts, m.w - 160.0f);
}

// One form input: box, current text (or placeholder), selection highlight and caret when focused.
static void ev_input(ccord_ui *ui, ccord_renderer *r, ccord_rect b, const ccord_text_edit *t, bool focused,
                     const char *placeholder)
{
  (void)uis_input(ui, r, b, t, placeholder, (uis_state){.focused = focused}, false, "evin",
                  (uint64_t)(uint32_t)b.x * 65536u + (uint32_t)b.y);
}

static void ev_field_label(ccord_ui *ui, ccord_renderer *r, float x, float y, float w, const char *label)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw(r, label, strlen(label), x, y, &ls, w);
}

// ---------------------------------------------------------------- list view

static void events_draw_list(ccord_ui *ui, ccord_renderer *r)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  ccord_rect box;
  size_t total = ccord_store_scheduled_event_count(ui->d.store, st->guild_id);
  bool compact = ui->mobile_layout || ui->win_w < 460.0f;
  float row_h = compact ? 112.0f : EV_ROW_H;
  // Height follows the row count up to a cap; beyond it the list scrolls inside the modal.
  float want_h = clampf((compact ? 112.0f : 76.0f) + (float)total * row_h, 240.0f, 560.0f);
  ev_modal_begin(ui, r, &box, 600.0f, want_h, "Scheduled Events");
  st->box = box;
  compact = compact || box.w < 440.0f;
  row_h = compact ? 112.0f : EV_ROW_H;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, st->guild_id);
  ccord_text_style gs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  const char *gname = gd && gd->name[0] ? gd->name : "Server";
  ccord_text_draw_ellipsis(r, gname, strlen(gname), box.x + 20.0f, box.y + 40.0f,
                           compact ? (box.w - 40.0f) : (box.w - 220.0f), &gs);

  st->close_btn = (ccord_rect){box.x + box.w - UIS_ICON_BTN - UIS_SP_3, box.y + 12.0f, UIS_ICON_BTN, UIS_ICON_BTN};
  bool chov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, st->close_btn);
  ccord_color cglyph = uis_icon_button(ui, r, st->close_btn, (uis_state){.hover = chov}, "evclose", 0);
  ui_icon_close(r, st->close_btn.x + st->close_btn.w * 0.5f, st->close_btn.y + st->close_btn.h * 0.5f, 12.0f, cglyph);
  st->create_btn = (ccord_rect){0, 0, 0, 0};
  float list_top = box.y + 60.0f;
  if (events_create(ui, st->guild_id)) {
    if (compact) {
      st->create_btn = (ccord_rect){box.x + 16.0f, box.y + 60.0f, box.w - 32.0f, UIS_H_BTN_SM};
      list_top = box.y + 96.0f;
    } else {
      st->create_btn = (ccord_rect){box.x + box.w - 180.0f, box.y + 14.0f, 118.0f, UIS_H_BTN_SM};
    }
    ev_button(ui, r, st->create_btn, "Create Event", UIS_BTN_PRIMARY);
  }

  st->list_view = (ccord_rect){box.x + 16.0f, list_top, box.w - 32.0f, fmaxf(box.y + box.h - 16.0f - list_top, 40.0f)};
  ccord_rect view = st->list_view;
  st->list_content_h = (float)total * row_h;
  float max_scroll = st->list_content_h > view.h ? st->list_content_h - view.h : 0.0f;
  st->scroll = clampf(st->scroll, 0.0f, max_scroll);
  st->row_count = 0;
  ccord_push_clip(r, view);   // scrolled rows must not paint over the header

  if (total == 0) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    const char *empty = "No scheduled events yet.";
    ccord_vec2 et = ccord_text_measure(r, empty, strlen(empty), &es, 0.0f);
    ccord_text_draw(r, empty, strlen(empty), view.x + (view.w - et.x) * 0.5f, view.y + 40.0f, &es, view.w);
    if (events_create(ui, st->guild_id)) {
      const char *hint = "Create one to bring the server together.";
      ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
      ccord_vec2 ht = ccord_text_measure(r, hint, strlen(hint), &hs, 0.0f);
      ccord_text_draw(r, hint, strlen(hint), view.x + (view.w - ht.x) * 0.5f, view.y + 40.0f + et.y + 8.0f,
                      &hs, view.w);
    }
  }

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_secondary));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  for (size_t i = 0; i < total && st->row_count < UI_EVENTS_ROW_MAX; i++) {
    const ccord_scheduled_event *ev = ccord_store_scheduled_event_at(ui->d.store, st->guild_id, i);
    if (!ev) continue;
    float ry = view.y + (float)i * row_h - st->scroll;
    if (ry + row_h < view.y || ry > view.y + view.h) continue;   // cull
    bool fully_visible = ry >= view.y && ry + row_h <= view.y + view.h;
    ccord_rect row = {view.x, ry, view.w, row_h - 6.0f};
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, row);
    uis_plate(ui, r, row, uis_tokens_of(ui).r_control, COL(bg_tertiary));
    uis_list_row_fill(ui, r, row, (uis_state){.hover = hov}, "evrow", (uint64_t)i);

    float tx = row.x + 12.0f;
    float tw = compact ? fmaxf(row.w - 24.0f, 40.0f) : fmaxf(row.w - 200.0f, 40.0f);
    ccord_text_draw_ellipsis(r, ev->name, strlen(ev->name), tx, row.y + 8.0f, tw, &ns);
    // Status · kind · where.
    char line[224];
    char where_buf[128];
    if (ev->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL) {
      snprintf(where_buf, sizeof where_buf, "%s", ev->location[0] ? ev->location : "External");
      snprintf(line, sizeof line, "%s · External · %s", event_status_label(ev->status), where_buf);
    } else {
      const ccord_channel *ch = ev->channel_id != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, ev->channel_id)
                                                                : nullptr;
      snprintf(where_buf, sizeof where_buf, "#%s", ch && ch->name[0] ? ch->name : "unknown");
      snprintf(line, sizeof line, "%s · %s · %s", event_status_label(ev->status),
               ev->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE ? "Stage" : "Voice", where_buf);
    }
    ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f,
                                    event_status_color(ui, ev->status));
    // Draw the status word in its own colour, the rest muted.
    const char *status_word = event_status_label(ev->status);
    ccord_text_draw(r, status_word, strlen(status_word), tx, row.y + 28.0f, &sts, tw);
    ccord_vec2 sw = ccord_text_measure(r, status_word, strlen(status_word), &sts, 0.0f);
    const char *rest = line + strlen(status_word);
    ccord_text_draw_ellipsis(r, rest, strlen(rest), tx + sw.x, row.y + 28.0f, fmaxf(tw - sw.x, 16.0f), &ss);
    // Start time · interested count.
    char when[128];
    if (ev->scheduled_start_ms > 0) {
      char date_part[48], clock_part[32];
      (void)ccord_timefmt_date(date_part, sizeof date_part, ev->scheduled_start_ms, ui->use_utc);
      (void)ccord_timefmt_clock(clock_part, sizeof clock_part, ev->scheduled_start_ms, ui->use_utc);
      snprintf(when, sizeof when, "%s · %s", date_part, clock_part);
    } else {
      snprintf(when, sizeof when, "Start time not set");
    }
    if (ev->user_count > 0) {
      size_t wl = strlen(when);
      snprintf(when + wl, sizeof when - wl, " · %u interested", (unsigned)ev->user_count);
    }
    ccord_text_draw_ellipsis(r, when, strlen(when), tx, row.y + 48.0f, tw, &ms);

    // Action buttons: right column on desktop, stacked bottom row on compact/mobile cards.
    if (fully_visible && st->row_count < UI_EVENTS_ROW_MAX) {
      size_t ri = st->row_count;
      st->rows[ri].event_id = ev->id;
      st->rows[ri].row = row;
      st->rows[ri].rsvp_on = ev->me_rsvped;
      st->rows[ri].delete_armed = st->delete_armed == ev->id;
      st->rows[ri].edit = (ccord_rect){0, 0, 0, 0};
      st->rows[ri].del = (ccord_rect){0, 0, 0, 0};
      bool can_mgr = event_can_manage(ui, st->guild_id, ev);
      if (compact) {
        float by = row.y + row.h - UIS_H_BTN_SM - 8.0f;
        float inner_w = row.w - 24.0f;
        if (can_mgr) {
          float bw = floorf((inner_w - 12.0f) / 3.0f);
          st->rows[ri].rsvp = (ccord_rect){row.x + 12.0f, by, bw, UIS_H_BTN_SM};
          st->rows[ri].edit = (ccord_rect){row.x + 12.0f + bw + 6.0f, by, bw, UIS_H_BTN_SM};
          st->rows[ri].del = (ccord_rect){row.x + 12.0f + 2.0f * (bw + 6.0f), by, inner_w - 2.0f * (bw + 6.0f), UIS_H_BTN_SM};
        } else {
          st->rows[ri].rsvp = (ccord_rect){row.x + 12.0f, by, inner_w, UIS_H_BTN_SM};
        }
      } else {
        st->rows[ri].rsvp = (ccord_rect){row.x + row.w - 170.0f, row.y + 9.0f, 158.0f, UIS_H_BTN_SM};
        if (can_mgr) {
          st->rows[ri].edit = (ccord_rect){row.x + row.w - 170.0f, row.y + 44.0f, 76.0f, UIS_H_BTN_SM};
          st->rows[ri].del = (ccord_rect){row.x + row.w - 86.0f, row.y + 44.0f, 74.0f, UIS_H_BTN_SM};
        }
      }
      ev_button(ui, r, st->rows[ri].rsvp, ev->me_rsvped ? "Interested ✓" : "Interested",
                ev->me_rsvped ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY);
      if (can_mgr) {
        bool armed = st->delete_armed == ev->id;
        ev_button(ui, r, st->rows[ri].edit, armed ? "Keep" : "Edit", UIS_BTN_SECONDARY);
        ev_button(ui, r, st->rows[ri].del, armed ? "Confirm" : "Delete", UIS_BTN_DANGER);
      }
      st->row_count++;
    }
  }
  ccord_pop_clip(r);
  st->list_thumb = ui_draw_list_scrollbar(ui, r, view, st->list_content_h, st->scroll, "events", 0);
  ui_popup_end(r);
}

// ---------------------------------------------------------------- form view

static void events_draw_form(ccord_ui *ui, ccord_renderer *r)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  ccord_rect box;
  ev_modal_begin(ui, r, &box, 560.0f, 480.0f, st->edit_id != CCORD_ID_NONE ? "Edit Event" : "Create Event");
  st->box = box;
  float x = box.x + 20.0f, w = box.w - 40.0f;
  st->f_cancel = (ccord_rect){box.x + box.w - 196.0f, box.y + box.h - 48.0f, 84.0f, UIS_H_BTN};
  st->f_save = (ccord_rect){box.x + box.w - 104.0f, box.y + box.h - 48.0f, 84.0f, UIS_H_BTN};
  float body_top = box.y + 52.0f;
  st->f_view = (ccord_rect){box.x + 12.0f, body_top, box.w - 24.0f, fmaxf(st->f_cancel.y - 10.0f - body_top, 40.0f)};
  st->form_scroll = clampf(st->form_scroll, 0.0f, st->form_max);
  float fy = body_top - st->form_scroll;
  bool external = st->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL;

  ccord_push_clip(r, st->f_view);
  ev_field_label(ui, r, x, fy, w, "NAME");
  st->f_name = (ccord_rect){x, fy + 16.0f, w, UIS_H_INPUT};
  ev_input(ui, r, st->f_name, &st->name, st->focus == 0, "Movie night");
  fy += 56.0f;

  // Type chip cycles Stage -> Voice -> External; the channel chip picks among eligible channels.
  const char *type_lbl = external ? "Type: External"
                       : st->entity_type == CCORD_SCHEDULED_EVENT_VOICE ? "Type: Voice"
                                                                        : "Type: Stage";
  bool stack_tc = (!external && w < 330.0f);
  float type_w = stack_tc ? w : fminf(150.0f, w);
  st->f_type = (ccord_rect){x, fy, type_w, UIS_H_BTN_SM};
  ev_button(ui, r, st->f_type, type_lbl, UIS_BTN_SECONDARY);
  st->f_channel = (ccord_rect){0, 0, 0, 0};
  if (!external) {
    ccord_id ids[EV_CHANNEL_MAX];
    size_t n = event_channels(ui, st->guild_id, st->entity_type, ids, EV_CHANNEL_MAX);
    char ch_lbl[140];
    if (n == 0) {
      snprintf(ch_lbl, sizeof ch_lbl, "No %s channel",
               st->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE ? "stage" : "voice");
    } else {
      ccord_id sel = ids[st->channel_sel < n ? st->channel_sel : 0];
      const ccord_channel *ch = ccord_store_channel(ui->d.store, sel);
      snprintf(ch_lbl, sizeof ch_lbl, "Channel: #%s", ch && ch->name[0] ? ch->name : "unknown");
    }
    if (stack_tc) {
      fy += UIS_H_BTN_SM + 6.0f;
      st->f_channel = (ccord_rect){x, fy, w, UIS_H_BTN_SM};
    } else {
      st->f_channel = (ccord_rect){x + type_w + 8.0f, fy, fmaxf(w - type_w - 8.0f, 60.0f), UIS_H_BTN_SM};
    }
    ev_button(ui, r, st->f_channel, ch_lbl, UIS_BTN_SECONDARY);
  }
  fy += 36.0f;

  st->f_location = (ccord_rect){0, 0, 0, 0};
  if (external) {
    ev_field_label(ui, r, x, fy, w, "LOCATION");
    st->f_location = (ccord_rect){x, fy + 16.0f, w, UIS_H_INPUT};
    ev_input(ui, r, st->f_location, &st->location, st->focus == 1, "Where it happens");
    fy += 56.0f;
  }

  ev_field_label(ui, r, x, fy, w, "STARTS (ISO 8601 OR +2h)");
  st->f_start = (ccord_rect){x, fy + 16.0f, w, UIS_H_INPUT};
  ev_input(ui, r, st->f_start, &st->start, st->focus == 2, "2026-11-14T18:00Z");
  fy += 56.0f;

  ev_field_label(ui, r, x, fy, w, external ? "ENDS (REQUIRED, AFTER START)" : "ENDS (OPTIONAL)");
  st->f_end = (ccord_rect){x, fy + 16.0f, w, UIS_H_INPUT};
  ev_input(ui, r, st->f_end, &st->end, st->focus == 3, external ? "2026-11-14T20:00Z" : "None");
  fy += 56.0f;

  ev_field_label(ui, r, x, fy, w, "DESCRIPTION");
  st->f_desc = (ccord_rect){x, fy + 16.0f, w, UIS_H_INPUT};
  ev_input(ui, r, st->f_desc, &st->desc, st->focus == 4, "What's happening (optional)");
  fy += 56.0f;

  if (st->form_error[0]) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(danger));
    ccord_text_draw_ellipsis(r, st->form_error, strlen(st->form_error), x, fy, w, &es);
    fy += 22.0f;
  }
  st->form_content_h = fmaxf((fy + st->form_scroll) - body_top + 6.0f, 0.0f);
  st->form_max = st->form_content_h > st->f_view.h ? (st->form_content_h - st->f_view.h) : 0.0f;
  ccord_pop_clip(r);
  if (st->form_max > 0.0f) {
    (void)ui_draw_list_scrollbar(ui, r, st->f_view, st->form_content_h, st->form_scroll, "evformscroll", 0);
  }

  ev_button(ui, r, st->f_cancel, "Cancel", UIS_BTN_SECONDARY);
  ev_button(ui, r, st->f_save, "Save", UIS_BTN_PRIMARY);
  ui_popup_end(r);
}

static void form_set_error(ui_events_state *st, const char *msg)
{
  snprintf(st->form_error, sizeof st->form_error, "%s", msg);
}

static void form_submit(ccord_ui *ui)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  const char *name = ccord_text_edit_cstr(&st->name);
  const char *desc = ccord_text_edit_cstr(&st->desc);
  const char *location = ccord_text_edit_cstr(&st->location);
  const char *start_text = ccord_text_edit_cstr(&st->start);
  const char *end_text = ccord_text_edit_cstr(&st->end);
  bool external = st->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL;

  int64_t start_ms = 0, end_ms = 0;
  if (!parse_event_time(start_text, ui->now_unix_ms, &start_ms)) {
    form_set_error(st, "Start time: use ISO 8601 (2026-11-14T18:00Z) or an offset like +2h.");
    ui->dirty = true;
    return;
  }
  if (end_text[0] && !parse_event_time(end_text, ui->now_unix_ms, &end_ms)) {
    form_set_error(st, "End time: use ISO 8601 or an offset like +3h (empty = none).");
    ui->dirty = true;
    return;
  }
  if (external) {
    if (!location[0]) {
      form_set_error(st, "External events need a location.");
      ui->dirty = true;
      return;
    }
    if (!end_text[0]) {
      form_set_error(st, "External events need an end time.");
      ui->dirty = true;
      return;
    }
  }
  if (end_ms != 0 && end_ms <= start_ms) {
    form_set_error(st, "The end time must be after the start time.");
    ui->dirty = true;
    return;
  }
  ccord_id channel = CCORD_ID_NONE;
  if (!external) {
    channel = form_channel(ui, st);
    if (channel == CCORD_ID_NONE) {
      form_set_error(st, st->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE
                           ? "This server has no stage channel for the event."
                           : "This server has no voice channel for the event.");
      ui->dirty = true;
      return;
    }
  }

  ccord_admin_request req;
  ccord_error err;
  ccord_status rc;
  if (st->edit_id == CCORD_ID_NONE) {
    ccord_scheduled_event ev = {0};
    ev.guild_id = st->guild_id;
    ev.entity_type = (uint8_t)st->entity_type;
    ev.channel_id = channel;
    ev.privacy_level = 2;
    ev.scheduled_start_ms = start_ms;
    ev.scheduled_end_ms = end_ms;
    snprintf(ev.name, sizeof ev.name, "%s", name);
    snprintf(ev.description, sizeof ev.description, "%s", desc);
    snprintf(ev.location, sizeof ev.location, "%s", external ? location : "");
    rc = ccord_admin_event_create(&req, st->guild_id, &ev, nullptr, &err);
  } else {
    ccord_admin_event_edit edit = CCORD_ADMIN_EVENT_EDIT_INIT;
    edit.name = name;
    edit.description = desc;
    edit.start_ms = start_ms;
    edit.end_ms = end_ms;                       // 0 clears the end time
    edit.entity_type = (int8_t)st->entity_type;
    edit.channel_id = external ? 0 : (int64_t)channel;
    edit.location = external ? location : "";   // non-external events have no location
    rc = ccord_admin_event_modify(&req, st->guild_id, st->edit_id, &edit, nullptr, &err);
  }
  if (rc != CCORD_OK) {
    form_set_error(st, err.message);
    ui->dirty = true;
    return;   // keep the form open so the values can be fixed
  }
  ui_admin_submit(ui, &req);   // the store changes when Discord answers
  form_back_to_list(ui);
}

// ---------------------------------------------------------------- stage prompts

static void stage_start_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  ccord_rect box;
  ev_modal_begin(ui, r, &box, 480.0f, 190.0f, "Start Stage");
  st->sp_box = box;
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  const char *hint = "Give the stage a topic (max 120 characters). Empty starts without one.";
  ccord_text_draw_ellipsis(r, hint, strlen(hint), box.x + 20.0f, box.y + 46.0f, box.w - 40.0f, &hs);
  st->sp_input = (ccord_rect){box.x + 20.0f, box.y + 72.0f, box.w - 40.0f, UIS_H_INPUT};
  ev_input(ui, r, st->sp_input, &st->stage_topic, true, "Stage topic");
  st->sp_cancel = (ccord_rect){box.x + box.w - 216.0f, box.y + box.h - 48.0f, 84.0f, UIS_H_BTN};
  st->sp_ok = (ccord_rect){box.x + box.w - 124.0f, box.y + box.h - 48.0f, 104.0f, UIS_H_BTN};
  ev_button(ui, r, st->sp_cancel, "Cancel", UIS_BTN_SECONDARY);
  ev_button(ui, r, st->sp_ok, "Start Stage", UIS_BTN_PRIMARY);
  ui_popup_end(r);
}

static void stage_start_submit(ccord_ui *ui)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  const char *topic = ccord_text_edit_cstr(&st->stage_topic);
  if (ccord_session_stage_start(ui->d.session, st->stage_guild, st->stage_channel, topic, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't start the stage: %s", err.message);
    return;   // keep the prompt open so the topic can be fixed
  }
  events_close(ui);   // the instance itself arrives as STAGE_INSTANCE_UPDATE
}

static void stage_end_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  ccord_rect box;
  ev_modal_begin(ui, r, &box, 480.0f, 200.0f, "End Stage");
  st->ec_box = box;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, st->stage_channel);
  char line1[160];
  snprintf(line1, sizeof line1, "End the stage in #%s?", ch && ch->name[0] ? ch->name : "this channel");
  ccord_text_style bs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
  ccord_text_draw_ellipsis(r, line1, strlen(line1), box.x + 20.0f, box.y + 52.0f, box.w - 40.0f, &bs);
  const char *line2 = "The stage instance is deleted and speakers return to the audience.";
  ccord_text_style b2 = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, line2, strlen(line2), box.x + 20.0f, box.y + 52.0f + ui_line_h(ui, bs.size) + 4.0f,
                           box.w - 40.0f, &b2);
  st->ec_cancel = (ccord_rect){box.x + box.w - 216.0f, box.y + box.h - 48.0f, 84.0f, UIS_H_BTN};
  st->ec_ok = (ccord_rect){box.x + box.w - 124.0f, box.y + box.h - 48.0f, 104.0f, UIS_H_BTN};
  ev_button(ui, r, st->ec_cancel, "Cancel", UIS_BTN_SECONDARY);
  ev_button(ui, r, st->ec_ok, "End Stage", UIS_BTN_DANGER);
  ui_popup_end(r);
}

static void stage_end_submit(ccord_ui *ui)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  ccord_error err;
  if (ccord_session_stage_end(ui->d.session, st->stage_channel, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't end the stage: %s", err.message);
    return;
  }
  events_close(ui);   // the removal arrives as STAGE_INSTANCE_UPDATE (stage_deleted)
}

// ---------------------------------------------------------------- draw & input

void ui_scheduled_events_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_scheduled_events_active(ui)) return;
  ui_events_state *st = events_state(ui, false);
  if (!st) return;
  // Geometry is rebuilt every draw; drop the previous frame's hits first.
  st->row_count = 0;
  memset(st->rows, 0, sizeof st->rows);
  st->create_btn = st->close_btn = st->list_view = st->list_thumb = (ccord_rect){0, 0, 0, 0};
  st->f_name = st->f_location = st->f_start = st->f_end = st->f_desc = (ccord_rect){0, 0, 0, 0};
  st->f_type = st->f_channel = st->f_save = st->f_cancel = (ccord_rect){0, 0, 0, 0};
  st->sp_box = st->sp_input = st->sp_ok = st->sp_cancel = (ccord_rect){0, 0, 0, 0};
  st->ec_box = st->ec_ok = st->ec_cancel = (ccord_rect){0, 0, 0, 0};
  switch (st->view) {
    case UI_EVENTS_LIST: events_draw_list(ui, r); break;
    case UI_EVENTS_FORM: events_draw_form(ui, r); break;
    case UI_EVENTS_STAGE_START: stage_start_draw(ui, r); break;
    case UI_EVENTS_STAGE_END: stage_end_draw(ui, r); break;
  }
}

// Shared text-editing keys for the single-line inputs (the voice prompt's key map).
static bool edit_key(ccord_text_edit *t, const ccord_input_event *ev)
{
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(t);
      else ccord_text_edit_backspace(t);
      return true;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(t);
      else ccord_text_edit_delete(t);
      return true;
    case CCORD_KEY_LEFT: ccord_text_edit_move(t, ctrl ? -3 : -1, shift); return true;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(t, ctrl ? 3 : 1, shift); return true;
    case CCORD_KEY_HOME: ccord_text_edit_move(t, -2, shift); return true;
    case CCORD_KEY_END: ccord_text_edit_move(t, 2, shift); return true;
    case CCORD_KEY_A:
      if (ctrl) {
        ccord_text_edit_select_all(t);
        return true;
      }
      return false;
    case CCORD_KEY_Z:
      if (ctrl) {
        if (shift) ccord_text_edit_redo(t);
        else ccord_text_edit_undo(t);
        return true;
      }
      return false;
    case CCORD_KEY_Y:
      if (ctrl) {
        ccord_text_edit_redo(t);
        return true;
      }
      return false;
    default: return false;
  }
}

static ccord_text_edit *form_field(ui_events_state *st, int focus)
{
  switch (focus) {
    case 0: return &st->name;
    case 1: return &st->location;
    case 2: return &st->start;
    case 3: return &st->end;
    case 4: return &st->desc;
    default: return &st->name;
  }
}

static int form_focus_next(const ui_events_state *st, int dir)
{
  bool external = st->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL;
  int f = st->focus;
  for (int step = 0; step < 5; step++) {
    f = (f + dir + 5) % 5;
    if (f == 1 && !external) continue;   // the location field only exists for external events
    return f;
  }
  return st->focus;
}

// Ticks the list glide (scroll.h) by fdt_ms; the glide is dropped when the modal is shut.
bool ui_events_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  ui_events_state *st = events_state(ui, false);
  if (!st || st->glide.pending == 0.0f) return false;
  if (!st->open) {
    ccord_scroller_cancel(&st->glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&st->glide, &st->scroll, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

static bool events_handle_list(ccord_ui *ui, const ccord_input_event *ev)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return false;
  if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_ESCAPE) {
    events_close(ui);
    return true;
  }
  if (ev->type == CCORD_IN_WHEEL) {
    float max_s = st->list_content_h > st->list_view.h ? st->list_content_h - st->list_view.h : 0.0f;
    ccord_scroller_scroll(&st->glide, &st->scroll, -ev->wheel * 48.0f, max_s, ui_wheel_direct(ui, ev));
    ui->last_scroll_ms = ui->now_ms ? ui->now_ms : 1;
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->close_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, st->close_btn)) {
      events_close(ui);
      return true;
    }
    if (st->create_btn.w > 0.0f && ui_in_rect(ev->x, ev->y, st->create_btn)) {
      form_open(ui, CCORD_ID_NONE);
      return true;
    }
    for (size_t i = 0; i < st->row_count; i++) {
      if (st->rows[i].rsvp.w > 0.0f && ui_in_rect(ev->x, ev->y, st->rows[i].rsvp)) {
        if (ui->d.session) {
          ccord_error err;
          if (ccord_session_rsvp_scheduled_event(ui->d.session, st->guild_id, st->rows[i].event_id,
                                                 !st->rows[i].rsvp_on, &err) != CCORD_OK) {
            ui_notice(ui, "Couldn't update your RSVP: %s", err.message);
          }
        } else {
          ui_notice(ui, "Not connected");
        }
        ui->dirty = true;
        return true;
      }
      if (st->rows[i].edit.w > 0.0f && ui_in_rect(ev->x, ev->y, st->rows[i].edit)) {
        if (st->rows[i].delete_armed) {
          st->delete_armed = CCORD_ID_NONE;   // "Keep": disarm the delete
          ui->dirty = true;
        } else {
          form_open(ui, st->rows[i].event_id);
        }
        return true;
      }
      if (st->rows[i].del.w > 0.0f && ui_in_rect(ev->x, ev->y, st->rows[i].del)) {
        if (st->delete_armed == st->rows[i].event_id) {
          ccord_admin_request req;
          ccord_error err;
          if (ccord_admin_event_delete(&req, st->guild_id, st->rows[i].event_id, nullptr, &err) == CCORD_OK) {
            ui_admin_submit(ui, &req);
          } else {
            ui_notice(ui, "%s", err.message);
          }
          st->delete_armed = CCORD_ID_NONE;
        } else {
          st->delete_armed = st->rows[i].event_id;   // first click arms the confirm
        }
        ui->dirty = true;
        return true;
      }
    }
    if (st->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->box)) events_close(ui);
    return true;
  }
  return true;   // modal: swallow everything else
}

static bool events_handle_form(ccord_ui *ui, const ccord_input_event *ev)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return false;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
    if (ev->key == CCORD_KEY_ESCAPE) {
      form_back_to_list(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      form_submit(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_TAB) {
      st->focus = form_focus_next(st, shift ? -1 : 1);
      ui->dirty = true;
      return true;
    }
    if (edit_key(form_field(st, st->focus), ev)) {
      ui->dirty = true;
      return true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_TEXT) {
    size_t n = strnlen(ev->text, sizeof ev->text);
    ccord_error err;
    if (ccord_text_edit_insert(form_field(st, st->focus), ev->text, n, &err) != CCORD_OK) {
      ui_notice(ui, "%s", err.message);
    }
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_WHEEL) {
    if (st->form_max > 0.0f) {
      st->form_scroll = clampf(st->form_scroll - ev->wheel * 36.0f, 0.0f, st->form_max);
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->f_save.w > 0.0f && ui_in_rect(ev->x, ev->y, st->f_save)) {
      form_submit(ui);
      return true;
    }
    if (st->f_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, st->f_cancel)) {
      form_back_to_list(ui);
      return true;
    }
    if (st->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->box)) {
      events_close(ui);
      return true;
    }
    if (st->f_view.h > 0.0f && !ui_in_rect(ev->x, ev->y, st->f_view)) return true;
    if (st->f_type.w > 0.0f && ui_in_rect(ev->x, ev->y, st->f_type)) {
      st->entity_type = st->entity_type % 3 + 1;   // Stage -> Voice -> External -> Stage
      st->channel_sel = 0;
      st->form_error[0] = '\0';
      if (st->focus == 1 && st->entity_type != CCORD_SCHEDULED_EVENT_EXTERNAL) st->focus = 0;
      ui->dirty = true;
      return true;
    }
    if (st->f_channel.w > 0.0f && ui_in_rect(ev->x, ev->y, st->f_channel)) {
      ccord_id ids[EV_CHANNEL_MAX];
      size_t n = event_channels(ui, st->guild_id, st->entity_type, ids, EV_CHANNEL_MAX);
      if (n > 0) st->channel_sel = (st->channel_sel + 1) % n;
      ui->dirty = true;
      return true;
    }
    const ccord_rect *rects[5] = {&st->f_name, &st->f_location, &st->f_start, &st->f_end, &st->f_desc};
    for (int f = 0; f < 5; f++) {
      if (rects[f]->w > 0.0f && ui_in_rect(ev->x, ev->y, *rects[f])) {
        st->focus = f;
        ui->dirty = true;
        return true;
      }
    }
    if (st->box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->box)) events_close(ui);
    return true;
  }
  return true;
}

static bool events_handle_stage_start(ccord_ui *ui, const ccord_input_event *ev)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return false;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      events_close(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      stage_start_submit(ui);
      return true;
    }
    if (edit_key(&st->stage_topic, ev)) {
      ui->dirty = true;
      return true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_TEXT) {
    size_t n = strnlen(ev->text, sizeof ev->text);
    ccord_error err;
    if (ccord_text_edit_insert(&st->stage_topic, ev->text, n, &err) != CCORD_OK) {
      ui_notice(ui, "%s", err.message);
    }
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->sp_ok.w > 0.0f && ui_in_rect(ev->x, ev->y, st->sp_ok)) {
      stage_start_submit(ui);
      return true;
    }
    if (st->sp_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, st->sp_cancel)) {
      events_close(ui);
      return true;
    }
    if (st->sp_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->sp_box)) events_close(ui);
    return true;
  }
  return true;
}

static bool events_handle_stage_end(ccord_ui *ui, const ccord_input_event *ev)
{
  ui_events_state *st = events_state(ui, false);
  if (!st) return false;
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      events_close(ui);
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      stage_end_submit(ui);
      return true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (st->ec_ok.w > 0.0f && ui_in_rect(ev->x, ev->y, st->ec_ok)) {
      stage_end_submit(ui);
      return true;
    }
    if (st->ec_cancel.w > 0.0f && ui_in_rect(ev->x, ev->y, st->ec_cancel)) {
      events_close(ui);
      return true;
    }
    if (st->ec_box.w > 0.0f && !ui_in_rect(ev->x, ev->y, st->ec_box)) events_close(ui);
    return true;
  }
  return true;
}

bool ui_scheduled_events_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_scheduled_events_active(ui) || !ev) return false;
  ui_events_state *st = events_state(ui, false);
  if (!st) return false;
  switch (st->view) {
    case UI_EVENTS_LIST: return events_handle_list(ui, ev);
    case UI_EVENTS_FORM: return events_handle_form(ui, ev);
    case UI_EVENTS_STAGE_START: return events_handle_stage_start(ui, ev);
    case UI_EVENTS_STAGE_END: return events_handle_stage_end(ui, ev);
  }
  return true;
}

// ---------------------------------------------------------------- introspection

ccord_cursor_kind ui_scheduled_events_cursor(const ccord_ui *ui)
{
  if (!ui_scheduled_events_active(ui) || !ui->mouse_in) return CCORD_CURSOR_DEFAULT;
  ui_events_debug d;
  ui_scheduled_events_get_debug(ui, &d);
  float x = ui->mx, y = ui->my;
  switch (d.view) {
    case UI_EVENTS_LIST:
      if ((d.close_btn.w > 0.0f && ui_in_rect(x, y, d.close_btn)) ||
          (d.create_btn.w > 0.0f && ui_in_rect(x, y, d.create_btn))) {
        return CCORD_CURSOR_POINTER;
      }
      for (size_t i = 0; i < d.row_count; i++) {
        if ((d.rows[i].rsvp.w > 0.0f && ui_in_rect(x, y, d.rows[i].rsvp)) ||
            (d.rows[i].edit.w > 0.0f && ui_in_rect(x, y, d.rows[i].edit)) ||
            (d.rows[i].del.w > 0.0f && ui_in_rect(x, y, d.rows[i].del))) {
          return CCORD_CURSOR_POINTER;
        }
      }
      return CCORD_CURSOR_DEFAULT;
    case UI_EVENTS_FORM:
      if ((d.name_input.w > 0.0f && ui_in_rect(x, y, d.name_input)) ||
          (d.location_input.w > 0.0f && ui_in_rect(x, y, d.location_input)) ||
          (d.start_input.w > 0.0f && ui_in_rect(x, y, d.start_input)) ||
          (d.end_input.w > 0.0f && ui_in_rect(x, y, d.end_input)) ||
          (d.desc_input.w > 0.0f && ui_in_rect(x, y, d.desc_input))) {
        return CCORD_CURSOR_TEXT;
      }
      if ((d.type_chip.w > 0.0f && ui_in_rect(x, y, d.type_chip)) ||
          (d.channel_chip.w > 0.0f && ui_in_rect(x, y, d.channel_chip)) ||
          (d.save_btn.w > 0.0f && ui_in_rect(x, y, d.save_btn)) ||
          (d.cancel_btn.w > 0.0f && ui_in_rect(x, y, d.cancel_btn))) {
        return CCORD_CURSOR_POINTER;
      }
      return CCORD_CURSOR_DEFAULT;
    case UI_EVENTS_STAGE_START:
      if (d.stage_input.w > 0.0f && ui_in_rect(x, y, d.stage_input)) return CCORD_CURSOR_TEXT;
      if ((d.stage_ok.w > 0.0f && ui_in_rect(x, y, d.stage_ok)) ||
          (d.stage_cancel.w > 0.0f && ui_in_rect(x, y, d.stage_cancel))) {
        return CCORD_CURSOR_POINTER;
      }
      return CCORD_CURSOR_DEFAULT;
    case UI_EVENTS_STAGE_END:
      if ((d.stage_ok.w > 0.0f && ui_in_rect(x, y, d.stage_ok)) ||
          (d.stage_cancel.w > 0.0f && ui_in_rect(x, y, d.stage_cancel))) {
        return CCORD_CURSOR_POINTER;
      }
      return CCORD_CURSOR_DEFAULT;
  }
  return CCORD_CURSOR_DEFAULT;
}

void ui_scheduled_events_get_debug(const ccord_ui *ui, ui_events_debug *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!ui) return;
  const ui_events_state *st = events_state(ui, false);
  if (!st) return;
  out->open = st->open;
  out->view = st->view;
  out->guild_id = st->guild_id;
  out->edit_event_id = st->edit_id;
  out->box = st->box;
  out->create_btn = st->create_btn;
  out->close_btn = st->close_btn;
  out->list_view = st->list_view;
  out->list_scroll = st->scroll;
  out->list_content_h = st->list_content_h;
  out->list_thumb = st->list_thumb;
  out->row_count = st->row_count;
  for (size_t i = 0; i < st->row_count && i < UI_EVENTS_ROW_MAX; i++) {
    out->rows[i].event_id = st->rows[i].event_id;
    out->rows[i].row = st->rows[i].row;
    out->rows[i].rsvp = st->rows[i].rsvp;
    out->rows[i].edit = st->rows[i].edit;
    out->rows[i].del = st->rows[i].del;
    out->rows[i].rsvp_on = st->rows[i].rsvp_on;
    out->rows[i].delete_armed = st->rows[i].delete_armed;
  }
  out->name_input = st->f_name;
  out->location_input = st->f_location;
  out->start_input = st->f_start;
  out->end_input = st->f_end;
  out->desc_input = st->f_desc;
  out->type_chip = st->f_type;
  out->channel_chip = st->f_channel;
  out->save_btn = st->f_save;
  out->cancel_btn = st->f_cancel;
  out->entity_type = st->entity_type;
  out->channel_id = st->view == UI_EVENTS_FORM ? form_channel(ui, st) : CCORD_ID_NONE;
  out->focus_field = st->focus;
  snprintf(out->form_error, sizeof out->form_error, "%s", st->form_error);
  out->stage_box = st->view == UI_EVENTS_STAGE_START ? st->sp_box : st->ec_box;
  out->stage_input = st->sp_input;
  out->stage_ok = st->view == UI_EVENTS_STAGE_START ? st->sp_ok : st->ec_ok;
  out->stage_cancel = st->view == UI_EVENTS_STAGE_START ? st->sp_cancel : st->ec_cancel;
  out->stage_channel = st->stage_channel;
  out->stage_topic[0] = '\0';
  if (st->stage_topic_init) {
    snprintf(out->stage_topic, sizeof out->stage_topic, "%s", ccord_text_edit_cstr(&st->stage_topic));
  }
}
