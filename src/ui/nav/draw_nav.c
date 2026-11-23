// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <math.h>
#include <stdio.h>

#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/platform/app_icon.h>

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

static const char *const k_app_logo_key = "ccord://app-logo";

static float maxf(float a, float b)
{
  return a > b ? a : b;
}

static bool ensure_app_logo_texture(ccord_renderer *r)
{
  if (!r) return false;
  if (ccord_render_has_texture(r, k_app_logo_key, nullptr, nullptr)) return true;
  enum { LOGO_SZ = 128 };
  const size_t nbytes = (size_t)LOGO_SZ * (size_t)LOGO_SZ * 4u;
  uint8_t *rgba = ccord_malloc(nbytes);
  if (!rgba) return false;
  bool ok = ccord_app_icon_rgba(LOGO_SZ, rgba, nbytes);
  if (ok) {
    ccord_error err = {0};
    ok = ccord_render_upload_rgba(r, k_app_logo_key, rgba, LOGO_SZ, LOGO_SZ, &err) == CCORD_OK;
  }
  ccord_free(rgba);
  return ok;
}

static ccord_color presence_color(const ccord_ui *ui, ccord_presence p)
{
  switch (p) {
    case CCORD_PRESENCE_ONLINE: return COL(status_online);
    case CCORD_PRESENCE_IDLE: return COL(status_idle);
    case CCORD_PRESENCE_DND: return COL(status_dnd);
    case CCORD_PRESENCE_OFFLINE: break;
  }
  return COL(status_offline);
}

static const char *presence_label(ccord_presence p)
{
  switch (p) {
    case CCORD_PRESENCE_ONLINE: return "Online";
    case CCORD_PRESENCE_IDLE: return "Idle";
    case CCORD_PRESENCE_DND: return "Do Not Disturb";
    case CCORD_PRESENCE_OFFLINE: break;
  }
  return "Offline";
}

// Centres text inside a box.
static void text_centered(ccord_renderer *r, const char *s, ccord_rect box, const ccord_text_style *st)
{
  size_t n = strlen(s);
  ccord_vec2 t = ccord_text_measure(r, s, n, st, 0.0f);
  ccord_text_draw(r, s, n, box.x + (box.w - t.x) * 0.5f, box.y + (box.h - t.y) * 0.5f, st, 0.0f);
}

// Fast integer Rec.709 perceptual luminance (0..255000), avoiding 6x powf() per fallback avatar.
static uint32_t fast_luma(ccord_color c)
{
  return 299u * (uint32_t)c.r + 587u * (uint32_t)c.g + 114u * (uint32_t)c.b;
}

// Light text on the avatar colour, unless the background is light: then the dark panel colour.
static ccord_color avatar_text_color(const ccord_ui *ui, ccord_color bg)
{
  uint32_t lb = fast_luma(bg);
  uint32_t lw = fast_luma(COL(text_on_brand));
  if (lw >= 128000u) return lb <= 150000u ? COL(text_on_brand) : COL(bg_tertiary);
  return lb >= 105000u ? COL(text_on_brand) : COL(bg_tertiary);
}

static float squircle_radius(float size)
{
  return fmaxf(4.0f, roundf(size * 0.26f));
}

void ui_draw_avatar(ccord_ui *ui, ccord_renderer *r, const ccord_user *u, float cx, float cy, float size, ccord_color ring, bool dim)
{
  bool show_deco = (size >= 24.0f && u && u->avatar_decoration_url && u->avatar_decoration_url[0] != '\0');
  float av_rad = show_deco ? (size * 0.5f) : squircle_radius(size);
  ccord_rect dst = {cx - size * 0.5f, cy - size * 0.5f, size, size};
  char url[256];
  if (u && ui_user_avatar_url(u, url, sizeof url) && ui_ensure_media(ui, r, url)) {
    ccord_draw_texture_rounded(r, url, dst, av_rad, dim ? 140 : 255);
    ui->dbg_avatar_textures_drawn++;
    if (show_deco) ui_draw_avatar_deco_url(ui, r, u->avatar_decoration_url, cx, cy, size, dim ? 140 : 255);
    if (ring.a != 0) {
      float dot = M(status_dot_size) * size / 32.0f;
      if (dot < 8.0f) dot = 8.0f;
      float dx = cx + size * 0.5f - dot * 0.18f, dy = cy + size * 0.5f - dot * 0.18f;
      ccord_draw_circle(r, dx, dy, dot * 0.5f + 2.2f, ring);
      ccord_draw_circle(r, dx, dy, dot * 0.5f, presence_color(ui, u->presence));
    }
    return;
  }
  ccord_color bg = ui_user_color(ui, u ? u->avatar_palette : 0);
  if (dim) bg = ui_mix(bg, ring.a ? ring : COL(bg_secondary), 0.55f);
  ccord_draw_rrect(r, dst, av_rad, bg);
  char init[8] = "?";
  if (u && u->name && u->name[0]) {
    size_t l = ccord_utf8_next(u->name, strlen(u->name), 0);
    if (l > 4) l = 4;
    memcpy(init, u->name, l);
    init[l] = '\0';
    if (l == 1 && init[0] >= 'a' && init[0] <= 'z') init[0] = (char)(init[0] - 'a' + 'A');
  }
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, size * 0.42f, avatar_text_color(ui, bg));
  text_centered(r, init, dst, &st);
  if (show_deco) ui_draw_avatar_deco_url(ui, r, u->avatar_decoration_url, cx, cy, size, dim ? 140 : 255);
  if (ring.a != 0 && u) {
    float dot = M(status_dot_size) * size / 32.0f;
    if (dot < 8.0f) dot = 8.0f;
    float dx = cx + size * 0.5f - dot * 0.18f, dy = cy + size * 0.5f - dot * 0.18f;
    ccord_draw_circle(r, dx, dy, dot * 0.5f + 2.2f, ring);
    ccord_draw_circle(r, dx, dy, dot * 0.5f, presence_color(ui, u->presence));
  }
}

// ---------------------------------------------------------------- group DM icon

// Custom group icon when set; otherwise a squircle tile with a two-person glyph.
void ui_draw_group_dm_icon(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch, float cx, float cy, float size)
{
  float av_rad = squircle_radius(size);
  ccord_rect dst = {cx - size * 0.5f, cy - size * 0.5f, size, size};
  if (ch && ch->icon_url && ch->icon_url[0] && ui_ensure_media(ui, r, ch->icon_url)) {
    ccord_draw_texture_rounded(r, ch->icon_url, dst, av_rad, 255);
    return;
  }
  ccord_color bg = ui_user_color(ui, (uint8_t)(ch ? (ch->id >> 22) % 6u : 0u));
  ccord_draw_rrect(r, dst, av_rad, bg);
  ccord_color fg = avatar_text_color(ui, bg);
  float u = size / 32.0f;
  // back person (smaller, offset right) then front person
  ccord_draw_circle(r, cx + 4.5f * u, cy - 4.0f * u, 3.6f * u, ui_mix(fg, bg, 0.35f));
  ccord_draw_rrect(r, (ccord_rect){cx + 0.5f * u, cy + 0.5f * u, 8.5f * u, 6.5f * u}, 3.2f * u, ui_mix(fg, bg, 0.35f));
  ccord_draw_circle(r, cx - 2.5f * u, cy - 2.5f * u, 4.4f * u, fg);
  ccord_draw_rrect(r, (ccord_rect){cx - 8.0f * u, cy + 3.0f * u, 11.0f * u, 7.5f * u}, 3.8f * u, fg);
}

// ---------------------------------------------------------------- unread / mention badges

bool ui_channel_muted(const ccord_ui *ui, const ccord_channel *ch)
{
  return ccord_store_channel_is_muted(ui->d.store, ch, ui->now_unix_ms);
}

typedef struct ui_unread_entry {
  ccord_id guild_id;
  uint32_t unread;
  uint32_t mentions;
} ui_unread_entry;

struct ui_unread_cache {
  uint64_t rev;
  ccord_id preview_guild;
  ccord_id preview_role;
  int64_t sec;
  size_t count;
  ui_unread_entry entries[CCORD_MAX_GUILDS + 1u];
};

void ui_nav_caches_free(ccord_ui *ui)
{
  if (!ui) return;
  ccord_free(ui->unread_cache);
  ui->unread_cache = nullptr;
}

// Muted channels (their own mute, their category's, or the server's) add no unread; a mention still counts, since
// Discord counts those regardless and an @mention is worth a badge even in a place you silenced.
uint32_t ui_guild_unread(const ccord_ui *ui, ccord_id guild_id, uint32_t *mentions)
{
  uint32_t unread = 0;
  *mentions = 0;
  struct ui_unread_cache *uc = ui->unread_cache;
  if (uc) {
    uint64_t rev = ui->d.store ? ccord_store_revision(ui->d.store) : 0;
    int64_t sec = ui->now_unix_ms / 1000LL;
    if (uc->rev != rev || uc->preview_guild != ui->role_preview_guild ||
        uc->preview_role != ui->role_preview_role_id || uc->sec != sec) {
      uc->rev = rev;
      uc->preview_guild = ui->role_preview_guild;
      uc->preview_role = ui->role_preview_role_id;
      uc->sec = sec;
      uc->count = 0;
    } else {
      for (size_t i = 0; i < uc->count; i++) {
        if (uc->entries[i].guild_id == guild_id) {
          *mentions = uc->entries[i].mentions;
          return uc->entries[i].unread;
        }
      }
    }
  }
  size_t n = ccord_store_channel_count(ui->d.store, guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *c = ccord_store_channel_at(ui->d.store, guild_id, i);
    if (!c || c->is_message_request || (c->unread_count == 0 && c->mention_count == 0)) continue;
    if (!ui_channel_visible(ui, guild_id, c)) continue;
    if (c->unread_count > 0 && !ui_channel_muted(ui, c)) unread += c->unread_count;
    *mentions += c->mention_count;
  }
  if (uc && uc->count < (sizeof uc->entries / sizeof uc->entries[0])) {
    uc->entries[uc->count++] = (ui_unread_entry){.guild_id = guild_id, .unread = unread, .mentions = *mentions};
  }
  return unread;
}

static void draw_unread_pill(ccord_renderer *r, float x, float cy, float h, ccord_color color)
{
  ccord_draw_rrect(r, (ccord_rect){x, cy - h * 0.5f, 3.0f, h}, 1.5f, color);
}

typedef struct mention_badge_layout {
  char label[16];
  size_t len;
  ccord_text_style st;
  ccord_vec2 text_sz;
  float w, h;
} mention_badge_layout;

static mention_badge_layout measure_mention_badge(ccord_ui *ui, ccord_renderer *r, uint32_t count)
{
  mention_badge_layout b = {0};
  if (count == 0) return b;
  if (count > 99) snprintf(b.label, sizeof b.label, "99+");
  else snprintf(b.label, sizeof b.label, "%u", count);
  b.len = strlen(b.label);
  b.st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
  b.text_sz = ccord_text_measure(r, b.label, b.len, &b.st, 0.0f);
  b.h = 16.0f;
  b.w = maxf(b.text_sz.x + 9.0f, 16.0f);
  return b;
}

static void draw_mention_badge_at(ccord_ui *ui, ccord_renderer *r, const mention_badge_layout *b, float x, float y)
{
  if (!b || b->len == 0) return;
  ccord_draw_rrect(r, (ccord_rect){x, y, b->w, b->h}, b->h * 0.5f, COL(danger));
  ccord_text_draw(r, b->label, b->len, x + (b->w - b->text_sz.x) * 0.5f, y + (b->h - b->text_sz.y) * 0.5f, &b->st, 0.0f);
}

static void draw_mention_badge(ccord_ui *ui, ccord_renderer *r, ccord_rect icon, uint32_t count, bool top)
{
  mention_badge_layout b = measure_mention_badge(ui, r, count);
  if (b.len == 0) return;
  float x = icon.x + icon.w - b.w + 3.0f;
  float y = top ? icon.y - 2.0f : icon.y + icon.h - b.h + 2.0f;
  draw_mention_badge_at(ui, r, &b, x, y);
}

static void draw_row_mention_badge(ccord_ui *ui, ccord_renderer *r, ccord_rect item, const mention_badge_layout *b)
{
  if (!b || b->len == 0) return;
  float x = item.x + item.w - M(channel_item_pad_x) - b->w;
  float y = item.y + (item.h - b->h) * 0.5f;
  draw_mention_badge_at(ui, r, b, x, y);
}

// ---------------------------------------------------------------- server list

// Fallback Home glyph only if embedded icon texture upload ever fails.
static void draw_dm_home_fallback(ccord_renderer *r, ccord_rect ic, ccord_color fg, ccord_color bg)
{
  float cx = ic.x + ic.w * 0.5f, cy = ic.y + ic.h * 0.5f;
  ccord_draw_rrect(r, (ccord_rect){cx - 10.0f, cy - 8.0f, 20.0f, 16.0f}, 5.0f, fg);
  ccord_draw_rrect(r, (ccord_rect){cx - 6.0f, cy - 4.0f, 12.0f, 8.0f}, 2.5f, bg);
}

// Identity of a rail slot across frames, for its row motion.
static uint64_t rail_slot_key(const ui_rail_slot *sl)
{
  switch (sl->kind) {
    case UI_RAIL_DM: return (uint64_t)1 << 56;
    case UI_RAIL_GUILD: return ((uint64_t)2 << 56) ^ sl->guild_id;
    case UI_RAIL_FOLDER_COLLAPSED: return ((uint64_t)3 << 56) ^ sl->folder_id;
    case UI_RAIL_FOLDER_HEADER: return ((uint64_t)4 << 56) ^ sl->folder_id;
    case UI_RAIL_ADD_SERVER: return (uint64_t)5 << 56;
    case UI_RAIL_DISCOVERY: return (uint64_t)6 << 56;
  }
  return 0;
}

void ui_draw_server_list(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_layout_rect *l = &ui->layout.server_list;
  ccord_rect rail_bg = ui_rect(*l);
  if (ui->mobile_layout) {
    float lt = ui->left_drawer_t < 0.0f ? 0.0f : (ui->left_drawer_t > 1.0f ? 1.0f : ui->left_drawer_t);
    float left_ext = ui->safe_insets.left * lt;
    rail_bg = (ccord_rect){l->x - left_ext, 0.0f, l->w + left_ext, ui->win_h};
  }
  ccord_draw_rect(r, rail_bg, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){l->x + l->w - 1.0f, rail_bg.y, 1.0f, rail_bg.h}, COL(border_subtle));
  ccord_push_clip(r, ui_rect(*l));

  if (!ui->unread_cache) {
    ui->unread_cache = ccord_calloc(1, sizeof *ui->unread_cache);
  }
  // The offset stays inside [0, total - view] on every draw (the content can shrink while the rail is scrolled).
  float rail_total = 0.0f;
  size_t nslots = 0;
  const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, &rail_total);
  float rail_max = rail_total - l->h > 0.0f ? rail_total - l->h : 0.0f;
  float prev_scroll = ui->server_scroll;
  if (ui->server_scroll > rail_max) ui->server_scroll = rail_max;
  if (ui->server_scroll < 0.0f) ui->server_scroll = 0.0f;
  if (ui->server_scroll != prev_scroll) {
    slots = ui_rail_slots_cached(ui, &nslots, nullptr);
  }
  if (!slots) nslots = 0;
  ccord_id selected = ccord_store_selected_guild(ui->d.store);

  // Row motion: a rail row pushed by a folder that opened or closed slides from where it was drawn; the members of a
  // folder that just opened fade in from its header. The hit-tests still use slots[].rect (the final layout).
  ccord_rect rr[UI_RAIL_SLOTS_MAX];
  float ralpha[UI_RAIL_SLOTS_MAX];
  ui_track_begin(ui, &ui->trk_rail, "trkr", 1u);
  float hdr_y = 0.0f;
  ccord_id hdr_folder = 0;
  for (size_t si = 0; si < nslots; si++) {
    const ui_rail_slot *sl = &slots[si];
    if (sl->kind == UI_RAIL_FOLDER_HEADER) {
      hdr_y = sl->rect.y;
      hdr_folder = sl->folder_id;
    }
    bool member = sl->kind == UI_RAIL_GUILD && sl->folder_id != 0 && sl->folder_id == hdr_folder;
    rr[si] = sl->rect;
    // Rows are tracked in content space (rect.y has the scroll offset in it), like the channel list: a scroll step is
    // not a row move, so it must not start a slide that holds the rows still until the wheel stops.
    rr[si].y = ui_track_row(ui, &ui->trk_rail, rail_slot_key(sl), sl->rect.y + ui->server_scroll, hdr_y + ui->server_scroll,
                            member, &ralpha[si]) - ui->server_scroll;
    ui->dbg_rail_alpha[si] = ralpha[si];
  }
  ui_track_end(&ui->trk_rail);

  // Expanded folder background pills behind header + member guilds. The pill grows to its full height as the folder opens.
  for (size_t si = 0; si < nslots; si++) {
    if (slots[si].kind != UI_RAIL_FOLDER_HEADER) continue;
    size_t last = si;
    while (last + 1u < nslots && slots[last + 1u].kind == UI_RAIL_GUILD &&
           slots[last + 1u].folder_id == slots[si].folder_id) {
      last++;
    }
    ccord_rect top_r = rr[si];
    ccord_rect bot_r = rr[last];
    float open_t = ui_enter(ui, "rfopen", slots[si].folder_id, 150);
    ccord_rect pill_bg = {top_r.x, top_r.y, top_r.w, ((bot_r.y + bot_r.h) - top_r.y) * open_t};
    if (pill_bg.y + pill_bg.h >= l->y && pill_bg.y <= l->y + l->h) {
      ccord_color fc = slots[si].folder_color != 0 ? ui_rgb_color(slots[si].folder_color) : COL(bg_secondary);
      ccord_color tint = slots[si].folder_color != 0 ? ui_alpha(fc, 0.16f) : COL(bg_secondary);
      ccord_draw_rrect(r, pill_bg, 10.0f, tint);
    }
  }

  for (size_t si = 0; si < nslots; si++) {
    const ui_rail_slot *sl = &slots[si];
    ccord_rect ic = rr[si];
    if (ic.y + ic.h < l->y && sl->kind != UI_RAIL_DM) continue;
    if (ic.y > l->y + l->h) continue;
    bool hovered = ((int)si == ui->hover_rail_slot) && !ui->menu.open && !ui->settings_open;

    if (sl->kind == UI_RAIL_DM || sl->kind == UI_RAIL_GUILD) {
      bool is_dm = (sl->kind == UI_RAIL_DM);
      const ccord_guild *g = is_dm ? nullptr : ccord_store_guild_at(ui->d.store, sl->guild_index);
      if (!is_dm && !g) continue;
      bool faded = ralpha[si] < 1.0f;   // a folder member that is fading in
      if (faded) ccord_push_alpha(r, ralpha[si]);
      const char *name = is_dm ? CCORD_DM_GUILD_NAME : g->name;
      ccord_id rail_id = is_dm ? CCORD_DM_GUILD_ID : g->id;
      bool is_sel = !ui->discovery_open && (is_dm ? selected == CCORD_DM_GUILD_ID : g->id == selected);
      // Icon: corner radius and matte surface elevation ease together toward the selected / hovered look.
      float a = ui_fade_ms(ui, "rgi", rail_id, hovered || is_sel, (uint32_t)M(hover_anim_ms));
      float radius = M(guild_icon_radius) + (M(guild_icon_radius_active) - M(guild_icon_radius)) * a;
      ui->dbg_rail_radius[si] = radius;
      ccord_color icon_bg = ui_mix(COL(bg_guild_idle), COL(bg_item_selected), a);
      if (a > 0.02f) {
        ccord_draw_rrect(r, (ccord_rect){ic.x - 1.0f, ic.y - 1.0f, ic.w + 2.0f, ic.h + 2.0f}, radius + 1.0f,
                         ui_alpha(COL(divider), 0.55f + 0.45f * a));
      }
      ccord_draw_rrect(r, ic, radius, icon_bg);
      char icon_url[256];
      if (!is_dm && g && ui_guild_icon_url(g, icon_url, sizeof icon_url) && ui_ensure_media(ui, r, icon_url)) {
        ccord_draw_texture_rounded_anim(r, icon_url, ic, radius, 255, hovered);
        ui->dbg_guild_icon_textures_drawn++;
      } else if (is_dm) {
        if (ensure_app_logo_texture(r)) {
          float inset = 2.0f;
          ccord_rect logo_rc = {ic.x + inset, ic.y + inset, ic.w - 2.0f * inset, ic.h - 2.0f * inset};
          ccord_draw_texture(r, k_app_logo_key, logo_rc, 255);
        } else {
          draw_dm_home_fallback(r, ic, ui_mix(COL(text_normal), COL(text_header), a), icon_bg);
        }
      } else {
        char init[8];
        ui_initials(name, init);
        ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) + 1.0f,
                                       ui_mix(COL(text_secondary), COL(text_header), a));
        text_centered(r, init, ic, &st);
      }
      uint32_t mentions = 0;
      uint32_t unread = ui_guild_unread(ui, rail_id, &mentions);
      // Pill height eases between selected, hovered, unread (a stub of 8 px) and hidden.
      float ptarget = is_sel ? M(pill_height_active) : (hovered ? M(pill_height_hover) : 0.0f);
      if (unread > 0 && !is_sel && ptarget < 8.0f) ptarget = 8.0f;
      float ph = ui_ease_to(ui, "rpill", rail_id, ptarget, UI_FADE_MS);
      ui->dbg_rail_pill_h[si] = ph;
      if (mentions > 0) draw_mention_badge(ui, r, ic, mentions, false);
      if (!is_dm) {
        (void)ui_decor_draw_inline_badges(ui, r, CCORD_DECOR_GUILD_BADGE, rail_id, ic.x, ic.y + ic.h - 14.0f, 14.0f, COL(bg_tertiary));
      }
      if (ph > 0.5f) {
        float pw = M(pill_width);
        ccord_draw_rrect(r, (ccord_rect){l->x - pw, ic.y + (ic.h - ph) * 0.5f, pw * 2.0f, ph}, pw, COL(pill));
      }
      if (is_dm) {
        float sep_y = ic.y + ic.h + M(guild_icon_gap);
        ccord_draw_rect(r, (ccord_rect){l->x + (l->w - 28.0f) * 0.5f, sep_y, 28.0f, 1.0f}, COL(divider));
      }
      if (faded) ccord_pop_alpha(r);
    } else if (sl->kind == UI_RAIL_FOLDER_COLLAPSED) {
      uint32_t mentions = 0, unread = 0;
      bool any_sel = false;
      for (size_t k = 0; k < sl->folder_len; k++) {
        const ccord_guild *fg = ccord_store_guild_at(ui->d.store, sl->folder_start + k);
        if (!fg) continue;
        uint32_t gm = 0;
        unread += ui_guild_unread(ui, fg->id, &gm);
        mentions += gm;
        if (!ui->discovery_open && fg->id == selected) any_sel = true;
      }
      float hv = ui_fade(ui, "rfld", sl->folder_id, hovered || any_sel);
      ccord_color fc = sl->folder_color != 0 ? ui_rgb_color(sl->folder_color) : COL(brand);
      ccord_color base_bg = ui_mix(COL(bg_guild_idle), fc, 0.22f);
      ccord_color hi_bg = ui_mix(COL(bg_guild_idle), fc, 0.38f);
      ccord_draw_rrect(r, ic, 10.0f, ui_mix(base_bg, hi_bg, hv));
      float pad = 5.0f, gap = 3.0f;
      float cell = (ic.w - 2.0f * pad - gap) * 0.5f;
      size_t tiles = sl->folder_len < 4u ? sl->folder_len : 4u;
      for (size_t k = 0; k < tiles; k++) {
        const ccord_guild *fg = ccord_store_guild_at(ui->d.store, sl->folder_start + k);
        if (!fg) continue;
        ccord_rect mc = {ic.x + pad + (float)(k % 2u) * (cell + gap),
                         ic.y + pad + (float)(k / 2u) * (cell + gap), cell, cell};
        ccord_draw_rrect(r, mc, 4.0f, COL(bg_tertiary));
        char icon_url[256];
        if (ui_guild_icon_url(fg, icon_url, sizeof icon_url) && ui_ensure_media(ui, r, icon_url)) {
          ccord_draw_texture_rounded(r, icon_url, mc, 4.0f, 255);
        } else {
          char init[8];
          ui_initials(fg->name, init);
          if (init[1] && init[2]) init[2] = '\0';
          ccord_text_style mst = ui_style(ui, CCORD_FONT_BOLD, 8.0f, COL(text_header));
          text_centered(r, init, mc, &mst);
        }
      }
      if (mentions > 0) draw_mention_badge(ui, r, ic, mentions, false);
      float ph = ui_ease_to(ui, "rfpill", sl->folder_id,
                            any_sel ? M(pill_height_active) : (hovered ? M(pill_height_hover) : (unread > 0 ? 8.0f : 0.0f)),
                            UI_FADE_MS);
      ui->dbg_rail_pill_h[si] = ph;
      if (ph > 0.5f) {
        float pw = M(pill_width);
        ccord_draw_rrect(r, (ccord_rect){l->x - pw, ic.y + (ic.h - ph) * 0.5f, pw * 2.0f, ph}, pw, COL(pill));
      }
    } else if (sl->kind == UI_RAIL_FOLDER_HEADER) {
      float hv = ui_fade(ui, "rfhdr", sl->folder_id, hovered);
      ccord_color fc = sl->folder_color != 0 ? ui_rgb_color(sl->folder_color) : COL(brand);
      ccord_draw_rrect(r, ic, 10.0f, ui_alpha(fc, 0.18f + 0.14f * hv));
      ui_icon_folder(r, ic.x + ic.w * 0.5f, ic.y + ic.h * 0.5f, 20.0f, fc);
    } else if (sl->kind == UI_RAIL_ADD_SERVER) {
      float sep_y = ic.y - M(guild_icon_gap) - 1.0f;
      ccord_draw_rect(r, (ccord_rect){l->x + (l->w - 28.0f) * 0.5f, sep_y, 28.0f, 1.0f}, COL(divider));
      bool active = (ui->modal_kind == UI_MODAL_ADD_SERVER);
      float hv = ui_fade(ui, "radd", 1u, hovered || active);
      float radius = M(guild_icon_radius) + (M(guild_icon_radius_active) - M(guild_icon_radius)) * hv;
      ccord_color bg = ui_mix(COL(bg_guild_idle), COL(bg_item_selected), hv);
      ccord_color fg = ui_mix(COL(text_muted), COL(text_header), hv);
      ccord_draw_rrect(r, ic, radius, bg);
      ui_icon_plus(r, ic.x + ic.w * 0.5f, ic.y + ic.h * 0.5f, 18.0f, fg);
    } else if (sl->kind == UI_RAIL_DISCOVERY) {
      bool active = ui->discovery_open;
      float hv = ui_fade(ui, "rdisc", 1u, hovered || active);
      float radius = M(guild_icon_radius) + (M(guild_icon_radius_active) - M(guild_icon_radius)) * hv;
      ccord_color bg = ui_mix(COL(bg_guild_idle), COL(bg_item_selected), hv);
      ccord_color fg = ui_mix(COL(text_muted), COL(text_header), hv);
      ccord_draw_rrect(r, ic, radius, bg);
      ui_icon_compass(r, ic.x + ic.w * 0.5f, ic.y + ic.h * 0.5f, 21.0f, fg, bg);
      float ph = ui_ease_to(ui, "rdpill", 1u, active ? M(pill_height_active) : (hovered ? M(pill_height_hover) : 0.0f),
                            UI_FADE_MS);
      ui->dbg_rail_pill_h[si] = ph;
      if (ph > 0.5f) {
        float pw = M(pill_width);
        ccord_draw_rrect(r, (ccord_rect){l->x - pw, ic.y + (ic.h - ph) * 0.5f, pw * 2.0f, ph}, pw, COL(pill));
      }
    }
  }

  if (ui->drag == DRAG_SERVER && ui->drag_active && ui->drag_drop_slot >= 0 && (size_t)ui->drag_drop_slot < nslots) {
    ccord_rect tr = slots[ui->drag_drop_slot].rect;
    if (ui->drag_drop_zone == UI_DROP_INTO_FOLDER) {
      ccord_draw_rrect(r, (ccord_rect){tr.x - 2.0f, tr.y - 2.0f, tr.w + 4.0f, tr.h + 4.0f}, 12.0f,
                       ui_alpha(COL(brand), 0.55f));
    } else if (ui->drag_drop_zone == UI_DROP_BEFORE || ui->drag_drop_zone == UI_DROP_AFTER) {
      ccord_draw_rrect(r, (ccord_rect){l->x + 10.0f, ui->drag_line_y - 1.5f, l->w - 20.0f, 3.0f}, 1.5f, COL(brand));
    }
  }
  {
    float fv_y = l->y + 12.0f;
    if (nslots > 0) fv_y = slots[nslots - 1u].rect.y + slots[nslots - 1u].rect.h + M(guild_icon_gap);
    ui_plugin_full_views_draw_rail(ui, r, l->x, l->w, &fv_y);
  }
  ccord_pop_clip(r);
}

#define UI_RAIL_VOICE_MAX_AVATARS 6u

static size_t collect_guild_voice_users(const ccord_ui *ui, ccord_id guild_id, ccord_id *out_users, size_t cap)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return 0u;
  size_t nvoices = 0u;
  const ccord_voice_state *voices = ccord_store_voice_states(ui->d.store, &nvoices);
  if (!voices || nvoices == 0u) return 0u;
  size_t total = 0u;
  ccord_id last_ch_id = CCORD_ID_NONE;
  bool last_ch_vis = false;
  for (size_t i = 0; i < nvoices; i++) {
    const ccord_voice_state *vs = &voices[i];
    if (vs->guild_id != guild_id || vs->channel_id == CCORD_ID_NONE || vs->user_id == CCORD_ID_NONE) continue;
    bool vis = false;
    if (vs->channel_id == last_ch_id) {
      vis = last_ch_vis;
    } else {
      const ccord_channel *ch = ccord_store_channel(ui->d.store, vs->channel_id);
      vis = ch && (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) &&
            ui_channel_visible(ui, guild_id, ch);
      last_ch_id = vs->channel_id;
      last_ch_vis = vis;
    }
    if (!vis) continue;
    if (total < cap) out_users[total] = vs->user_id;
    total++;
  }
  return total;
}

static void draw_guild_voice_avatar(ccord_ui *ui, ccord_renderer *r, ccord_id guild_id, ccord_id user_id,
                                    float cx, float cy, float size)
{
  const ccord_user *u = ccord_store_user(ui->d.store, user_id);
  const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, user_id);
  ccord_user resolved = {0};
  if (u) resolved = *u;
  resolved.id = user_id;
  if (prof && prof->guild_id == guild_id && prof->guild_avatar_url && prof->guild_avatar_url[0]) {
    resolved.avatar_url = prof->guild_avatar_url;
  } else if ((!resolved.avatar_url || !resolved.avatar_url[0]) && prof && prof->avatar_url && prof->avatar_url[0]) {
    resolved.avatar_url = prof->avatar_url;
  }
  float rad = size * 0.5f;
  ccord_rect dst = {cx - rad, cy - rad, size, size};
  char url[256];
  if (ui_user_avatar_url(&resolved, url, sizeof url) && ui_ensure_media(ui, r, url)) {
    ccord_draw_texture_rounded(r, url, dst, rad, 255);
    ui->dbg_avatar_textures_drawn++;
    return;
  }
  ccord_color bg = ui_user_color(ui, u ? u->avatar_palette : (uint8_t)(user_id % 6u));
  ccord_draw_circle(r, cx, cy, rad, bg);
  char init[8] = "?";
  if (u && u->name && u->name[0]) {
    size_t l = ccord_utf8_next(u->name, strlen(u->name), 0);
    if (l > 4) l = 4;
    memcpy(init, u->name, l);
    init[l] = '\0';
    if (l == 1 && init[0] >= 'a' && init[0] <= 'z') init[0] = (char)(init[0] - 'a' + 'A');
  }
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, size * 0.44f, avatar_text_color(ui, bg));
  text_centered(r, init, dst, &st);
}

static void draw_guild_voice_tooltip(ccord_ui *ui, ccord_renderer *r, ccord_rect anchor, const ccord_guild *g,
                                     uint64_t tip_id)
{
  const char *name = (g && g->name) ? g->name : nullptr;
  if (!name) return;
  ccord_id voice_users[UI_RAIL_VOICE_MAX_AVATARS];
  size_t voice_total = collect_guild_voice_users(ui, g->id, voice_users, UI_RAIL_VOICE_MAX_AVATARS);
  if (voice_total == 0u) {
    uis_tooltip(ui, r, anchor, name, UIS_TIP_RIGHT, tip_id);
    return;
  }
  size_t shown = voice_total <= UI_RAIL_VOICE_MAX_AVATARS ? voice_total : (UI_RAIL_VOICE_MAX_AVATARS - 1u);
  size_t overflow = voice_total > UI_RAIL_VOICE_MAX_AVATARS ? (voice_total - shown) : 0u;
  ui->dbg_rail_tip_voice_shown = shown;
  ui->dbg_rail_tip_voice_total = voice_total;

  uis_tokens tok = uis_tokens_of(ui);
  ccord_text_style title_st = ui_style(ui, CCORD_FONT_BOLD, tok.fs_control, COL(text_header));
  ccord_vec2 title_sz = ccord_text_measure(r, name, strlen(name), &title_st, 0.0f);

  // Snap avatar diameter to the spacing grid (24 px on default themes) so the tooltip height stays grid-aligned.
  float av_sz = roundf(fminf(fmaxf(M(avatar_size_small), 20.0f), 28.0f) / UIS_SP_1) * UIS_SP_1;
  float step = roundf(av_sz * 0.84f);
  const float halo_pad = 1.5f;
  const float icon_sz = 16.0f;
  const float icon_gap = UIS_SP_2;
  const float row_gap = UIS_SP_2;

  char more_lbl[24] = "";
  ccord_text_style more_st = ui_style(ui, CCORD_FONT_BOLD, tok.fs_caption, COL(text_secondary));
  float more_w = 0.0f;
  if (overflow > 0u) {
    snprintf(more_lbl, sizeof more_lbl, "+%zu", overflow);
    ccord_vec2 mt = ccord_text_measure(r, more_lbl, strlen(more_lbl), &more_st, 0.0f);
    more_w = maxf(av_sz, mt.x + 2.0f * UIS_SP_1 + 2.0f);
  }
  float avatars_w = overflow > 0u ? ((float)shown * step + more_w) : ((float)(shown - 1u) * step + av_sz);
  float voice_w = icon_sz + icon_gap + avatars_w;
  float content_w = maxf(title_sz.x, voice_w);
  float max_w = ui->win_w - 16.0f - 2.0f * UIS_SP_3;
  if (max_w > voice_w && content_w > max_w) content_w = max_w;
  float content_h = title_sz.y + row_gap + av_sz;

  ccord_rect tip = uis_tooltip_begin(ui, r, anchor, content_w, content_h, UIS_TIP_RIGHT, tip_id);
  float ix = tip.x + UIS_SP_3;
  float iy = tip.y + UIS_SP_2;
  ccord_text_draw_ellipsis(r, name, strlen(name), ix, iy, tip.w - 2.0f * UIS_SP_3, &title_st);

  float row_cy = iy + title_sz.y + row_gap + av_sz * 0.5f;
  ui_icon_speaker(r, ix + icon_sz * 0.5f, row_cy, icon_sz, COL(text_secondary));
  float ax0 = ix + icon_sz + icon_gap;
  for (size_t i = 0; i < shown; i++) {
    float cx = ax0 + av_sz * 0.5f + (float)i * step;
    if (i > 0u) ccord_draw_circle(r, cx, row_cy, av_sz * 0.5f + halo_pad, COL(bg_floating));
    draw_guild_voice_avatar(ui, r, g->id, voice_users[i], cx, row_cy, av_sz);
  }
  if (overflow > 0u) {
    ccord_rect pill = {ax0 + (float)shown * step, row_cy - av_sz * 0.5f, more_w, av_sz};
    ccord_rect halo = {pill.x - halo_pad, pill.y - halo_pad, pill.w + 2.0f * halo_pad, pill.h + 2.0f * halo_pad};
    ccord_draw_rrect(r, halo, halo.h * 0.5f, COL(bg_floating));
    ccord_draw_rrect(r, pill, pill.h * 0.5f, COL(bg_tertiary));
    text_centered(r, more_lbl, pill, &more_st);
  }
  uis_tooltip_end(r);
}

// Tooltip for the hovered rail item (drawn after the left-column panels so it never clips behind the user panel).
void ui_draw_server_rail_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  if (ui->touch_mode || ui->hover_rail_slot < 0 || ui->menu.open || ui->settings_open ||
      (ui->drag == DRAG_SERVER && ui->drag_active)) {
    return;
  }
  size_t nslots = 0;
  const ui_rail_slot *slots = ui_rail_slots_cached(ui, &nslots, nullptr);
  if (!slots || (size_t)ui->hover_rail_slot >= nslots) return;
  const ccord_layout_rect *l = &ui->layout.server_list;
  const ui_rail_slot *sl = &slots[ui->hover_rail_slot];
  ccord_rect ic = sl->rect;
  // Anchor: the rail column at the icon's height, so the tip sits 6 px right of the rail.
  ccord_rect anchor = {l->x, ic.y, l->w, ic.h};
  uint64_t tip_id = (uint64_t)ui->hover_rail_slot;
  if (sl->kind == UI_RAIL_GUILD) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, sl->guild_index);
    draw_guild_voice_tooltip(ui, r, anchor, g, tip_id);
    return;
  }
  const char *name = nullptr;
  switch (sl->kind) {
    case UI_RAIL_DM: name = CCORD_DM_GUILD_NAME; break;
    case UI_RAIL_FOLDER_COLLAPSED:
    case UI_RAIL_FOLDER_HEADER:
      name = sl->folder_name[0] ? sl->folder_name : "Server Folder";
      break;
    case UI_RAIL_ADD_SERVER: name = "Add a Server"; break;
    case UI_RAIL_DISCOVERY: name = "Explore Discoverable Servers"; break;
    case UI_RAIL_GUILD: break;
  }
  if (name) {
    uis_tooltip(ui, r, anchor, name, UIS_TIP_RIGHT, tip_id);
  }
}

// ---------------------------------------------------------------- channel list

// Calm sentence-case category heading: converts shouty ALL-CAPS defaults ("TEXT CHANNELS" -> "Text channels")
// while preserving intentional mixed-case category names ("C-Cord Core", "Dev & Ops").
static void format_category_label(const char *s, char *out, size_t cap)
{
  if (!s || cap == 0) return;
  bool has_upper = false, has_lower = false;
  for (size_t i = 0; s[i]; i++) {
    if (s[i] >= 'A' && s[i] <= 'Z') has_upper = true;
    else if (s[i] >= 'a' && s[i] <= 'z') has_lower = true;
  }
  size_t i = 0;
  bool first_alpha = true;
  for (; s[i] && i + 1 < cap; i++) {
    char c = s[i];
    if (has_upper && !has_lower && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
      if (first_alpha) {
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        first_alpha = false;
      } else {
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
      }
    } else {
      out[i] = c;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) first_alpha = false;
    }
  }
  out[i] = '\0';
}

// Identity of a channel-list row across frames, for its row motion.
static uint64_t chan_row_key(const ccord_ui *ui, ccord_id g, const chan_row *row)
{
  if (row->dm_nav != DM_NAV_NONE) return ((uint64_t)1 << 56) ^ (uint64_t)row->dm_nav;
  if (row->is_relationship) return row->is_category ? (uint64_t)2 << 56 : ((uint64_t)3 << 56) ^ row->user_id;
  const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, row->index);
  if (row->is_voice_user) return ((uint64_t)4 << 56) ^ row->user_id ^ ((ch ? ch->id : 0) * 0x9E3779B1ull);
  if (!ch) return ((uint64_t)5 << 56) ^ (uint64_t)row->index;
  return ((uint64_t)(row->is_category ? 6 : 7) << 56) ^ ch->id;
}

void ui_draw_channel_list(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_layout *lay = &ui->layout;
  ccord_rect cbg = ui_rect(lay->channel_list);
  if (ui->mobile_layout) {
    cbg.y = 0.0f;
    cbg.h = ui->win_h;
  }
  ccord_draw_rect(r, cbg, COL(bg_secondary));
  ui_theme_bg_draw(ui, r, "sidebar", cbg);
  ccord_draw_rect(r, (ccord_rect){lay->channel_list.x + lay->channel_list.w - 1.0f, cbg.y,
                                  1.0f, cbg.h}, COL(border_subtle));
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  const ccord_guild *gd = ccord_store_guild(ui->d.store, g);

  // guild header
  ccord_rect gh = ui_rect(lay->guild_header);
  if (g == CCORD_DM_GUILD_ID) {
    ccord_rect sb = {gh.x + 10.0f, gh.y + (gh.h - 28.0f) * 0.5f, gh.w - 20.0f, 28.0f};
    ccord_draw_rrect(r, (ccord_rect){sb.x - 1.0f, sb.y - 1.0f, sb.w + 2.0f, sb.h + 2.0f}, 7.0f, COL(border_subtle));
    ccord_draw_rrect(r, sb, 6.0f, COL(bg_tertiary));
    ui_icon_search(r, sb.x + 13.0f, sb.y + sb.h * 0.5f, 13.0f, COL(text_muted));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    const char *placeholder = "Find or start a chat";
    float sty = sb.y + (sb.h - ui_line_h(ui, ss.size)) * 0.5f;
    ccord_text_draw_ellipsis(r, placeholder, strlen(placeholder), sb.x + 25.0f, sty, sb.w - 33.0f, &ss);
  } else {
    float gh_h = ui_fade(ui, "ghdr", 0, ui->hover_guild_header && !ui->menu.open && !ui->settings_open);
    if (gh_h > 0.004f) ccord_draw_rect(r, (ccord_rect){gh.x, gh.y, gh.w - 1.0f, gh.h}, ui_alpha(COL(bg_item_hover), gh_h));
    ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base) - 0.5f, COL(text_header));
    const char *header_name = gd ? gd->name : nullptr;
    if (header_name) {
      float ty = gh.y + (gh.h - ui_line_h(ui, hs.size)) * 0.5f;
      ccord_text_draw_ellipsis(r, header_name, strlen(header_name), gh.x + 14.0f, ty, gh.w - 14.0f - 36.0f, &hs);
    }
    ui_icon_chevron(r, gh.x + gh.w - 20.0f, gh.y + gh.h * 0.5f, 14.0f, ICON_DOWN, COL(text_muted));
  }
  ccord_draw_rect(r, (ccord_rect){gh.x, gh.y + gh.h - 1.0f, gh.w, 1.0f}, COL(border_subtle));

  // channel rows
  const ccord_layout_rect *cs = &lay->channel_scroll;
  ccord_push_clip(r, (ccord_rect){cs->x, cs->y, cs->w - 1.0f, cs->h});
  ui->dm_header_plus_rect = (ccord_rect){0, 0, 0, 0};
  ui->dm_requests_nav_rect = (ccord_rect){0, 0, 0, 0};
  ui->dm_requests_badge_count = 0u;
  ui->ndm_close_rects = 0;
  ui->nchan_gear_rects = 0;
  if (g == CCORD_DM_GUILD_ID) {
    size_t dmn = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
    for (size_t di = 0; di < dmn; di++) {
      const ccord_channel *mc = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, di);
      if (mc && mc->is_message_request && !mc->is_spam) ui->dm_requests_badge_count++;
    }
  }
  ccord_id sel_ch = ccord_store_selected_channel(ui->d.store);
  float max_scroll = ui->chan_content_h - cs->h;
  if (max_scroll < 0.0f) max_scroll = 0.0f;
  if (ui->chan_scroll > max_scroll) ui->chan_scroll = max_scroll;
  // Row motion: rows that moved slide from where they were drawn; rows that appear under a category fade in from it.
  // Hit-tests (ui.c) keep the final layout, so a drawn row can sit a few pixels off its hit rect while it slides.
  ui_track_begin(ui, &ui->trk_chan, "trkc", g);
  float anchor_y = 0.0f;
  bool has_anchor = false;
  for (size_t i = 0; i < ui->ncrow; i++) {
    const chan_row *row = &ui->crow[i];
    float alpha = 1.0f;
    float row_cy = ui_track_row(ui, &ui->trk_chan, chan_row_key(ui, g, row), row->y, anchor_y, has_anchor, &alpha);
    if (row->is_category) {
      anchor_y = row->y;
      has_anchor = true;
    }
    if (i < UI_TRACK_MAX) ui->dbg_crow_y[i] = row_cy;
    float y = cs->y + row_cy - ui->chan_scroll;
    if (y + row->h < cs->y || y > cs->y + cs->h) continue;
    ccord_push_alpha(r, alpha);
    bool hover = (int)i == ui->hover_crow && !ui->menu.open && !ui->settings_open;
    if (row->dm_nav != DM_NAV_NONE) {
      if (row->dm_nav == DM_NAV_DM_HEADER) {
        ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.5f,
                                       ui_mix(COL(text_muted), COL(text_interactive_hover), ui_fade(ui, "dmhdr", 0, hover)));
        const char *up = "Direct messages";
        float ty = y + (row->h - ui_line_h(ui, st.size)) * 0.5f + 2.0f;
        ccord_text_draw_ellipsis(r, up, strlen(up), cs->x + 14.0f, ty, cs->w - 40.0f, &st);
        ui_icon_plus(r, cs->x + cs->w - 18.0f, y + row->h * 0.5f + 2.0f, 13.0f, st.color);
        ui->dm_header_plus_rect = (ccord_rect){cs->x + cs->w - 32.0f, y, 28.0f, row->h};
        goto row_done;
      }
      ccord_rect item = {cs->x + M(channel_item_margin_x), y, cs->w - 2.0f * M(channel_item_margin_x), row->h};
      bool nav_sel = (row->dm_nav == DM_NAV_FRIENDS && ui_is_dm_friends_view(ui)) ||
                     (row->dm_nav == DM_NAV_MESSAGE_REQUESTS && ui_is_dm_requests_view(ui));
      ui_row_state nav_rs = ui_row_state_of(ui, "dmnav", (uint64_t)row->dm_nav, hover, nav_sel, COL(bg_secondary));
      ccord_color tc = ui_row_state_text(&nav_rs, COL(text_secondary), COL(text_interactive_hover), COL(text_interactive_active));
      ccord_color item_bg = nav_rs.bg;
      ui_row_state_fill(ui, r, &nav_rs, item, M(channel_item_radius));
      float icx = item.x + M(channel_item_pad_x) + 12.0f;
      float icy = item.y + item.h * 0.5f;
      const char *label = "Friends";
      if (row->dm_nav == DM_NAV_FRIENDS) {
        ui_icon_members(r, icx, icy, 20.0f, tc, item_bg);
        label = "Friends";
      } else if (row->dm_nav == DM_NAV_MESSAGE_REQUESTS) {
        ui->dm_requests_nav_rect = item;
        ui_icon_inbox(r, icx, icy, 18.0f, tc, item_bg);
        label = "Message Requests";
      } else if (row->dm_nav == DM_NAV_NITRO) {
        ccord_draw_circle(r, icx, icy, 7.5f, tc);
        ccord_draw_circle(r, icx, icy, 3.2f, item_bg);
        label = "Nitro";
      } else if (row->dm_nav == DM_NAV_SHOP) {
        ccord_draw_rrect(r, (ccord_rect){icx - 7.0f, icy - 6.0f, 14.0f, 12.0f}, 2.5f, tc);
        ccord_draw_rect(r, (ccord_rect){icx - 4.0f, icy - 9.0f, 8.0f, 3.0f}, tc);
        label = "Shop";
      }
      ccord_text_style ns = ui_style(ui, nav_sel ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_base) - 0.5f, tc);
      float tx = item.x + M(channel_item_pad_x) + 32.0f;
      float ty = item.y + (item.h - ui_line_h(ui, ns.size)) * 0.5f;
      mention_badge_layout badge = {0};
      if (row->dm_nav == DM_NAV_MESSAGE_REQUESTS && ui->dm_requests_badge_count > 0) {
        badge = measure_mention_badge(ui, r, ui->dm_requests_badge_count);
      }
      float reserve_nav = badge.len > 0 ? badge.w + 6.0f : 0.0f;
      float avail_nav = item.w - (tx - item.x) - M(channel_item_pad_x) - reserve_nav;
      ccord_text_draw_ellipsis(r, label, strlen(label), tx, ty, avail_nav, &ns);
      if (badge.len > 0) draw_row_mention_badge(ui, r, item, &badge);
      goto row_done;
    }
    if (row->is_relationship) {
      if (row->is_category) {
        ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.5f,
                                       ui_mix(COL(text_muted), COL(text_interactive_hover), ui_fade(ui, "relhdr", 0, hover)));
        const char *up = "Friends";
        float ty = y + (row->h - ui_line_h(ui, st.size)) * 0.5f + 2.0f;
        ccord_text_draw_ellipsis(r, up, strlen(up), cs->x + 14.0f, ty, cs->w - 32.0f, &st);
        goto row_done;
      }
      const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, row->index);
      if (!rel) goto row_done;
      const ccord_user *u = ccord_store_user(ui->d.store, rel->user_id);
      ccord_rect item = {cs->x + M(channel_item_margin_x), y, cs->w - 2.0f * M(channel_item_margin_x), row->h};
      ui_row_state rel_rs = ui_row_state_of(ui, "rel", rel->user_id, hover, false, COL(bg_secondary));
      ui_row_state_fill(ui, r, &rel_rs, item, M(channel_item_radius));
      float av_sz = 30.0f;
      float ax = item.x + M(channel_item_pad_x) + av_sz * 0.5f;
      ui_draw_avatar(ui, r, u, ax, item.y + item.h * 0.5f, av_sz, rel_rs.bg, false);
      const char *fnick = ui_friend_nickname(ui, rel->user_id);
      const char *name = (fnick && fnick[0]) ? fnick
                         : ((u && u->display_name && u->display_name[0]) ? u->display_name
                         : (u ? u->name : "Unknown User"));
      ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base) - 0.5f,
                                     ui_mix(COL(text_normal), COL(text_interactive_hover), rel_rs.h));
      const char *tag = "Friend";
      ccord_color tag_col = COL(status_online);
      if (rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING) {
        tag = "Incoming";
        tag_col = COL(brand);
      } else if (rel->kind == CCORD_RELATIONSHIP_PENDING_OUTGOING) {
        tag = "Outgoing";
        tag_col = COL(status_idle);
      } else if (rel->kind == CCORD_RELATIONSHIP_BLOCKED) {
        tag = "Blocked";
        tag_col = COL(danger);
      }
      ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, tag_col);
      ccord_vec2 tt = ccord_text_measure(r, tag, strlen(tag), &ts, 0.0f);
      float tx = item.x + M(channel_item_pad_x) + av_sz + 10.0f;
      float right = item.x + item.w - tt.x - 16.0f;
      float ty = item.y + (item.h - ui_line_h(ui, ns.size)) * 0.5f;
      ccord_text_draw_ellipsis(r, name, strlen(name), tx, ty, right - tx, &ns);
      ccord_text_draw(r, tag, strlen(tag), item.x + item.w - tt.x - 8.0f,
                      item.y + (item.h - tt.y) * 0.5f, &ts, 0.0f);
      goto row_done;
    }
    const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, row->index);
    if (!ch) goto row_done;
    if (row->is_category) {
      ccord_color cat_rest = ui_channel_muted(ui, ch) ? ui_mix(COL(text_muted), COL(bg_secondary), 0.45f) : COL(text_muted);
      ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.5f,
                                     ui_mix(cat_rest, COL(text_interactive_hover), ui_fade(ui, "cat", ch->id, hover)));
      char cat_lbl[128];
      format_category_label(ch->name, cat_lbl, sizeof cat_lbl);
      float ty = y + (row->h - ui_line_h(ui, st.size)) * 0.5f + 2.0f;
      ui_icon_chevron(r, cs->x + 14.0f, ty + ui_line_h(ui, st.size) * 0.5f, 11.0f,
                      ui_is_collapsed(ui, ch->id) ? ICON_RIGHT : ICON_DOWN, st.color);
      ccord_text_draw_ellipsis(r, cat_lbl, strlen(cat_lbl), cs->x + 25.0f, ty, cs->w - 38.0f, &st);
      goto row_done;
    }
    ccord_rect item = {cs->x + M(channel_item_margin_x), y, cs->w - 2.0f * M(channel_item_margin_x), row->h};
    if (row->is_voice_user) {
      const ccord_user *u = ccord_store_user(ui->d.store, row->user_id);
      const ccord_voice_state *vs = ccord_store_voice_user(ui->d.store, g, row->user_id);
      const ccord_member *vm = ccord_store_find_member(ui->d.store, g, row->user_id);
      bool srv_mute = (vs && vs->mute) || (vm && vm->mute);
      bool srv_deaf = (vs && vs->deaf) || (vm && vm->deaf);
      bool any_mute = (vs && vs->self_mute) || srv_mute;
      bool any_deaf = (vs && vs->self_deaf) || srv_deaf;
      bool speaking = ccord_store_user_speaking(ui->d.store, row->user_id) && !any_mute;
      ui_state_fill(ui, r, item, M(channel_item_radius), "vuser", row->user_id, hover, false);
      float ax = item.x + M(channel_item_pad_x) + 24.0f;
      float ay = item.y + item.h * 0.5f;
      float talk = ui_fade_ms(ui, "talk", row->user_id, speaking, 140);   // the green ring fades with the voice
      if (talk > 0.004f) {
        ccord_draw_rrect(r, (ccord_rect){ax - 11.0f, ay - 11.0f, 22.0f, 22.0f}, 6.0f, ui_alpha(COL(status_online), talk));
      }
      ui_draw_avatar(ui, r, u, ax, ay, 18.0f, (ccord_color){0, 0, 0, 0}, false);
      const char *name = u ? u->name : "Unknown User";
      ccord_text_style ns = ui_style(ui, speaking ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, M(font_size_small),
                                     ui_mix(COL(text_secondary), COL(text_header), talk));
      float tx = ax + 14.0f;
      float right = item.x + item.w - 8.0f;
      if (vs && vs->self_stream) {
        ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 5.0f, COL(text_on_brand));
        ccord_vec2 lt = ccord_text_measure(r, "LIVE", 4, &ls, 0.0f);
        ccord_rect lb = {right - lt.x - 10.0f, item.y + (item.h - 15.0f) * 0.5f, lt.x + 10.0f, 15.0f};
        ccord_draw_rrect(r, lb, 3.0f, COL(danger));
        ccord_text_draw(r, "LIVE", 4, lb.x + 5.0f, lb.y + (lb.h - lt.y) * 0.5f, &ls, 0.0f);
        right = lb.x - 6.0f;
      }
      if (any_deaf) {
        ui_icon_headphones(r, right - 7.0f, ay, 14.0f, srv_deaf ? COL(danger) : COL(text_muted), true);
        right -= 18.0f;
      }
      if (any_mute) {
        ui_icon_mic(r, right - 7.0f, ay, 14.0f, srv_mute ? COL(danger) : COL(text_muted), true);
        right -= 18.0f;
      }
      float ty = item.y + (item.h - ui_line_h(ui, ns.size)) * 0.5f;
      if (right > tx + 8.0f) ccord_text_draw_ellipsis(r, name, strlen(name), tx, ty, right - tx, &ns);
      goto row_done;
    }
    bool selected = ch->id == sel_ch &&
                    (g != CCORD_DM_GUILD_ID || (!ui_is_dm_friends_view(ui) && !ui_is_dm_requests_view(ui)));
    ui_voice_lock_kind vlock_kind = UI_VOICE_LOCK_NONE;
    const char *vlock = (g != CCORD_DM_GUILD_ID && !row->is_thread &&
                         (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE))
                          ? ui_voice_lock_reason_ex(ui, g, ch, &vlock_kind)
                          : nullptr;
    bool voice_locked = (vlock != nullptr);
    ui_row_state rs = ui_row_state_of(ui, "chan", ch->id, hover && !voice_locked, selected && !voice_locked,
                                      COL(bg_secondary));
    if (selected) ui->dbg_chan_sel_fade = rs.s;
    // A muted channel is quiet: no unread marker or bold name, and its name sits back until you point at it.
    bool muted = ui_channel_muted(ui, ch);
    bool has_unread = ch->unread_count > 0 && !muted && !voice_locked;
    ccord_color rest_tc = (voice_locked || muted) ? ui_mix(COL(text_muted), COL(bg_secondary), 0.45f) : COL(text_muted);
    ccord_color tc = voice_locked
                       ? rest_tc
                       : ui_row_state_text(&rs, rest_tc, COL(text_interactive_hover), COL(text_interactive_active));
    ccord_color item_bg = rs.bg;
    ui_row_state_fill(ui, r, &rs, item, M(channel_item_radius));
    // The unread marker grows in (and out) instead of blinking; text weight cannot ease, but its colour does.
    float unread_t = ui_fade_ms(ui, "unread", ch->id, has_unread && !selected, 160);
    if (g == CCORD_DM_GUILD_ID) {
      bool group = ui_channel_is_group_dm(ch);
      const ccord_user *store_u = ui_dm_recipient(ui, ch);
      if (!group) {
        const char *np_asset = (store_u && store_u->nameplate_asset[0] != '\0') ? store_u->nameplate_asset : nullptr;
        if (np_asset || hover || selected) {
          (void)ui_draw_nameplate(ui, r, np_asset, store_u ? store_u->nameplate_palette : nullptr,
                                  item, M(channel_item_radius),
                                  (ui_nameplate_opts){
                                    .hover = hover,
                                    .selected = selected,
                                    .animate = hover || selected,
                                    .reveal_on_active = true,
                                    .anim_ns = "dmnp",
                                    .anim_id = ch->id,
                                  });
        }
      }
      const char *dm_name = ui_dm_display_name(ui, ch);
      const char *dm_avatar = (store_u && store_u->avatar_url && store_u->avatar_url[0]) ? store_u->avatar_url
                              : ((ch->icon_url && ch->icon_url[0]) ? ch->icon_url : "");
      char act_line[160];
      const char *dm_sub = "";
      if (!group && store_u) {
        if (ui_format_user_activity_line(ui, store_u, act_line, sizeof act_line)) {
          dm_sub = act_line;
        } else if (store_u->presence != CCORD_PRESENCE_OFFLINE) {
          dm_sub = presence_label(store_u->presence);
        }
      }
      ccord_user dm_u = {
        .id = store_u ? store_u->id : (ch->user_id != CCORD_ID_NONE ? ch->user_id : ch->id),
        .name = dm_name,
        .status_text = dm_sub,
        .avatar_url = dm_avatar,
        .avatar_decoration_url = (store_u && store_u->avatar_decoration_url) ? store_u->avatar_decoration_url : "",
        .presence = store_u ? store_u->presence : CCORD_PRESENCE_OFFLINE,
        .is_bot = store_u ? store_u->is_bot : false,
        .avatar_palette = store_u ? store_u->avatar_palette : (uint8_t)(ch->id % 6),
      };
      char members_line[32];
      if (group) {
        snprintf(members_line, sizeof members_line, "%zu Members", ui_dm_member_count(ch));
        dm_u.status_text = members_line;
      }
      float icy = item.y + item.h * 0.5f;
      if (unread_t > 0.01f) draw_unread_pill(r, item.x + 2.0f, icy, 10.0f * unread_t, ui_alpha(COL(pill), unread_t));
      float av_sz = 36.0f;
      float ax = item.x + M(channel_item_pad_x) + av_sz * 0.5f;
      if (group) {
        ui_draw_group_dm_icon(ui, r, ch, ax, icy, av_sz);
      } else {
        ui_draw_avatar(ui, r, &dm_u, ax, icy, av_sz, item_bg, false);
      }
      ccord_color dm_rest = muted ? ui_mix(COL(text_secondary), COL(bg_secondary), 0.45f) : COL(text_normal);
      ccord_color dm_tc = ui_row_state_text(&rs, has_unread ? COL(text_header) : dm_rest,
                                            COL(text_interactive_hover), COL(text_interactive_active));
      ccord_text_style st = ui_style(ui, (selected || has_unread) ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR,
                                     M(font_size_base), dm_tc);
      bool dm_pinned = ui_dm_is_pinned(ui, ch->id);
      bool dm_fav = ui_dm_is_favorite(ui, ch->id);
      mention_badge_layout badge = ch->mention_count > 0 ? measure_mention_badge(ui, r, ch->mention_count) : (mention_badge_layout){0};
      float tx = item.x + M(channel_item_pad_x) + av_sz + 12.0f;
      float reserve = badge.len > 0 ? badge.w + 6.0f : ((dm_pinned || dm_fav || hover) ? 22.0f : 0.0f);
      float avail_w = item.w - (tx - item.x) - M(channel_item_pad_x) - reserve;
      if (dm_u.status_text && dm_u.status_text[0]) {
        ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
        float nh = ui_line_h(ui, st.size), sh = ui_line_h(ui, ss.size);
        float ty = icy - (nh + sh) * 0.5f;
        ccord_text_draw_ellipsis(r, dm_name, strlen(dm_name), tx, ty, avail_w, &st);
        ccord_text_draw_ellipsis(r, dm_u.status_text, strlen(dm_u.status_text), tx, ty + nh - 1.0f, avail_w, &ss);
      } else {
        float ty = item.y + (item.h - ui_line_h(ui, st.size)) * 0.5f;
        ccord_text_draw_ellipsis(r, dm_name, strlen(dm_name), tx, ty, avail_w, &st);
      }
      if (badge.len > 0) {
        draw_row_mention_badge(ui, r, item, &badge);
      } else if (hover && ui->ndm_close_rects < 32) {
        ccord_rect cr = {item.x + item.w - 24.0f, item.y + (item.h - 18.0f) * 0.5f, 18.0f, 18.0f};
        ui_icon_close(r, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 10.0f, COL(text_muted));
        ui->dm_close_rects[ui->ndm_close_rects].rect = cr;
        ui->dm_close_rects[ui->ndm_close_rects].channel_id = ch->id;
        ui->ndm_close_rects++;
      } else if (dm_pinned || dm_fav) {
        ui_icon_pin(r, item.x + item.w - 15.0f, item.y + item.h * 0.5f, 11.0f,
                    dm_fav ? COL(brand) : COL(text_muted));
      }
      goto row_done;
    }
    float thread_indent = row->is_thread ? 14.0f : 0.0f;
    float icx = item.x + M(channel_item_pad_x) + 8.0f + thread_indent, icy = item.y + item.h * 0.5f;
    if (row->is_thread) {
      ccord_draw_rect(r, (ccord_rect){item.x + 12.0f, item.y + 4.0f, 1.5f, item.h * 0.5f}, COL(divider));
      ccord_draw_rect(r, (ccord_rect){item.x + 12.0f, icy - 0.75f, 6.0f, 1.5f}, COL(divider));
    }
    if (unread_t > 0.01f) draw_unread_pill(r, item.x + 2.5f, icy, 8.0f * unread_t, ui_alpha(COL(pill), unread_t));
    if (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) {
      if (vlock_kind == UI_VOICE_LOCK_PERMISSION) ui_icon_speaker_locked(r, icx, icy, 17.0f, tc, item_bg);
      else ui_icon_speaker(r, icx, icy, 17.0f, tc);
    } else if (ccord_store_channel_is_forum(ui->d.store, ch->id)) {
      ui_icon_forum(r, icx, icy, 17.0f, tc);
    } else {
      ui_icon_hash(r, icx, icy, row->is_thread ? 14.0f : 17.0f, tc);
    }
    ccord_color name_tc = voice_locked
                            ? rest_tc
                            : ui_row_state_text(&rs, has_unread ? COL(text_header) : rest_tc,
                                                COL(text_interactive_hover), COL(text_interactive_active));
    ccord_text_style st = ui_style(ui, ((selected && !voice_locked) || has_unread) ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR,
                                   row->is_thread ? M(font_size_small) : M(font_size_base) - 0.5f, name_tc);
    float ty = item.y + (item.h - ui_line_h(ui, st.size)) * 0.5f;
    mention_badge_layout badge = ch->mention_count > 0 ? measure_mention_badge(ui, r, ch->mention_count) : (mention_badge_layout){0};
    bool show_gear = !row->is_thread && badge.len == 0 && hover &&
                     ui->nchan_gear_rects < 64 && ui_channel_settings_available(ui, g, ch->id);
    char limit_buf[24] = "";
    float limit_w = 0.0f;
    bool show_limit = !row->is_thread && badge.len == 0 && !show_gear &&
                      ch->kind == CCORD_CHANNEL_VOICE && ch->user_limit > 0;
    ccord_text_style lim_st = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f,
                                       vlock_kind == UI_VOICE_LOCK_FULL ? COL(danger) : rest_tc);
    if (show_limit) {
      size_t vcnt = ccord_store_voice_count(ui->d.store, ch->id);
      snprintf(limit_buf, sizeof limit_buf, "%02zu/%02u", vcnt, (unsigned)ch->user_limit);
      ccord_vec2 lt = ccord_text_measure(r, limit_buf, strlen(limit_buf), &lim_st, 0.0f);
      limit_w = lt.x + 10.0f;
    }
    float reserve = badge.len > 0 ? badge.w + 6.0f : (show_gear ? 22.0f : (show_limit ? limit_w + 6.0f : 0.0f));
    float name_x = item.x + M(channel_item_pad_x) + 25.0f + thread_indent;
    float name_w = ccord_text_draw_ellipsis(r, ch->name, strlen(ch->name), name_x, ty,
                                            item.w - M(channel_item_pad_x) * 2.0f - 25.0f - thread_indent - reserve, &st);
    (void)ui_decor_draw_inline_badges(ui, r, CCORD_DECOR_CHANNEL_BADGE, ch->id, name_x + name_w + 4.0f, item.y, item.h, item_bg);
    if (badge.len > 0) {
      draw_row_mention_badge(ui, r, item, &badge);
    } else if (show_gear) {
      ccord_rect gr = {item.x + item.w - 22.0f, item.y + (item.h - 18.0f) * 0.5f, 18.0f, 18.0f};
      bool ghov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, gr);
      ui_icon_gear(r, gr.x + gr.w * 0.5f, gr.y + gr.h * 0.5f, 14.0f,
                   ghov ? COL(text_header) : COL(text_muted), item_bg);
      ui->chan_gear_rects[ui->nchan_gear_rects].rect = gr;
      ui->chan_gear_rects[ui->nchan_gear_rects].channel_id = ch->id;
      ui->nchan_gear_rects++;
    } else if (show_limit) {
      ccord_rect lr = {item.x + item.w - limit_w - 4.0f, item.y + (item.h - 16.0f) * 0.5f, limit_w, 16.0f};
      ccord_color lbg = vlock_kind == UI_VOICE_LOCK_FULL ? ui_alpha(COL(danger), 0.18f) : COL(bg_tertiary);
      ccord_draw_rrect(r, lr, 4.0f, lbg);
      ccord_vec2 lt = ccord_text_measure(r, limit_buf, strlen(limit_buf), &lim_st, 0.0f);
      ccord_text_draw(r, limit_buf, strlen(limit_buf), lr.x + (lr.w - lt.x) * 0.5f, lr.y + (lr.h - lt.y) * 0.5f,
                      &lim_st, 0.0f);
    }
  row_done:
    ccord_pop_alpha(r);
  }
  ui_track_end(&ui->trk_chan);
  if (ui->drag == DRAG_CHANNEL && ui->drag_active && ui->drag_chan_target_id != CCORD_ID_NONE) {
    float ly = cs->y + ui->drag_line_y - ui->chan_scroll;
    ccord_draw_rrect(r, (ccord_rect){cs->x + 14.0f, ly - 1.25f, cs->w - 24.0f, 2.5f}, 1.25f, COL(brand));
    ccord_draw_circle(r, cs->x + 14.0f, ly, 3.5f, COL(brand));
  }
  ccord_pop_clip(r);
}

// Tooltip for a hovered locked/full voice channel (drawn outside channel_scroll clip so it extends cleanly to the right).
void ui_draw_channel_list_tooltip(ccord_ui *ui, ccord_renderer *r)
{
  if (ui->touch_mode || !ui->mouse_in || ui->hover_crow < 0 || (size_t)ui->hover_crow >= ui->ncrow ||
      ui->menu.open || ui->settings_open || ui->go_live_open ||
      (ui->drag == DRAG_CHANNEL && ui->drag_active)) {
    return;
  }
  for (size_t i = 0; i < ui->nchan_gear_rects; i++) {
    if (ui_in_rect(ui->mx, ui->my, ui->chan_gear_rects[i].rect)) return;
  }
  const chan_row *row = &ui->crow[ui->hover_crow];
  if (row->dm_nav != DM_NAV_NONE || row->is_relationship || row->is_category || row->is_voice_user || row->is_thread) {
    return;
  }
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, row->index);
  if (!ch) return;
  const char *lock = ui_voice_lock_reason(ui, g, ch);
  if (!lock) return;
  const ccord_layout_rect *cs = &ui->layout.channel_scroll;
  float y = cs->y + row->y - ui->chan_scroll;
  if (y + row->h < cs->y || y > cs->y + cs->h) return;
  ccord_rect anchor = {cs->x, y, cs->w, row->h};
  (void)uis_tooltip(ui, r, anchor, lock, UIS_TIP_RIGHT, ch->id ^ 0x564F4943454C4B00ULL);
}

// ---------------------------------------------------------------- user panel

void ui_draw_user_panel(ccord_ui *ui, ccord_renderer *r)
{
  const ccord_layout_rect *p = &ui->layout.user_panel;
  ccord_rect pbg = ui_rect(*p);
  if (ui->mobile_layout && ui->win_h > p->y) {
    pbg.h = maxf(p->h, ui->win_h - p->y);
  }
  ccord_draw_rect(r, pbg, COL(bg_user_panel));
  ccord_draw_rect(r, (ccord_rect){p->x, p->y, p->w, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){p->x + p->w - 1.0f, p->y, 1.0f, pbg.h}, COL(border_subtle));
  ui->user_status_rect = ui_user_status_area_rect(ui);
  bool us_hover = ui->hover_user_status && !ui->menu.open && !ui->settings_open && !ui->go_live_open;
  float us_hv = ui_fade(ui, "ustatus", 1u, us_hover);
  const ccord_user *u = ccord_store_user(ui->d.store, ccord_store_self_id(ui->d.store));
  if (u && u->nameplate_asset[0] != '\0') {
    bool np_anim = us_hover || ui->sc.open;
    (void)ui_draw_nameplate(ui, r, u->nameplate_asset, u->nameplate_palette,
                            (ccord_rect){p->x, p->y + 1.0f, fmaxf(p->w - 1.0f, 0.0f), fmaxf(p->h - 1.0f, 0.0f)},
                            0.0f,
                            (ui_nameplate_opts){
                              .hover = us_hover,
                              .selected = ui->sc.open,
                              .animate = np_anim,
                              .anim_ns = "upnp",
                              .anim_id = u->id,
                            });
  }
  if (us_hv > 0.004f) {
    ccord_draw_rrect(r, ui->user_status_rect, 6.0f, ui_alpha(COL(bg_item_hover), us_hv));
  }
  const ccord_voice_state *self = ui_self_voice(ui);
  bool self_speaking = self && !self->self_mute &&
                       ccord_store_user_speaking(ui->d.store, ccord_store_self_id(ui->d.store));
  float ax = p->x + 12.0f + 16.0f;
  float cy = p->y + p->h * 0.5f;
  if (self_speaking) {
    ccord_draw_rrect(r, (ccord_rect){ax - 18.0f, cy - 18.0f, 36.0f, 36.0f}, 10.0f, COL(status_online));
  }
  ui_draw_avatar(ui, r, u, ax, cy, 32.0f, COL(bg_user_panel), false);
  if (u) {
    float tx = p->x + 12.0f + 32.0f + 10.0f;
    float right_lim = ui_user_btn_rect(ui, 0).x - 4.0f;
    if (ui->d.contrib && ccord_contrib_action_btn_count(ui->d.contrib, CCORD_ACTBTN_USER_PANEL) > 0 && right_lim > tx + 40.0f) {
      ccord_menu_context uctx = {.target = CCORD_MENU_PROFILE, .user_id = u->id};
      ccord_rect uab = {tx + 24.0f, p->y + (p->h - 24.0f) * 0.5f, right_lim - (tx + 24.0f), 24.0f};
      right_lim -= ui_actbtns_draw_slot(ui, r, CCORD_ACTBTN_USER_PANEL, uab, true, &uctx);
    }
    float maxw = right_lim - tx;
    if (maxw < 20.0f) maxw = 20.0f;
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    char cs_buf[224];
    const char *status = presence_label(u->presence);
    bool cs_valid = (u->custom_status_emoji[0] != '\0' || u->custom_status_text[0] != '\0') &&
                    (u->custom_status_expires_ms == 0 || ui->now_unix_ms <= 0 ||
                     u->custom_status_expires_ms > ui->now_unix_ms);
    if (cs_valid) {
      if (u->custom_status_emoji[0] && u->custom_status_text[0]) {
        snprintf(cs_buf, sizeof cs_buf, "%s %s", u->custom_status_emoji, u->custom_status_text);
      } else if (u->custom_status_emoji[0]) {
        snprintf(cs_buf, sizeof cs_buf, "%s", u->custom_status_emoji);
      } else {
        snprintf(cs_buf, sizeof cs_buf, "%s", u->custom_status_text);
      }
      status = cs_buf;
    } else if (ui_format_user_activity_line(ui, u, cs_buf, sizeof cs_buf)) {
      status = cs_buf;
    }
    float nh = ui_line_h(ui, ns.size), sh = ui_line_h(ui, ss.size);
    float ty = cy - (nh + sh) * 0.5f;
    ccord_text_draw_ellipsis(r, u->name, strlen(u->name), tx, ty, maxw, &ns);
    ccord_text_draw_ellipsis(r, status, strlen(status), tx, ty + nh - 1.0f, maxw, &ss);
  }
  bool muted = self ? self->self_mute : (ui->d.session && ccord_session_voice_muted(ui->d.session));
  bool deafened = self ? self->self_deaf : (ui->d.session && ccord_session_voice_deafened(ui->d.session));
  bool hovers[3] = {ui->hover_mute, ui->hover_deafen, ui->hover_gear};
  for (int i = 0; i < 3; i++) {
    ccord_rect b = ui_user_btn_rect(ui, i);
    bool hover = hovers[i] && !ui->menu.open && !ui->settings_open && !ui->go_live_open;
    // The hit rect is 28 px; the shared icon button is drawn at its 32 px token size around the same centre.
    float cx = b.x + b.w * 0.5f, cym = b.y + b.h * 0.5f;
    ccord_rect vb = {cx - UIS_ICON_BTN * 0.5f, cym - UIS_ICON_BTN * 0.5f, UIS_ICON_BTN, UIS_ICON_BTN};
    ccord_color ic = uis_icon_button(ui, r, vb, (uis_state){.hover = hover}, "userbtn", (uint64_t)i);
    if ((i == 0 && muted) || (i == 1 && deafened)) ic = COL(danger);
    if (hover && !ui->touch_mode) uis_tooltip(ui, r, vb, i == 0 ? (muted ? "Unmute" : "Mute") : i == 1 ? (deafened ? "Undeafen" : "Deafen") : "User Settings",
                                              UIS_TIP_ABOVE, 100u + (uint64_t)i);
    if (i == 0) ui_icon_mic(r, cx, cym, 18.0f, ic, muted);
    else if (i == 1) ui_icon_headphones(r, cx, cym, 18.0f, ic, deafened);
    else ui_icon_gear(r, cx, cym, 18.0f, ic, hover ? COL(bg_item_hover) : COL(bg_user_panel));
  }
}

// ---------------------------------------------------------------- voice connected bar

static bool voice_bar_reconnecting(const ccord_ui *ui)
{
  if (!ui || !ui->d.session) return false;
  ccord_rtc_stats st;
  ccord_session_voice_stats(ui->d.session, &st);
  return st.state == CCORD_VOICE_RECONNECTING;
}

const char *ui_voice_bar_title(const ccord_ui *ui)
{
  return voice_bar_reconnecting(ui) ? "Reconnecting..." : "Voice Connected";
}

void ui_draw_voice_bar(ccord_ui *ui, ccord_renderer *r)
{
  ccord_rect bar = ui_voice_bar_rect(ui);
  if (bar.w <= 0.0f || bar.h <= 0.0f) return;
  const ccord_voice_state *v = ui_self_voice(ui);
  const ccord_channel *ch = v ? ccord_store_channel(ui->d.store, v->channel_id) : nullptr;
  ccord_draw_rect(r, bar, COL(bg_user_panel));
  ccord_draw_rect(r, (ccord_rect){bar.x, bar.y, bar.w, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){bar.x + bar.w - 1.0f, bar.y, 1.0f, bar.h}, COL(border_subtle));
  float icon_x = bar.x + 14.0f, cy = bar.y + bar.h * 0.5f;
  ui_icon_speaker(r, icon_x, cy, 18.0f, COL(status_online));
  const char *title = ui_voice_bar_title(ui);
  bool reconnecting = voice_bar_reconnecting(ui);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, reconnecting ? COL(status_idle) : COL(status_online));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 4.0f, COL(text_muted));
  const char *sub = ch ? ch->name : "";
  float tx = icon_x + 14.0f;
  float right = ui_voice_btn_rect(ui, 0).x - 4.0f;
  float lh = ui_line_h(ui, ts.size), sh = ui_line_h(ui, ss.size);
  float ty = cy - (lh + sh) * 0.5f;
  ccord_text_draw_ellipsis(r, title, strlen(title), tx, ty, right - tx, &ts);
  ccord_text_draw_ellipsis(r, sub, strlen(sub), tx, ty + lh - 1.0f, right - tx, &ss);
  bool hovers[3] = {ui->hover_camera, ui->hover_screen, ui->hover_disconnect};
  for (int i = 0; i < 3; i++) {
    ccord_rect b = ui_voice_btn_rect(ui, i);
    float cx = b.x + b.w * 0.5f, cym = b.y + b.h * 0.5f;
    ccord_rect vb = {cx - UIS_ICON_BTN * 0.5f, cym - UIS_ICON_BTN * 0.5f, UIS_ICON_BTN, UIS_ICON_BTN};
    ccord_color ic = uis_icon_button(ui, r, vb, (uis_state){.hover = hovers[i] && !ui->go_live_open}, "voicebtn", (uint64_t)i);
    if (i == 0) ui_icon_camera(r, cx, cym, 18.0f, ic, ui->camera_on);
    else if (i == 1) ui_icon_screen(r, cx, cym, 18.0f, ic);
    else ui_icon_disconnect(r, cx, cym, 16.0f, ic);
  }
}

// ---------------------------------------------------------------- transient notice

void ui_draw_notice(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->notice[0] || ui->settings_open) return;
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
  ccord_rect box;
  float wrap;
  if (ui->mobile_layout) {
    float sl = ui->safe_insets.left, sr = ui->safe_insets.right;
    float sb = fmaxf(ui->safe_insets.bottom, ui->keyboard_inset);
    float uw = fmaxf(ui->win_w - sl - sr, 120.0f);
    float bw = fminf(uw - 24.0f, 360.0f);
    wrap = fmaxf(bw - 20.0f, 60.0f);
    ccord_vec2 t = ccord_text_measure(r, ui->notice, strlen(ui->notice), &st, wrap);
    float h = t.y + 20.0f;
    float base = (ui->layout.composer.h > 0.0f && ui->layout.composer.y > 0.0f &&
                  !ui_server_settings_is_open(ui) && !ui_channel_settings_is_open(ui))
                   ? fminf(ui->layout.composer.y, ui->win_h - sb)
                   : (ui->win_h - sb);
    float by = fmaxf(ui->safe_insets.top + 8.0f, base - h - 10.0f);
    box = (ccord_rect){sl + (uw - bw) * 0.5f, by, bw, h};
  } else {
    const ccord_layout_rect *p = &ui->layout.user_panel;
    ccord_rect bar = ui_voice_bar_rect(ui);
    float base = bar.h > 0.0f ? bar.y : p->y;
    wrap = p->w - 16.0f - 20.0f;
    ccord_vec2 t = ccord_text_measure(r, ui->notice, strlen(ui->notice), &st, wrap);
    float h = t.y + 20.0f;
    box = (ccord_rect){p->x + 8.0f, base - h - 8.0f, p->w - 16.0f, h};
  }
  float ta, tslide;
  ui_toast_look(ui, &ta, &tslide);
  ccord_push_alpha(r, ta);
  ccord_push_offset(r, 0.0f, tslide);
  ccord_draw_rrect(r, (ccord_rect){box.x - 1, box.y - 1, box.w + 2, box.h + 2}, 6.0f, COL(border_subtle));
  ccord_draw_rrect(r, box, 5.0f, COL(bg_floating));
  ccord_text_draw(r, ui->notice, strlen(ui->notice), box.x + 10.0f, box.y + 10.0f, &st, wrap);
  ccord_pop_offset(r);
  ccord_pop_alpha(r);
}
