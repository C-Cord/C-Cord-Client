// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Motion area "nav": rail pill and icon radius, folder expand, category collapse and channel row selection
// (see verify_motion.c). Drives the real UI over the offline mock: hovers and clicks arrive as input events, and each
// check reads the value the draw pass recorded on that frame (ui->dbg_*), so nothing here re-implements the easing.
// Time is virtual: one frame is MN_FRAME_MS, and every transition must reach its end value within MN_SETTLE_MS.
#include "verify_ui_parity.h"

#include <math.h>
#include <stdio.h>

#include "ui_internal.h"

#define MN_FRAME_MS 16u
#define MN_SETTLE_MS 300u
#define MN_FOLDER_ID 0x7001u

typedef struct mn {
  const ccord_verify_ui_parity_ctx *c;
  int checks;
  char fail[200];
} mn;

// A value the draw pass recorded: a rail guild's pill height, icon radius or alpha, a channel-list row's content y
// (by channel id), or the selection fade of the selected channel row.
typedef enum { MP_PILL, MP_RADIUS, MP_ALPHA, MP_CROW, MP_SEL } mp_kind;
typedef struct mp {
  mp_kind kind;
  ccord_id id;   // rail guild id, channel id; unused for MP_SEL
} mp;

// One transition: `p` starts at `from` and must end at `to`.
typedef struct mt {
  mp p;
  float from, to;
} mt;

static bool mn_check(mn *m, bool ok, const char *what)
{
  if (!ok) {
    if (!m->fail[0]) snprintf(m->fail, sizeof m->fail, "%s", what);
    return false;
  }
  m->checks++;
  return true;
}

#define MN(cond, what)                              \
  do {                                              \
    if (!mn_check(m, (cond), (what))) return false; \
  } while (0)

static void mn_frame(mn *m) { m->c->frame_ms(m->c->ud, MN_FRAME_MS); }

static void mn_settle_frames(mn *m, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) mn_frame(m);
}

static void mn_send(mn *m, ccord_input_event ev) { m->c->input(m->c->ud, &ev); }

static void mn_move(mn *m, float x, float y)
{
  mn_send(m, (ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y});
}

// A left click at (x, y). It does not advance time: the next frame is the first one that shows its effect.
static void mn_click(mn *m, float x, float y)
{
  mn_move(m, x, y);
  mn_send(m, (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y});
  mn_send(m, (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .x = x, .y = y});
}

static bool mn_at(float v, float t) { return fabsf(v - t) < 0.01f; }

// Strictly between a and b, never equal to either end.
static bool mn_between(float v, float a, float b)
{
  float lo = a < b ? a : b, hi = a < b ? b : a;
  return v > lo + 0.01f && v < hi - 0.01f;
}

static bool rail_rect(const ccord_ui *ui, ui_rail_slot_kind kind, ccord_id id, ccord_rect *out)
{
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t n = ui_rail_build_slots(ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  for (size_t i = 0; i < n; i++) {
    if (slots[i].kind != kind) continue;
    if ((kind == UI_RAIL_GUILD ? slots[i].guild_id : slots[i].folder_id) != id) continue;
    *out = slots[i].rect;
    return true;
  }
  return false;
}

// Index of the rail slot the draw pass records for a guild, or -1 when it is not drawn.
static int rail_index(const ccord_ui *ui, ccord_id guild_id)
{
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t n = ui_rail_build_slots(ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  for (size_t i = 0; i < n; i++) {
    if (slots[i].kind == UI_RAIL_GUILD && slots[i].guild_id == guild_id) return (int)i;
  }
  return -1;
}

// The channel id a channel-list row stands for (categories included); 0 for rows that are not channels.
static ccord_id row_channel(const mn *m, const chan_row *row)
{
  if (row->is_relationship || row->is_voice_user || row->dm_nav != DM_NAV_NONE) return 0;
  const ccord_channel *ch = ccord_store_channel_at(m->c->store, ccord_store_selected_guild(m->c->store), row->index);
  return ch ? ch->id : 0;
}

// A plain channel row: not a category, relationship, voice user, thread or DM nav row.
static bool plain_row(const chan_row *row)
{
  return !row->is_category && !row->is_relationship && !row->is_voice_user && !row->is_thread &&
         row->dm_nav == DM_NAV_NONE;
}

static bool row_visible(const ccord_ui *ui, const chan_row *row)
{
  const ccord_layout_rect *cs = &ui->layout.channel_scroll;
  float y = cs->y + row->y - ui->chan_scroll;
  return y >= cs->y && y + row->h <= cs->y + cs->h;
}

// Screen y of the centre of a channel-list row (final layout; the list is not scrolled by this slice).
static float row_mid_y(const ccord_ui *ui, const chan_row *row)
{
  return ui->layout.channel_scroll.y + row->y - ui->chan_scroll + row->h * 0.5f;
}

static int crow_index(const mn *m, ccord_id ch_id)
{
  const ccord_ui *ui = m->c->ui;
  if (ch_id == 0) return -1;
  for (size_t i = 0; i < ui->ncrow; i++) {
    if (row_channel(m, &ui->crow[i]) == ch_id) return (int)i;
  }
  return -1;
}

// A visible, unselected text channel: inside `category`, or anywhere when `category` is 0.
static int text_row(const mn *m, ccord_id category)
{
  const ccord_ui *ui = m->c->ui;
  ccord_id g = ccord_store_selected_guild(m->c->store);
  ccord_id sel = ccord_store_selected_channel(m->c->store);
  bool in_block = true;
  for (size_t i = 0; i < ui->ncrow; i++) {
    const chan_row *row = &ui->crow[i];
    if (row->is_category) {
      in_block = category == 0 || row_channel(m, row) == category;
      continue;
    }
    if (!in_block || !plain_row(row) || !row_visible(ui, row)) continue;
    const ccord_channel *ch = ccord_store_channel_at(m->c->store, g, row->index);
    if (ch && ch->kind == CCORD_CHANNEL_TEXT && ch->id != sel) return (int)i;
  }
  return -1;
}

static float mp_read(const mn *m, mp p)
{
  const ccord_ui *ui = m->c->ui;
  int i = -1;
  switch (p.kind) {
    case MP_PILL:
      i = rail_index(ui, p.id);
      return i < 0 ? -1.0f : ui->dbg_rail_pill_h[i];
    case MP_RADIUS:
      i = rail_index(ui, p.id);
      return i < 0 ? -1.0f : ui->dbg_rail_radius[i];
    case MP_ALPHA:
      i = rail_index(ui, p.id);
      return i < 0 ? -1.0f : ui->dbg_rail_alpha[i];
    case MP_CROW:
      i = crow_index(m, p.id);
      return i < 0 ? -1.0f : ui->dbg_crow_y[i];
    case MP_SEL:
      return ui->dbg_chan_sel_fade;
  }
  return -1.0f;
}

// Mid-transition check. Frames run until the first value leaves its start; that first sample must be strictly between
// start and end (it neither sits at the start nor jumps to the end). Every value must then reach its end within
// MN_SETTLE_MS of the trigger.
static bool mn_transition(mn *m, const mt *t, size_t n)
{
  uint32_t elapsed = 0;
  for (;;) {
    bool moved = false;
    for (size_t i = 0; i < n; i++) moved = moved || !mn_at(mp_read(m, t[i].p), t[i].from);
    if (moved) break;
    if (elapsed >= MN_SETTLE_MS) return false;
    mn_frame(m);
    elapsed += MN_FRAME_MS;
  }
  for (size_t i = 0; i < n; i++) {
    if (!mn_between(mp_read(m, t[i].p), t[i].from, t[i].to)) return false;
  }
  for (;;) {
    bool done = true;
    for (size_t i = 0; i < n; i++) done = done && mn_at(mp_read(m, t[i].p), t[i].to);
    if (done) return true;
    if (elapsed >= MN_SETTLE_MS) return false;
    mn_frame(m);
    elapsed += MN_FRAME_MS;
  }
}

static bool mn_run(mn *m)
{
  const ccord_verify_ui_parity_ctx *c = m->c;
  ccord_ui *ui = c->ui;
  ccord_store *st = c->store;
  ccord_error err = {0};

  // ---- setup: let the mock session load its guilds, then pick a guild to select (no unread) and two others for a folder
  mn_settle_frames(m, 20);
  size_t ng = ccord_store_guild_count(st);
  MN(ng >= 3 && ng <= CCORD_MAX_GUILDS, "mock has three or more guilds for the rail checks");
  ccord_id sel_g = ccord_store_selected_guild(st);
  ccord_id pick = 0, fa = 0, fb = 0;
  for (size_t i = 0; i < ng && pick == 0; i++) {
    const ccord_guild *g = ccord_store_guild_at(st, i);
    uint32_t mentions = 0;
    if (!g || g->id == sel_g || ui_guild_unread(ui, g->id, &mentions) > 0 || mentions > 0) continue;
    pick = g->id;
  }
  MN(pick != 0, "a guild without unread messages to select");
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *g = ccord_store_guild_at(st, i);
    if (!g || g->id == pick) continue;
    if (fa == 0) fa = g->id;
    else if (fb == 0) fb = g->id;
  }
  MN(fa != 0 && fb != 0, "two guilds besides the picked one for the folder");
  ccord_guild_folder_item items[CCORD_MAX_GUILDS];
  for (size_t i = 0; i < ng; i++) {
    const ccord_guild *g = ccord_store_guild_at(st, i);
    items[i] = (ccord_guild_folder_item){.guild_id = g->id,
                                         .folder_id = (g->id == fa || g->id == fb) ? MN_FOLDER_ID : 0,
                                         .folder_color = 0x3366ffu};
    snprintf(items[i].folder_name, sizeof items[i].folder_name, "%s", "Motion");
  }
  MN(ccord_store_set_guild_layout(st, items, ng, &err) == CCORD_OK, "folder layout applied to the store");
  float nx = ui->layout.member_list.x + ui->layout.member_list.w * 0.5f;
  float ny = ui->layout.member_list.y + ui->layout.member_list.h * 0.5f;
  mn_move(m, nx, ny);
  mn_settle_frames(m, 20);

  // ---- rail: hover eases the pill and the icon radius; a click selects the guild and eases the pill to selected
  float hover_h = ui->theme->m.pill_height_hover, active_h = ui->theme->m.pill_height_active;
  float r_base = ui->theme->m.guild_icon_radius, r_act = ui->theme->m.guild_icon_radius_active;
  MN(fabsf(hover_h - active_h) > 1.0f && fabsf(r_base - r_act) > 1.0f, "theme: rest and active rail values differ");
  mp pill = {MP_PILL, pick}, rad = {MP_RADIUS, pick};
  MN(mn_at(mp_read(m, pill), 0.0f), "rail: the picked guild rests with no pill");
  MN(mn_at(mp_read(m, rad), r_base), "rail: the picked guild rests at the base icon radius");
  ccord_rect gr;
  MN(rail_rect(ui, UI_RAIL_GUILD, pick, &gr), "rail: the picked guild is drawn in the rail");
  float gx = gr.x + gr.w * 0.5f, gy = gr.y + gr.h * 0.5f;
  mn_move(m, gx, gy);
  mt hover[] = {{pill, 0.0f, hover_h}, {rad, r_base, r_act}};
  MN(mn_transition(m, hover, 2), "rail: hover eases pill height and icon radius between rest and hover/active");

  mn_click(m, gx, gy);
  MN(ccord_store_selected_guild(st) == pick, "rail: the click selected the guild");
  mt on_select[] = {{pill, hover_h, active_h}};
  MN(mn_transition(m, on_select, 1), "rail: selection eases pill height between hover and selected");

  // ---- folder expand: the members fade in
  ccord_rect fr;
  MN(rail_rect(ui, UI_RAIL_FOLDER_COLLAPSED, MN_FOLDER_ID, &fr), "folder: collapsed in the rail");
  mn_click(m, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mn_frame(m);   // the first frame that draws the expanded folder; its values are the transition's start
  mt expand[] = {{{MP_ALPHA, fa}, 0.0f, 1.0f}};
  MN(mn_transition(m, expand, 1), "folder: an expanded member fades in between 0 and 1");

  // ---- channel row selection eases its fade
  int xi = text_row(m, 0);
  MN(xi >= 0, "channel: a visible text channel that is not selected");
  const chan_row *xrow = &ui->crow[xi];
  ccord_id x_id = row_channel(m, xrow);
  mn_click(m, ui->layout.channel_scroll.x + 60.0f, row_mid_y(ui, xrow));
  mn_frame(m);   // the first frame that draws the new selection; the fade starts from its value there
  MN(ccord_store_selected_channel(st) == x_id, "channel: the click selected the row");
  mt sel[] = {{{MP_SEL, 0}, 0.0f, 1.0f}};
  MN(mn_transition(m, sel, 1), "channel: selection fade eases between 0 and 1");

  // ---- category collapse: the row below slides up; a click where a collapsed child was drawn selects nothing
  int cidx = -1, kidx = -1, jidx = -1;
  for (size_t i = 0; i < ui->ncrow && cidx < 0; i++) {
    const chan_row *row = &ui->crow[i];
    if (!row->is_category || row->is_relationship || row->dm_nav != DM_NAV_NONE) continue;
    size_t j = i + 1;
    while (j < ui->ncrow && !ui->crow[j].is_category) j++;
    if (j >= ui->ncrow) continue;   // the last category has no row below to slide
    for (size_t k = i + 1; k < j && cidx < 0; k++) {
      const ccord_channel *ch = ccord_store_channel_at(st, ccord_store_selected_guild(st), ui->crow[k].index);
      if (plain_row(&ui->crow[k]) && ch && ch->kind == CCORD_CHANNEL_TEXT &&
          ch->id != ccord_store_selected_channel(st)) {
        cidx = (int)i;
        kidx = (int)k;
        jidx = (int)j;
      }
    }
  }
  MN(cidx >= 0, "category: a category with an unselected text child and a row below it");
  ccord_id cat_id = row_channel(m, &ui->crow[cidx]);
  ccord_id child_id = row_channel(m, &ui->crow[kidx]);
  ccord_id below_id = row_channel(m, &ui->crow[jidx]);
  float below_old = ui->crow[jidx].y;
  float child_old_mid = row_mid_y(ui, &ui->crow[kidx]);
  mn_click(m, ui->layout.channel_scroll.x + 60.0f, row_mid_y(ui, &ui->crow[cidx]));
  mn_frame(m);
  int bi = crow_index(m, below_id);
  MN(bi >= 0, "category: the row below is still listed after collapsing");
  float below_new = ui->crow[bi].y;
  MN(fabsf(below_new - below_old) > 1.0f, "category: collapsing moves the row below");
  MN(crow_index(m, child_id) < 0, "category: the collapsed child is no longer listed");
  mt collapse[] = {{{MP_CROW, below_id}, below_old, below_new}};
  MN(mn_transition(m, collapse, 1), "category: the row below slides between its old and new y");
  mn_click(m, ui->layout.channel_scroll.x + 60.0f, child_old_mid);
  mn_frame(m);
  MN(ccord_store_selected_channel(st) != child_id, "category: a click where a collapsed row was drawn does not select it");

  // ---- reduced motion: every state is at its end value on the next frame
  ui->settings_reduced_motion = true;   // the setting, and the anim set it is copied into when settings apply
  ui->anim.reduced_motion = true;
  ccord_id other = 0;
  for (size_t i = 0; i < ng && other == 0; i++) {
    const ccord_guild *g = ccord_store_guild_at(st, i);
    uint32_t mentions = 0;
    if (g && g->id != pick && ui_guild_unread(ui, g->id, &mentions) == 0 && mentions == 0) other = g->id;
  }
  MN(other != 0, "reduced motion: a second guild to hover");
  ccord_rect orr;
  MN(rail_rect(ui, UI_RAIL_GUILD, other, &orr), "reduced motion: the second guild is drawn");
  mn_move(m, orr.x + orr.w * 0.5f, orr.y + orr.h * 0.5f);
  mn_frame(m);
  MN(mn_at(mp_read(m, (mp){MP_PILL, other}), hover_h), "reduced motion: rail pill is at hover on the next frame");
  MN(mn_at(mp_read(m, (mp){MP_RADIUS, other}), r_act), "reduced motion: rail icon radius is at active on the next frame");
  mn_move(m, nx, ny);
  mn_settle_frames(m, 2);

  ccord_rect fh;
  MN(rail_rect(ui, UI_RAIL_FOLDER_HEADER, MN_FOLDER_ID, &fh), "reduced motion: folder header is drawn");
  mn_click(m, fh.x + fh.w * 0.5f, fh.y + fh.h * 0.5f);
  mn_frame(m);
  MN(rail_rect(ui, UI_RAIL_FOLDER_COLLAPSED, MN_FOLDER_ID, &fr), "reduced motion: folder collapsed again");
  mn_click(m, fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f);
  mn_frame(m);
  MN(mn_at(mp_read(m, (mp){MP_ALPHA, fa}), 1.0f), "reduced motion: a folder member is fully shown on the next frame");

  int ci = crow_index(m, cat_id);
  MN(ci >= 0, "reduced motion: the collapsed category is listed");
  mn_click(m, ui->layout.channel_scroll.x + 60.0f, row_mid_y(ui, &ui->crow[ci]));
  mn_frame(m);
  bi = crow_index(m, below_id);
  MN(bi >= 0 && mn_at(ui->dbg_crow_y[bi], ui->crow[bi].y),
     "reduced motion: the row below is at its layout y on the next frame");

  int ti = text_row(m, cat_id);
  MN(ti >= 0, "reduced motion: a text channel inside the expanded category");
  ccord_id t_id = row_channel(m, &ui->crow[ti]);
  mn_click(m, ui->layout.channel_scroll.x + 60.0f, row_mid_y(ui, &ui->crow[ti]));
  mn_frame(m);
  MN(ccord_store_selected_channel(st) == t_id, "reduced motion: the click selected the channel");
  MN(mn_at(mp_read(m, (mp){MP_SEL, 0}), 1.0f), "reduced motion: selection fade is at 1 on the next frame");

  ui->settings_reduced_motion = false;
  ui->anim.reduced_motion = false;
  return true;
}

int ccord_verify_motion_nav(const ccord_verify_ui_parity_ctx *ctx, int *checks)
{
  mn m = {.c = ctx};
  if (!mn_run(&m)) {
    printf("motion nav detail: %s\n", m.fail[0] ? m.fail : "check failed");
    fflush(stdout);
    return 1;
  }
  *checks += m.checks;
  return 0;
}
