// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---- Embedded Activities (shelf modal + voice-stage instance cards) -----------------------------
// Owner-tracked static state like g_stage: struct ccord_ui is owned by ui.c. The geometry is rebuilt
// every draw; clicks are matched in ui_voice_stage_handle (stage) / ui_activity_shelf_handle (modal).
// Launch / join / leave are session intents — instances only appear once the transport answers.
static ui_activity_state g_activity;

ui_activity_state *dc_activity_state(const ccord_ui *ui)
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
  ui_activity_state *st = dc_activity_state(ui);
  if (st->list_glide.pending == 0.0f) return false;
  if (!ui_activity_shelf_active(ui)) {
    ccord_scroller_cancel(&st->list_glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&st->list_glide, &st->list_scroll, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}


void ui_activity_shelf_open(ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  if (!ui || guild_id == CCORD_ID_NONE || channel_id == CCORD_ID_NONE) return;
  if (ui->modal_kind != UI_MODAL_NONE) return;
  ui_activity_state *st = dc_activity_state(ui);
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
  const ui_activity_state *st = dc_activity_state(ui);
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
  ui_activity_state *st = dc_activity_state(ui);
  size_t n = 0;
  const ccord_activity_app *apps = activity_shelf_apps(ui, st->guild_id, &n);
  if (idx >= n) return;
  ccord_id channel_id = st->channel_id, app_id = apps[idx].application_id;
  activity_shelf_close(ui);
  activity_launch(ui, channel_id, app_id);
}

void ui_activity_shelf_draw(ccord_ui *ui, ccord_renderer *r)
{
  ui_activity_state *st = dc_activity_state(ui);
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
  ui_activity_state *st = dc_activity_state(ui);
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
  const ui_activity_state *st = dc_activity_state(ui);
  if (st->close_btn.w > 0.0f && ui_in_rect(ui->mx, ui->my, st->close_btn)) return CCORD_CURSOR_POINTER;
  return activity_shelf_row_at(st, ui->mx, ui->my) != SIZE_MAX ? CCORD_CURSOR_POINTER : CCORD_CURSOR_DEFAULT;
}

// A small labelled chip button used for the status / topic / request-to-speak row.
ccord_rect dc_stage_chip(ccord_ui *ui, ccord_renderer *r, float x, float cy, const char *label,
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

size_t dc_activity_instance_total(const ccord_ui *ui, const ccord_channel *ch, const ccord_activity_instance **out)
{
  size_t n = 0;
  *out = ui->d.session ? ccord_session_activity_instances(ui->d.session, ch->id, &n) : nullptr;
  if (!*out) return 0;
  return n < UI_ACTIVITY_INST_MAX ? n : UI_ACTIVITY_INST_MAX;
}

// Vertical space the block takes inside the stage scroll area (0 when nothing runs).
float dc_activity_block_h(size_t n)
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
void dc_activity_draw_block(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch,
                                const ccord_activity_instance *inst, size_t n, float x, float y, float w,
                                ccord_rect clip)
{
  if (n == 0) return;
  ui_activity_state *st = dc_activity_state(ui);
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
bool dc_activity_stage_click(ccord_ui *ui, const ccord_channel *ch, float x, float y)
{
  ui_activity_state *st = dc_activity_state(ui);
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
      size_t n = dc_activity_instance_total(ui, ch, &rows);
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

