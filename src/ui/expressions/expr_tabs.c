// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "expr_internal.h"

// ---------------------------------------------------------------- Expression Picker drawing

const char *const ep_k_skin_tone_swatches[6] = {
  "\xF0\x9F\x91\x8F",                 // 👏 Default
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBB", // 👏🏻 Light
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBC", // 👏🏼 Medium-Light
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBD", // 👏🏽 Medium
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBE", // 👏🏾 Medium-Dark
  "\xF0\x9F\x91\x8F\xF0\x9F\x8F\xBF", // 👏🏿 Dark
};

static const ccord_color k_skin_tone_colors[6] = {
  {0xff, 0xc8, 0x3d, 255},
  {0xf7, 0xde, 0xce, 255},
  {0xf3, 0xd2, 0xa2, 255},
  {0xd5, 0xab, 0x88, 255},
  {0xaf, 0x7e, 0x57, 255},
  {0x7c, 0x53, 0x3e, 255},
};

static const char *const k_unicode_cat_icons[CCORD_EMOJI_CATEGORY_COUNT] = {
  "\xF0\x9F\x98\x80", // 😀 People
  "\xF0\x9F\x90\xBB", // 🐻 Nature
  "\xF0\x9F\x8D\x94", // 🍔 Food
  "\xE2\x9A\xBD",     // ⚽ Activities
  "\xF0\x9F\x9A\x97", // 🚗 Travel
  "\xF0\x9F\x92\xA1", // 💡 Objects
  "\xE2\x9D\xA4\xEF\xB8\x8F", // ❤️ Symbols
  "\xF0\x9F\x8F\xB3\xEF\xB8\x8F", // 🏳️ Flags
};

static const char *const k_quick_reaction_shortcodes[UI_EMOJI_PICKER_COUNT] = {
  "thumbsup", "heart", "joy", "open_mouth", "sob", "fire", "tada", "eyes",
};

static bool contains_ci_str(const char *hay, const char *needle)
{
  if (!needle || !needle[0]) return true;
  if (!hay) return false;
  size_t nlen = strlen(needle);
  for (const char *p = hay; *p; p++) {
    if (strncasecmp(p, needle, nlen) == 0) return true;
  }
  return false;
}

static void ensure_emoji_search_cache(const ccord_ui *ui, ui_expr_body *b, const char *query, size_t qlen)
{
  uint64_t srev = ui->d.store ? ccord_store_revision(ui->d.store) : 0u;
  uint64_t erev = ui->d.store ? ccord_store_expr_revision(ui->d.store) : 0u;
  if (b->emoji_cache_valid && b->emoji_cache_srev == srev && b->emoji_cache_erev == erev &&
      strncmp(b->emoji_cache_query, query, sizeof b->emoji_cache_query) == 0) {
    return;
  }
  b->emoji_custom_count = 0;
  size_t ng = ui->d.store ? ccord_store_guild_count(ui->d.store) : 0;
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
    if (!g) continue;
    size_t ne = ccord_store_guild_emoji_count(ui->d.store, g->id);
    for (size_t ei = 0; ei < ne; ei++) {
      const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, g->id, ei);
      if (!ce || !contains_ci_str(ce->name, query)) continue;
      if (b->emoji_custom_count == b->emoji_custom_cap) {
        size_t ncap = b->emoji_custom_cap ? b->emoji_custom_cap * 2u : 32u;
        expr_custom_emoji_hit *nv = ccord_realloc(b->emoji_custom_hits, ncap * sizeof *nv);
        if (!nv) break;
        b->emoji_custom_hits = nv;
        b->emoji_custom_cap = ncap;
      }
      b->emoji_custom_hits[b->emoji_custom_count++] = (expr_custom_emoji_hit){gi, ei};
    }
  }
  b->emoji_unicode_count = ccord_emoji_search(query, qlen, b->emoji_unicode_hits, 96);
  b->emoji_cache_srev = srev;
  b->emoji_cache_erev = erev;
  snprintf(b->emoji_cache_query, sizeof b->emoji_cache_query, "%.127s", query);
  b->emoji_cache_valid = true;
}

static void record_emoji_cell(ccord_ui *ui, ui_expr_body *b, ccord_rect cell,
                              bool is_custom, ccord_id custom_id, ccord_id guild_id, bool animated,
                              const char *name, const char *utf8, const char *sub, size_t *cell_seq,
                              ccord_rect clip_area)
{
  size_t idx = (*cell_seq)++;
  if (idx < UI_EMOJI_PICKER_COUNT) {
    ui->emoji_picker_cells[idx] = cell;
  }
  // Only cells inside the viewport earn hit slots: off-screen items must not exhaust EXPR_HIT_MAX
  // (which would leave the visible emojis unclickable) nor leave rects clickable outside the grid.
  if (cell.y + cell.h < clip_area.y || cell.y > clip_area.y + clip_area.h) return;
  if (b->nhits >= EXPR_HIT_MAX) return;
  expr_hit_cell *h = &b->hits[b->nhits++];
  memset(h, 0, sizeof *h);
  h->rect = cell;
  h->kind = is_custom ? EXPR_ITEM_CUSTOM_EMOJI : EXPR_ITEM_UNICODE_EMOJI;
  h->id = custom_id;
  h->guild_id = guild_id;
  h->animated = animated;
  snprintf(h->name, sizeof h->name, "%.63s", name ? name : "");
  snprintf(h->utf8, sizeof h->utf8, "%.31s", utf8 ? utf8 : "");
  snprintf(h->sub, sizeof h->sub, "%.63s", sub ? sub : "");
}

static void draw_emoji_cell(ccord_ui *ui, ccord_renderer *r, ui_expr_body *b, ccord_rect cell,
                            bool is_custom, ccord_id custom_id, ccord_id guild_id, bool animated,
                            const char *name, const char *utf8, const char *sub, size_t *cell_seq,
                            ccord_rect clip_area)
{
  size_t nhits_before = b->nhits;
  record_emoji_cell(ui, b, cell, is_custom, custom_id, guild_id, animated, name, utf8, sub, cell_seq, clip_area);
  if (cell.y + cell.h < clip_area.y || cell.y > clip_area.y + clip_area.h) return;

  bool hov = ui->mouse_in && !b->skin_popup_open && ui_in_rect(ui->mx, ui->my, clip_area) &&
             ui_in_rect(ui->mx, ui->my, cell);
  bool recorded = b->nhits > nhits_before;
  if (hov && recorded) b->hovered_hit = (int)(b->nhits - 1);
  float ecell_h = ui_fade(ui, "ecell", is_custom ? (uint64_t)custom_id : ui_name_id(utf8), hov && recorded);
  if (ecell_h > 0.004f) ccord_draw_rrect(r, cell, 6.0f, ui_alpha(COL(bg_item_hover), ecell_h));
  if (is_custom) {
    ccord_rect img = {cell.x + 4.0f, cell.y + 4.0f, cell.w - 8.0f, cell.h - 8.0f};
    ui_expr_draw_custom_emoji(ui, r, custom_id, name, name ? strlen(name) : 0, animated, img);
  } else if (utf8 && utf8[0]) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_EMOJI, 22.0f, COL(text_normal));
    ccord_vec2 et = ccord_text_measure(r, utf8, strlen(utf8), &es, 0.0f);
    ccord_text_draw(r, utf8, strlen(utf8), cell.x + (cell.w - et.x) * 0.5f,
                    cell.y + (cell.h - et.y) * 0.5f, &es, 0.0f);
  }
}

static void draw_section_header(ccord_ui *ui, ccord_renderer *r, const char *title, float x, float y, float w,
                                ccord_rect clip_area)
{
  if (y + 22.0f < clip_area.y || y > clip_area.y + clip_area.h) return;
  char up[96];
  size_t i = 0;
  for (; title && title[i] && i < sizeof up - 1; i++) {
    up[i] = (char)toupper((unsigned char)title[i]);
  }
  up[i] = '\0';
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, 11.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, up, i, x + 2.0f, y + 4.0f, w - 4.0f, &hs);
}

// Discord server icon in a 28x28 rail button: CDN texture via ui_guild_icon_url + ui_ensure_media,
// falling back to the hashed circle + initials while the icon is absent or still loading.
static void draw_rail_guild_icon(ccord_ui *ui, ccord_renderer *r, const expr_rail_item *ri, float cx, float cy)
{
  const ccord_guild *g = ri->guild_id != CCORD_ID_NONE ? ccord_store_guild(ui->d.store, ri->guild_id) : nullptr;
  char icon_url[256];
  if (g && ui_guild_icon_url(g, icon_url, sizeof icon_url)) {
    bool ready = ui_ensure_media(ui, r, icon_url);
    int tw = 0, th = 0;
    if (ready && ccord_render_has_texture(r, icon_url, &tw, &th) && tw > 1 && th > 1) {
      ccord_draw_texture_rounded(r, icon_url, ri->rect, ri->rect.w * 0.5f, 255);
      ui->dbg_expr_rail_icons_drawn++;
      return;
    }
  }
  char init[4] = {0};
  ui_initials(ri->label, init);
  ccord_color gc = ep_palette_from_hash(ri->guild_id);
  ccord_draw_circle(r, cx, cy, 11.0f, gc);
  ccord_text_style is = ui_style(ui, CCORD_FONT_BOLD, 9.5f, (ccord_color){255, 255, 255, 255});
  ccord_vec2 im = ccord_text_measure(r, init, strlen(init), &is, 0.0f);
  ccord_text_draw(r, init, strlen(init), cx - im.x * 0.5f, cy - im.y * 0.5f, &is, 0.0f);
}

void ep_draw_emoji_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  ui_expr_body *b = ep->body;
  float rail_w = EXPR_RAIL_W;
  float preview_h = 44.0f;
  ccord_rect rail_rect = {body.x, body.y, rail_w, body.h - preview_h};
  ccord_rect grid_area = {body.x + rail_w, body.y, body.w - rail_w, body.h - preview_h};
  ccord_rect prev_rect = {body.x, body.y + body.h - preview_h, body.w, preview_h};
  // The bottom strip of the rail holds the pinned Default Unicode button; the rest scrolls on its own.
  ccord_rect rail_view = {rail_rect.x, rail_rect.y, rail_rect.w, ep_maxf(rail_rect.h - EXPR_RAIL_PINNED_H, 0.0f)};
  b->rail_rect = rail_rect;
  b->rail_view = rail_view;
  b->hits_clip = grid_area;

  ccord_draw_rect(r, rail_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){grid_area.x, grid_area.y, 1.0f, grid_area.h}, COL(border_subtle));

  const char *query = ccord_text_edit_cstr(&b->search_edit);
  size_t qlen = b->search_edit.len;
  float scroll = ep->scroll_y[EXPR_TAB_EMOJI];

  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(body.w, &lay);
  const float cell_sz = lay.emoji_cell;
  const float gap = lay.emoji_gap;
  const float step_y = cell_sz + gap;
  float gx0 = grid_area.x + EXPR_GRID_PAD;
  float gy = grid_area.y + 6.0f - scroll;
  float gw = grid_area.w - EXPR_GRID_PAD * 2.0f;
  // As many cells as the popover width allows (8 at the default width); never more than fit inside the grid.
  int cols = lay.emoji_cols;
  size_t cell_seq = 0;

  b->nrail = 0;
  float ry = rail_view.y + 8.0f - ep->rail_scroll_y;

  ccord_push_clip(r, grid_area);

  if (qlen > 0) {
    draw_section_header(ui, r, "SEARCH RESULTS", gx0, gy, gw, grid_area);
    gy += 24.0f;
    ensure_emoji_search_cache(ui, b, query, qlen);
    int col = 0;
    // 1. Cached matching custom guild emojis
    for (size_t ci = 0; ci < b->emoji_custom_count; ci++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, b->emoji_custom_hits[ci].guild_idx);
      if (!g) continue;
      const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, g->id, b->emoji_custom_hits[ci].emoji_idx);
      if (!ce) continue;
      ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
      draw_emoji_cell(ui, r, b, cell, true, ce->id, g->id, ce->animated, ce->name, "", g->name, &cell_seq, grid_area);
      if (++col >= cols) { col = 0; gy += step_y; }
    }
    // 2. Cached matching Unicode emojis
    for (size_t mi = 0; mi < b->emoji_unicode_count; mi++) {
      const ccord_emoji_entry *e = ccord_emoji_at(b->emoji_unicode_hits[mi]);
      if (!e) continue;
      ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
      if (cell_seq >= UI_EMOJI_PICKER_COUNT && (cell.y + cell.h < grid_area.y || cell.y > grid_area.y + grid_area.h)) {
        cell_seq++;
      } else {
        char toned[32];
        const char *u = e->utf8;
        if (e->has_skin_tones && ep->skin_tone > 0 && ccord_emoji_with_tone(e, ep->skin_tone, toned, sizeof toned) > 0) {
          u = toned;
        }
        draw_emoji_cell(ui, r, b, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, u,
                        ccord_emoji_category_name(e->category), &cell_seq, grid_area);
      }
      if (++col >= cols) { col = 0; gy += step_y; }
    }
    if (col > 0) gy += step_y;
  } else {
    // 1. FREQUENTLY USED
    if (b->nrail < EXPR_RAIL_MAX) {
      b->rail[b->nrail++] = (expr_rail_item){
        .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
        .target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
        .section_id = -1,
        .label = "Frequently Used",
      };
      ry += 32.0f;
    }
    draw_section_header(ui, r, "FREQUENTLY USED", gx0, gy, gw, grid_area);
    gy += 24.0f;
    int col = 0;
    // Always include the 8 quick reaction emojis first so indices 0..7 match k_quick_reaction_shortcodes
    for (size_t qi = 0; qi < UI_EMOJI_PICKER_COUNT; qi++) {
      const char *sc = k_quick_reaction_shortcodes[qi];
      const ccord_emoji_entry *e = ccord_emoji_find_shortcode(sc, strlen(sc));
      if (!e) continue;
      char toned[32];
      const char *u = e->utf8;
      if (e->has_skin_tones && ep->skin_tone > 0 && ccord_emoji_with_tone(e, ep->skin_tone, toned, sizeof toned) > 0) {
        u = toned;
      }
      ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
      draw_emoji_cell(ui, r, b, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, u,
                      "Frequently Used", &cell_seq, grid_area);
      if (++col >= cols) { col = 0; gy += step_y; }
    }
    size_t nfreq = ccord_store_frequent_emoji_count(ui->d.store);
    for (size_t fi = 0; fi < nfreq && fi < 8; fi++) {
      const char *key = ccord_store_frequent_emoji_at(ui->d.store, fi);
      if (!key || !key[0]) continue;
      const ccord_emoji_entry *e = ccord_emoji_find_shortcode(key, strlen(key));
      if (e) {
        bool dup = false;
        for (size_t qi = 0; qi < UI_EMOJI_PICKER_COUNT; qi++) {
          if (strcmp(e->name, k_quick_reaction_shortcodes[qi]) == 0) { dup = true; break; }
        }
        if (dup) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_emoji_cell(ui, r, b, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, e->utf8,
                        "Frequently Used", &cell_seq, grid_area);
        if (++col >= cols) { col = 0; gy += step_y; }
      } else {
        const ccord_emoji *ce = ccord_store_find_emoji_by_name(ui->d.store, key);
        if (ce) {
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
          draw_emoji_cell(ui, r, b, cell, true, ce->id, CCORD_ID_NONE, ce->animated, ce->name, "",
                          "Frequently Used", &cell_seq, grid_area);
          if (++col >= cols) { col = 0; gy += step_y; }
        }
      }
    }
    if (col > 0) { col = 0; gy += step_y; }
    gy += 4.0f;

    // 2. FAVORITES
    size_t nfav = ccord_store_favorite_emoji_count(ui->d.store);
    if (nfav > 0) {
      if (b->nrail < EXPR_RAIL_MAX) {
        b->rail[b->nrail++] = (expr_rail_item){
          .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
          .target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
          .section_id = -2,
          .label = "Favorites",
        };
        ry += 32.0f;
      }
      draw_section_header(ui, r, "FAVORITES", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t fi = 0; fi < nfav; fi++) {
        const char *fk = ccord_store_favorite_emoji_at(ui->d.store, fi);
        if (!fk) continue;
        const ccord_emoji *ce = ccord_store_find_emoji_by_name(ui->d.store, fk);
        if (ce) {
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
          draw_emoji_cell(ui, r, b, cell, true, ce->id, CCORD_ID_NONE, ce->animated, ce->name, "",
                          "Favorites", &cell_seq, grid_area);
          if (++col >= cols) { col = 0; gy += step_y; }
          continue;
        }
        const ccord_emoji_entry *e = ccord_emoji_find_shortcode(fk, strlen(fk));
        if (e) {
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
          draw_emoji_cell(ui, r, b, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, e->utf8,
                          "Favorites", &cell_seq, grid_area);
          if (++col >= cols) { col = 0; gy += step_y; }
        }
      }
      if (col > 0) { col = 0; gy += step_y; }
      gy += 4.0f;
    }

    // 3. GUILD CUSTOM EMOJIS (skip off-screen sections/rows in O(1))
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
      if (!g) continue;
      size_t ne = ccord_store_guild_emoji_count(ui->d.store, g->id);
      if (ne == 0) continue;
      if (b->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &b->rail[b->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = 100 + (int)gi;
        ri->guild_id = g->id;
        snprintf(ri->label, sizeof ri->label, "%.31s", g->name ? g->name : "Server");
        ry += 32.0f;
      }
      draw_section_header(ui, r, g->name ? g->name : "SERVER EMOJIS", gx0, gy, gw, grid_area);
      gy += 24.0f;
      size_t rows = (ne + (size_t)cols - 1u) / (size_t)cols;
      float sec_h = (float)rows * step_y;
      if (cell_seq >= UI_EMOJI_PICKER_COUNT && (gy + sec_h < grid_area.y || gy > grid_area.y + grid_area.h)) {
        cell_seq += ne;
        gy += sec_h + 4.0f;
        continue;
      }
      size_t start_row = 0;
      if (cell_seq >= UI_EMOJI_PICKER_COUNT && gy < grid_area.y) {
        float skip = floorf((grid_area.y - gy) / step_y);
        if (skip > 1.0f) start_row = (size_t)(skip - 1.0f);
      }
      size_t start_ei = start_row * (size_t)cols;
      if (start_ei > ne) start_ei = ne;
      size_t local_seq = cell_seq + start_ei;
      float row_y = gy + (float)start_row * step_y;
      col = 0;
      for (size_t ei = start_ei; ei < ne; ei++) {
        if (local_seq >= UI_EMOJI_PICKER_COUNT && row_y > grid_area.y + grid_area.h) break;
        const ccord_emoji *ce = ccord_store_guild_emoji_at(ui->d.store, g->id, ei);
        if (!ce) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), row_y, cell_sz, cell_sz};
        draw_emoji_cell(ui, r, b, cell, true, ce->id, g->id, ce->animated, ce->name, "",
                        g->name ? g->name : "Custom Emoji", &local_seq, grid_area);
        if (++col >= cols) { col = 0; row_y += step_y; }
      }
      col = 0;
      cell_seq += ne;
      gy += sec_h + 4.0f;
    }

    // 4. 8 UNICODE CATEGORIES (skip off-screen categories/rows in O(1))
    for (int cat = 0; cat < CCORD_EMOJI_CATEGORY_COUNT; cat++) {
      size_t cfirst = 0, ccount = 0;
      ccord_emoji_category_range((ccord_emoji_category)cat, &cfirst, &ccount);
      if (ccount == 0) continue;
      const char *cname = ccord_emoji_category_name((ccord_emoji_category)cat);
      if (b->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &b->rail[b->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = cat;
        snprintf(ri->label, sizeof ri->label, "%.31s", cname);
        ry += 30.0f;
      }
      draw_section_header(ui, r, cname, gx0, gy, gw, grid_area);
      gy += 24.0f;
      size_t rows = (ccount + (size_t)cols - 1u) / (size_t)cols;
      float sec_h = (float)rows * step_y;
      if (cell_seq >= UI_EMOJI_PICKER_COUNT && (gy + sec_h < grid_area.y || gy > grid_area.y + grid_area.h)) {
        cell_seq += ccount;
        gy += sec_h + 4.0f;
        continue;
      }
      size_t start_row = 0;
      if (cell_seq >= UI_EMOJI_PICKER_COUNT && gy < grid_area.y) {
        float skip = floorf((grid_area.y - gy) / step_y);
        if (skip > 1.0f) start_row = (size_t)(skip - 1.0f);
      }
      size_t start_ei = start_row * (size_t)cols;
      if (start_ei > ccount) start_ei = ccount;
      size_t local_seq = cell_seq + start_ei;
      float row_y = gy + (float)start_row * step_y;
      col = 0;
      for (size_t ei = start_ei; ei < ccount; ei++) {
        if (local_seq >= UI_EMOJI_PICKER_COUNT && row_y > grid_area.y + grid_area.h) break;
        const ccord_emoji_entry *e = ccord_emoji_at(cfirst + ei);
        if (!e) continue;
        char toned[32];
        const char *u = e->utf8;
        if (e->has_skin_tones && ep->skin_tone > 0 && ccord_emoji_with_tone(e, ep->skin_tone, toned, sizeof toned) > 0) {
          u = toned;
        }
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), row_y, cell_sz, cell_sz};
        draw_emoji_cell(ui, r, b, cell, false, CCORD_ID_NONE, CCORD_ID_NONE, false, e->name, u, cname,
                        &local_seq, grid_area);
        if (++col >= cols) { col = 0; row_y += step_y; }
      }
      col = 0;
      cell_seq += ccount;
      gy += sec_h + 4.0f;
    }
  }

  b->content_h[EXPR_TAB_EMOJI] = ep_maxf((gy + scroll) - grid_area.y + 12.0f, grid_area.h);
  ccord_pop_clip(r);

  // Rail scroll bounds + pending "Default Unicode emoji" jump from the pinned button.
  b->rail_content_h = ep_maxf((ry + ep->rail_scroll_y) - rail_view.y + 8.0f, 0.0f);
  ep_expr_rail_set_scroll(ep, ep->rail_scroll_y);
  if (b->pending_unicode_jump) {
    b->pending_unicode_jump = false;
    for (size_t i = 0; i < b->nrail; i++) {
      if (b->rail[i].section_id != 0) continue;
      ccord_scroller_cancel(&b->glide[EXPR_TAB_EMOJI]);   // a category jump: no glide left over from a wheel notch
      ep->scroll_y[EXPR_TAB_EMOJI] = b->rail[i].target_scroll_y;
      scroll = ep->scroll_y[EXPR_TAB_EMOJI];
      // Bring the first Unicode category button into the rail view as well.
      float unscrolled_y = b->rail[i].rect.y + ep->rail_scroll_y;
      ep_expr_rail_set_scroll(ep, unscrolled_y - rail_view.y - 4.0f);
      // The grid above was drawn at the old scroll; force a redraw so the Unicode section and its
      // hit cells actually appear even when no fade/animation schedules another frame (reduced motion).
      ui->dirty = true;
      break;
    }
  }

  // Draw Left Category Rail icons, clipped to the scrollable rail view
  bool unicode_active = false;
  ccord_push_clip(r, rail_view);
  for (size_t i = 0; i < b->nrail; i++) {
    const expr_rail_item *ri = &b->rail[i];
    if (ri->rect.y + ri->rect.h < rail_view.y || ri->rect.y > rail_view.y + rail_view.h) continue;
    bool active = (i + 1 == b->nrail || (scroll >= ri->target_scroll_y - 16.0f &&
                   scroll < b->rail[i + 1].target_scroll_y - 16.0f));
    if (active && ri->section_id >= 0 && ri->section_id < CCORD_EMOJI_CATEGORY_COUNT) unicode_active = true;
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rail_view) && ui_in_rect(ui->mx, ui->my, ri->rect);
    uis_list_row_fill(ui, r, ri->rect, (uis_state){.hover = hov, .selected = active}, "erail", (uint64_t)i);
    float cx = ri->rect.x + ri->rect.w * 0.5f;
    float cy = ri->rect.y + ri->rect.h * 0.5f;
    if (ri->section_id == -1) {
      ep_draw_clock_icon(r, cx, cy, 8.5f, active ? COL(text_header) : COL(text_secondary),
                      active ? COL(bg_item_selected) : COL(bg_tertiary));
    } else if (ri->section_id == -2) {
      ep_draw_star_icon(r, cx, cy, 8.5f, (ccord_color){0xf0, 0xb2, 0x32, 255});
    } else if (ri->section_id >= 100) {
      draw_rail_guild_icon(ui, r, ri, cx, cy);
    } else if (ri->section_id >= 0 && ri->section_id < CCORD_EMOJI_CATEGORY_COUNT) {
      const char *ico = k_unicode_cat_icons[ri->section_id];
      ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, 15.0f, COL(text_normal));
      ccord_vec2 em = ccord_text_measure(r, ico, strlen(ico), &es, 0.0f);
      ccord_text_draw(r, ico, strlen(ico), cx - em.x * 0.5f, cy - em.y * 0.5f, &es, 0.0f);
    }
  }
  ccord_pop_clip(r);

  // Pinned Default Unicode Emoji button: fixed bottom strip, always visible (even during search).
  ccord_rect pin_zone = {rail_rect.x, rail_rect.y + rail_rect.h - EXPR_RAIL_PINNED_H, rail_rect.w, EXPR_RAIL_PINNED_H};
  ccord_draw_rect(r, (ccord_rect){pin_zone.x, pin_zone.y, pin_zone.w, 1.0f}, COL(border_subtle));
  b->rail_pinned_rect = (ccord_rect){rail_rect.x + 6.0f, pin_zone.y + 4.0f, 28.0f, 28.0f};
  bool pin_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, b->rail_pinned_rect);
  bool pin_active = unicode_active && qlen == 0;
  uis_list_row_fill(ui, r, b->rail_pinned_rect, (uis_state){.hover = pin_hov, .selected = pin_active}, "erailpin", 0);
  ui_expr_draw_smiley(r, b->rail_pinned_rect.x + 14.0f, b->rail_pinned_rect.y + 14.0f, 18.0f,
                      ui_state_mix(ui, "erailpinico", 0, pin_hov, pin_active, COL(text_secondary), COL(text_header),
                                   COL(status_idle)));

  // Bottom Hover Preview Bar
  ccord_draw_rect(r, prev_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){prev_rect.x, prev_rect.y, prev_rect.w, 1.0f}, COL(border_subtle));
  const expr_hit_cell *preview = nullptr;
  if (b->hovered_hit >= 0 && (size_t)b->hovered_hit < b->nhits) {
    preview = &b->hits[b->hovered_hit];
  } else if (b->nhits > 0) {
    preview = &b->hits[0];
  }
  if (preview) {
    ccord_rect picon = {prev_rect.x + 12.0f, prev_rect.y + 8.0f, 28.0f, 28.0f};
    if (preview->kind == EXPR_ITEM_CUSTOM_EMOJI) {
      ui_expr_draw_custom_emoji(ui, r, preview->id, preview->name, strlen(preview->name), preview->animated, picon);
    } else if (preview->utf8[0]) {
      ccord_text_style es = ui_style(ui, CCORD_FONT_EMOJI, 22.0f, COL(text_normal));
      ccord_vec2 et = ccord_text_measure(r, preview->utf8, strlen(preview->utf8), &es, 0.0f);
      ccord_text_draw(r, preview->utf8, strlen(preview->utf8),
                      picon.x + (picon.w - et.x) * 0.5f, picon.y + (picon.h - et.y) * 0.5f, &es, 0.0f);
    }
    char sc_buf[80];
    snprintf(sc_buf, sizeof sc_buf, ":%s:", preview->name[0] ? preview->name : "emoji");
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float nw = ccord_text_draw_ellipsis(r, sc_buf, strlen(sc_buf), picon.x + 36.0f,
                                        prev_rect.y + (prev_rect.h - ui_line_h(ui, ns.size)) * 0.5f,
                                        prev_rect.w - 160.0f, &ns);
    if (preview->sub[0]) {
      ccord_text_draw_ellipsis(r, preview->sub, strlen(preview->sub), picon.x + 46.0f + nw,
                               prev_rect.y + (prev_rect.h - ui_line_h(ui, ss.size)) * 0.5f,
                               prev_rect.w - (60.0f + nw), &ss);
    }
  }

  // Skin-tone selector popup if open
  if (b->skin_popup_open) {
    float sw = 32.0f * 6.0f + 10.0f;
    float sh = 38.0f;
    ccord_rect pop = {b->skin_btn_rect.x + b->skin_btn_rect.w - sw, b->skin_btn_rect.y + b->skin_btn_rect.h + 4.0f, sw, sh};
    uis_popover_frame(ui, r, pop);
    for (int t = 0; t < 6; t++) {
      ccord_rect sr = {pop.x + 5.0f + (float)t * 32.0f, pop.y + 4.0f, 30.0f, 30.0f};
      b->skin_option_rects[t] = sr;
      bool sel = (int)ep->skin_tone == t;
      bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, sr);
      ccord_draw_rrect(r, sr, 5.0f, ui_state_mix(ui, "eskin", (uint64_t)t, hov, sel, ui_alpha(COL(bg_item_hover), 0.0f),
                                                 COL(bg_item_hover), COL(brand)));
      ccord_draw_circle(r, sr.x + sr.w * 0.5f, sr.y + sr.h * 0.5f, 10.0f, k_skin_tone_colors[t]);
    }
  }
}

static void gif_title_from_url(const char *url, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  const char *slash = url ? strrchr(url, '/') : nullptr;
  const char *slug = (slash && slash[1]) ? slash + 1 : "Tenor GIF";
  size_t i = 0;
  for (; slug[i] && i + 1 < cap; i++) {
    if (strncmp(&slug[i], "-gif-", 5) == 0 || slug[i] == '.') break;
    out[i] = (slug[i] == '-') ? ' ' : slug[i];
  }
  out[i] = '\0';
  if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
  if (!out[0]) snprintf(out, cap, "Tenor GIF");
}

// Previews are decoded no larger than this: a tile is about 200 px wide.
#define GIF_PREVIEW_DIM 256

static bool gif_auto_play(const ccord_ui *ui)
{
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return us ? us->gif_auto_play : true;
}

static void draw_gif_card(ccord_ui *ui, ccord_renderer *r, ui_expr_body *b, const ccord_gif_item *g,
                          ccord_rect card, ccord_rect clip_area)
{
  if (!g) return;
  // Cull before formatting/recording: off-screen tiles must not consume hit slots or stay clickable.
  if (card.y + card.h < clip_area.y || card.y > clip_area.y + clip_area.h) return;
  char title_buf[64];
  gif_title_from_url(g->url, title_buf, sizeof title_buf);
  const char *purl = g->src[0] ? g->src : g->url;
  expr_hit_cell *hit = nullptr;
  if (b->nhits < EXPR_HIT_MAX) {
    hit = &b->hits[b->nhits++];
    memset(hit, 0, sizeof *hit);
    hit->rect = card;
    hit->star_rect = (ccord_rect){card.x + card.w - 28.0f, card.y + 6.0f, 22.0f, 22.0f};
    hit->kind = EXPR_ITEM_GIF_TILE;
    snprintf(hit->name, sizeof hit->name, "%.63s", title_buf);
    snprintf(hit->url, sizeof hit->url, "%s", g->url);
    snprintf(hit->preview_url, sizeof hit->preview_url, "%s", purl);
    hit->gif_w = g->width;
    hit->gif_h = g->height;
    hit->gif_format = g->format;
  }

  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, clip_area) && ui_in_rect(ui->mx, ui->my, card);
  // The preview is the GIF itself, playing: a small decode of the animation (older favourites point at a
  // short video instead, which plays the same way). A favourite saved as a VIDEO (settings-proto format 2) is a
  // video whatever its src looks like: Tenor/Klipy media URLs do not always end in .mp4, and without allow_video the
  // bytes go to the still-image decoder, which refuses them ("Couldn't load").
  // A scheme-less src ("//media.tenor.com/x/y.mp4") is loaded as https://; the stored favorite (and what a star
  // click writes back) keeps its src as it came. The host allow-list still applies to the https:// form.
  char load_buf[CCORD_URL_MAX_BYTES + 8];
  const char *lurl = purl;
  if (purl[0] == '/' && purl[1] == '/') {
    (void)snprintf(load_buf, sizeof load_buf, "https:%s", purl);
    lurl = load_buf;
  }
  bool video = g->format == 2u || ui_url_path_ends(lurl, ".mp4") || ui_url_path_ends(lurl, ".webm");
  // The ring, caption and star share one hover fade (ui_fade), so none of them snaps on the pointer.
  float gh = ui_fade(ui, "gifcell", ui_name_id(g->url), hov);
  float gh_cap = hit ? gh : 0.0f;
  if (gh > 0.004f) {   // a ring around the tile under the pointer
    ccord_draw_rrect(r, (ccord_rect){card.x - 2.0f, card.y - 2.0f, card.w + 4.0f, card.h + 4.0f}, 8.0f,
                     ui_alpha(COL(brand), gh));
  }
  if (ui_ensure_media_ex(ui, r, lurl, GIF_PREVIEW_DIM, video)) {
    ccord_draw_texture_cover(r, lurl, card, 6.0f, 255, gif_auto_play(ui) || hov);
  } else {
    // Still on its way (or lost): a quiet tile that says which.
    ccord_draw_rrect(r, card, 6.0f, COL(bg_tertiary));
    const char *note = ui_media_failed(ui, lurl) ? "Couldn't load" : "GIF";
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, 11.0f, COL(text_muted));
    ccord_vec2 nm = ccord_text_measure(r, note, strlen(note), &ns, 0.0f);
    ccord_text_draw(r, note, strlen(note), card.x + (card.w - nm.x) * 0.5f, card.y + (card.h - nm.y) * 0.5f, &ns, 0.0f);
  }

  if (hov && hit) b->hovered_hit = (int)(b->nhits - 1);
  if (gh_cap > 0.004f) {
    // The name only where the pointer is, so the pictures are not half covered by labels.
    ccord_rect cap_r = {card.x, card.y + card.h - 22.0f, card.w, 22.0f};
    ccord_draw_rrect(r, cap_r, 4.0f, ui_alpha((ccord_color){0x11, 0x12, 0x14, 195}, gh_cap));
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, 10.5f, ui_alpha((ccord_color){255, 255, 255, 255}, gh_cap));
    ccord_text_draw_ellipsis(r, title_buf, strlen(title_buf), cap_r.x + 7.0f,
                             cap_r.y + (cap_r.h - ui_line_h(ui, ts.size)) * 0.5f, cap_r.w - 14.0f, &ts);
  }
  // Favorite star button in top-right corner: always shown on a favorite (and subtly on touch), eased in on hover otherwise.
  bool is_fav = ccord_store_is_favorite_gif(ui->d.store, g->url);
  float star_a = is_fav ? 1.0f : (ui->touch_mode ? 0.72f : gh_cap);
  if (star_a > 0.004f && hit) {
    ccord_draw_rrect(r, hit->star_rect, 4.0f, ui_alpha((ccord_color){0x11, 0x12, 0x14, 210}, star_a));
    ccord_color scol = is_fav ? (ccord_color){0xf0, 0xb2, 0x32, 255} : (ccord_color){220, 222, 228, 255};
    scol = ui_alpha(scol, star_a);
    ep_draw_star_icon(r, hit->star_rect.x + hit->star_rect.w * 0.5f,
                   hit->star_rect.y + hit->star_rect.h * 0.5f, 6.5f, scol);
  }
}

// Columns of GIFs (2 at the default popover width), each tile in its picture's own shape and placed in the shorter column, so nothing is
// squashed into a fixed box. Returns the y below the taller column.
static float draw_gif_grid(ccord_ui *ui, ccord_renderer *r, ui_expr_body *b,
                           const ccord_gif_item *(*at)(const ccord_store *, size_t), size_t n, float x, float y,
                           float col_w, int cols, float gap, ccord_rect body)
{
  float col_y[EXPR_GIF_MAX_COLS];
  if (cols < 1) cols = 1;
  if (cols > EXPR_GIF_MAX_COLS) cols = EXPR_GIF_MAX_COLS;
  for (int c = 0; c < cols; c++) col_y[c] = y;
  for (size_t i = 0; i < n; i++) {
    const ccord_gif_item *g = at(ui->d.store, i);
    if (!g) continue;
    // Discord's GIF masonry sizes a tile as columnWidth * (height / width) with no clamp, so the shortest-column choice
    // below matches the official client's; only an unknown shape falls back to 4:3.
    float th = col_w * 0.75f;
    if (g->width > 0 && g->height > 0) th = col_w * (float)g->height / (float)g->width;
    int col = 0;   // the shortest column takes the next tile
    for (int c = 1; c < cols; c++) {
      if (col_y[c] + 0.5f < col_y[col]) col = c;
    }
    ccord_rect card = {x + (float)col * (col_w + gap), col_y[col], col_w, th};
    draw_gif_card(ui, r, b, g, card, body);
    col_y[col] += th + gap;
  }
  float bottom = col_y[0];
  for (int c = 1; c < cols; c++) bottom = ep_maxf(bottom, col_y[c]);
  return bottom;
}

// Loading / empty hint for GIF lists, drawn inside the clipped tab body.
static float draw_gif_hint(ccord_ui *ui, ccord_renderer *r, const char *msg, float x, float y, float w)
{
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw(r, msg, strlen(msg), x, y, &hs, w);
  return y + 24.0f;
}

void ep_draw_gifs_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  ui_expr_body *b = ep->body;
  ccord_push_clip(r, body);
  b->hits_clip = body;
  float scroll = ep->scroll_y[EXPR_TAB_GIFS];
  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(body.w, &lay);
  float pad = EXPR_GRID_PAD;
  float gap = lay.gif_gap;
  float col_w = lay.gif_col_w;     // one GIF tile column
  int gif_cols = lay.gif_cols;
  float cat_w = lay.gif_cat_w;     // half-width Favorites / Trending / category tiles
  float y = body.y + 8.0f - scroll;
  const char *query = ccord_text_edit_cstr(&b->search_edit);

  b->back_btn_rect = (ccord_rect){0, 0, 0, 0};

  if (query[0] != '\0') {
    draw_section_header(ui, r, "SEARCH RESULTS", body.x + pad, y, body.w - pad * 2.0f, body);
    y += 24.0f;
    size_t n = ccord_store_gif_search_count(ui->d.store);
    y = draw_gif_grid(ui, r, b, ccord_store_gif_search_at, n, body.x + pad, y, col_w, gif_cols, gap, body);
    if (n == 0) {
      ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, CCORD_EXPR_GIF_SEARCH, query);
      const char *hint = (st == CCORD_EXPR_BLOB_PENDING || st == CCORD_EXPR_BLOB_NONE)
                           ? "Searching..."
                           : "No GIFs found. Try a different search.";
      y = draw_gif_hint(ui, r, hint, body.x + pad, y, body.w - pad * 2.0f);
    }
  } else if (b->gif_subview == 1) {
    // Favorites subview
    ccord_rect back = {body.x + pad, y, 82.0f, 22.0f};
    b->back_btn_rect = back;
    ccord_draw_rrect(r, back, 4.0f, COL(bg_tertiary));
    ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 11.0f, COL(text_header));
    ccord_text_draw(r, "< Back", 6, back.x + 10.0f, back.y + 4.0f, &bs, 0.0f);
    draw_section_header(ui, r, "FAVORITE GIFS", body.x + pad + 90.0f, y, body.w - 110.0f, body);
    y += 28.0f;
    size_t nfav = ccord_store_favorite_gif_count(ui->d.store);
    y = draw_gif_grid(ui, r, b, ccord_store_favorite_gif_at, nfav, body.x + pad, y, col_w, gif_cols, gap, body);
    if (nfav == 0) {
      ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, CCORD_EXPR_SETTINGS_PROTO, nullptr);
      const char *hint = (st == CCORD_EXPR_BLOB_PENDING || st == CCORD_EXPR_BLOB_NONE)
                           ? "Loading Favorite GIFs..."
                           : "No Favorite GIFs yet. Star a GIF to keep it here.";
      y = draw_gif_hint(ui, r, hint, body.x + pad, y, body.w - pad * 2.0f);
    }
  } else {
    // Home view: Favorites & Trending cards + Category tiles + Trending GIFs 2-column grid
    float cat_h = 58.0f;
    ccord_rect fav_tile = {body.x + pad, y, cat_w, cat_h};
    ccord_rect trd_tile = {body.x + pad + cat_w + gap, y, cat_w, cat_h};
    bool cards_visible = (y + cat_h >= body.y && y <= body.y + body.h);

    if (cards_visible && b->nhits < EXPR_HIT_MAX) {
      expr_hit_cell *hf = &b->hits[b->nhits++];
      memset(hf, 0, sizeof *hf);
      hf->rect = fav_tile;
      hf->kind = EXPR_ITEM_GIF_FAV_CAT;
      snprintf(hf->name, sizeof hf->name, "Favorites");
    }
    if (cards_visible && b->nhits < EXPR_HIT_MAX) {
      expr_hit_cell *ht = &b->hits[b->nhits++];
      memset(ht, 0, sizeof *ht);
      ht->rect = trd_tile;
      ht->kind = EXPR_ITEM_GIF_TREND_CAT;
      snprintf(ht->name, sizeof ht->name, "Trending GIFs");
    }

    if (cards_visible) {
      // Favorites card with star badge
      ccord_draw_rrect(r, fav_tile, 6.0f, (ccord_color){0x58, 0x65, 0xf2, 255});
      ep_draw_star_icon(r, fav_tile.x + 22.0f, fav_tile.y + cat_h * 0.5f, 9.0f, (ccord_color){0xf0, 0xb2, 0x32, 255});
      char flbl[48];
      snprintf(flbl, sizeof flbl, "Favorites (%zu)", ccord_store_favorite_gif_count(ui->d.store));
      ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, 12.5f, (ccord_color){255, 255, 255, 255});
      ccord_text_draw_ellipsis(r, flbl, strlen(flbl), fav_tile.x + 38.0f,
                               fav_tile.y + (cat_h - ui_line_h(ui, cs.size)) * 0.5f, fav_tile.w - 44.0f, &cs);

      // Trending GIFs card
      ccord_draw_rrect(r, trd_tile, 6.0f, (ccord_color){0x9b, 0x59, 0xb6, 255});
      ccord_text_draw_ellipsis(r, "Trending GIFs", 13, trd_tile.x + 16.0f,
                               trd_tile.y + (cat_h - ui_line_h(ui, cs.size)) * 0.5f, trd_tile.w - 24.0f, &cs);
    }
    y += cat_h + gap;

    // Category tiles: every loaded category, with its cover art when available
    size_t ncat = ccord_store_gif_category_count(ui->d.store);
    float sm_cat_h = 46.0f;
    for (size_t i = 0; i < ncat; i++) {
      const ccord_gif_category *cat = ccord_store_gif_category_at(ui->d.store, i);
      if (!cat) continue;
      int col = (int)(i % 2u);
      ccord_rect cr = {body.x + pad + (float)col * (cat_w + gap), y, cat_w, sm_cat_h};
      bool cat_visible = (cr.y + cr.h >= body.y && cr.y <= body.y + body.h);
      if (cat_visible && b->nhits < EXPR_HIT_MAX) {
        expr_hit_cell *hc = &b->hits[b->nhits++];
        memset(hc, 0, sizeof *hc);
        hc->rect = cr;
        hc->kind = EXPR_ITEM_GIF_CATEGORY;
        snprintf(hc->name, sizeof hc->name, "%.63s", cat->name);
        snprintf(hc->sub, sizeof hc->sub, "%.63s", cat->name);
      }
      if (cat_visible) {
        bool cover = false;
        if (cat->src[0]) {
          bool ready = ui_ensure_media_ex(ui, r, cat->src, GIF_PREVIEW_DIM, false);
          int tw = 0, th = 0;
          cover = ready && ccord_render_has_texture(r, cat->src, &tw, &th) && tw > 1 && th > 1;
          if (ready) ccord_draw_texture_cover(r, cat->src, cr, 6.0f, 255, gif_auto_play(ui));
        }
        if (cover) {
          // Dark scrim so the white label stays readable over the cover art
          ccord_draw_rrect(r, cr, 6.0f, (ccord_color){0x11, 0x12, 0x14, 110});
        } else {
          ccord_color ccol = ep_palette_from_hash(i + 2u);
          ccord_draw_rrect(r, cr, 6.0f, ui_mix(ccol, COL(bg_tertiary), 0.35f));
        }
        ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, 12.0f, (ccord_color){255, 255, 255, 255});
        ccord_vec2 cm = ccord_text_measure(r, cat->name, strlen(cat->name), &cs, 0.0f);
        ccord_text_draw(r, cat->name, strlen(cat->name), cr.x + (cr.w - cm.x) * 0.5f,
                        cr.y + (cr.h - cm.y) * 0.5f, &cs, 0.0f);
      }
      if (col == 1 || i + 1 == ncat) y += sm_cat_h + gap;
    }

    // Trending GIFs 2-column grid directly visible below categories!
    draw_section_header(ui, r, "TRENDING GIFS", body.x + pad, y, body.w - pad * 2.0f, body);
    y += 22.0f;
    size_t ntr = ccord_store_gif_trending_count(ui->d.store);
    y = draw_gif_grid(ui, r, b, ccord_store_gif_trending_at, ntr, body.x + pad, y, col_w, gif_cols, gap, body);
    if (ntr == 0) {
      ccord_expr_blob_state st = ccord_store_expr_blob_state(ui->d.store, CCORD_EXPR_GIF_TRENDING, nullptr);
      const char *hint = (st == CCORD_EXPR_BLOB_PENDING || st == CCORD_EXPR_BLOB_NONE)
                           ? "Loading Trending GIFs..."
                           : "No Trending GIFs right now.";
      y = draw_gif_hint(ui, r, hint, body.x + pad, y, body.w - pad * 2.0f);
    }
  }

  b->content_h[EXPR_TAB_GIFS] = ep_maxf((y + scroll) - body.y + 12.0f, body.h);
  ccord_pop_clip(r);
}

static void ensure_sticker_search_cache(const ccord_ui *ui, ui_expr_body *b, const char *query)
{
  uint64_t srev = ui->d.store ? ccord_store_revision(ui->d.store) : 0u;
  uint64_t erev = ui->d.store ? ccord_store_expr_revision(ui->d.store) : 0u;
  if (b->sticker_cache_valid && b->sticker_cache_srev == srev && b->sticker_cache_erev == erev &&
      strncmp(b->sticker_cache_query, query, sizeof b->sticker_cache_query) == 0) {
    return;
  }
  b->sticker_count = 0;
  size_t ng = ui->d.store ? ccord_store_guild_count(ui->d.store) : 0;
  for (size_t gi = 0; gi < ng; gi++) {
    const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
    if (!g) continue;
    size_t ns = ccord_store_guild_sticker_count(ui->d.store, g->id);
    for (size_t si = 0; si < ns; si++) {
      const ccord_sticker *st = ccord_store_guild_sticker_at(ui->d.store, g->id, si);
      if (!st || (!contains_ci_str(st->name, query) && !contains_ci_str(st->tags, query))) continue;
      if (b->sticker_count == b->sticker_cap) {
        size_t ncap = b->sticker_cap ? b->sticker_cap * 2u : 32u;
        expr_sticker_hit *nv = ccord_realloc(b->sticker_hits, ncap * sizeof *nv);
        if (!nv) break;
        b->sticker_hits = nv;
        b->sticker_cap = ncap;
      }
      b->sticker_hits[b->sticker_count++] = (expr_sticker_hit){false, gi, si};
    }
  }
  size_t np = ui->d.store ? ccord_store_sticker_pack_count(ui->d.store) : 0;
  for (size_t pi = 0; pi < np; pi++) {
    const ccord_sticker_pack *pk = ccord_store_sticker_pack_at(ui->d.store, pi);
    if (!pk) continue;
    for (size_t si = 0; si < pk->sticker_count; si++) {
      const ccord_sticker *st = &pk->stickers[si];
      if (!contains_ci_str(st->name, query) && !contains_ci_str(st->tags, query)) continue;
      if (b->sticker_count == b->sticker_cap) {
        size_t ncap = b->sticker_cap ? b->sticker_cap * 2u : 32u;
        expr_sticker_hit *nv = ccord_realloc(b->sticker_hits, ncap * sizeof *nv);
        if (!nv) break;
        b->sticker_hits = nv;
        b->sticker_cap = ncap;
      }
      b->sticker_hits[b->sticker_count++] = (expr_sticker_hit){true, pi, si};
    }
  }
  b->sticker_cache_srev = srev;
  b->sticker_cache_erev = erev;
  snprintf(b->sticker_cache_query, sizeof b->sticker_cache_query, "%.127s", query);
  b->sticker_cache_valid = true;
}

static void draw_sticker_cell(ccord_ui *ui, ccord_renderer *r, ui_expr_body *b, const ccord_sticker *st,
                              const char *source_name, ccord_rect cell, ccord_rect clip_area)
{
  if (!st) return;
  // Cull before recording: off-screen stickers must not consume hit slots or stay clickable.
  if (cell.y + cell.h < clip_area.y || cell.y > clip_area.y + clip_area.h) return;
  expr_hit_cell *hit = nullptr;
  if (b->nhits < EXPR_HIT_MAX) {
    hit = &b->hits[b->nhits++];
    memset(hit, 0, sizeof *hit);
    hit->rect = cell;
    hit->star_rect = (ccord_rect){cell.x + cell.w - 24.0f, cell.y + 4.0f, 20.0f, 20.0f};
    hit->kind = EXPR_ITEM_STICKER;
    hit->id = st->id;
    hit->guild_id = st->guild_id;
    hit->format_type = (uint8_t)st->format;
    snprintf(hit->name, sizeof hit->name, "%.63s", st->name);
    snprintf(hit->sub, sizeof hit->sub, "%.63s", source_name ? source_name : "Sticker");
  }

  bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, clip_area) && ui_in_rect(ui->mx, ui->my, cell);
  if (hov && hit) b->hovered_hit = (int)(b->nhits - 1);
  // One sticker can sit in several sections (Frequently Used, a pack, Favorites): key each instance by its section too,
  // so hovering one copy does not drive the fade of another.
  uint64_t inst = ui_name_id(source_name ? source_name : "") + (uint64_t)st->id * 0x9E3779B97F4A7C15ull;
  float scell_h = ui_fade(ui, "scell", inst, hov && hit);
  if (scell_h > 0.004f) {
    ccord_draw_rrect(r, (ccord_rect){cell.x - 2.0f, cell.y - 2.0f, cell.w + 4.0f, cell.h + 4.0f}, 9.0f,
                     ui_alpha(COL(bg_item_hover), scell_h));
  }
  ui_expr_draw_sticker_art(ui, r, st->id, st->name, strlen(st->name), (uint8_t)st->format, cell, false);

  // The star eases in and out on the pointer (always subtly visible on touch so users can favorite without hover).
  bool is_fav = ccord_store_is_favorite_sticker(ui->d.store, st->id);
  float star_h = ui_fade(ui, "sstar", inst, hov && hit);
  float star_a = is_fav ? 1.0f : (ui->touch_mode ? 0.72f : star_h);
  if (star_a > 0.004f && hit) {
    ccord_draw_rrect(r, hit->star_rect, 4.0f, ui_alpha((ccord_color){0x11, 0x12, 0x14, 210}, star_a));
    ccord_color scol = is_fav ? (ccord_color){0xf0, 0xb2, 0x32, 255} : (ccord_color){220, 222, 228, 255};
    scol = ui_alpha(scol, star_a);
    ep_draw_star_icon(r, hit->star_rect.x + hit->star_rect.w * 0.5f,
                   hit->star_rect.y + hit->star_rect.h * 0.5f, 6.0f, scol);
  }
}

void ep_draw_stickers_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  ui_expr_body *b = ep->body;
  float rail_w = EXPR_RAIL_W;
  float preview_h = 40.0f;
  ccord_rect rail_rect = {body.x, body.y, rail_w, body.h - preview_h};
  ccord_rect grid_area = {body.x + rail_w, body.y, body.w - rail_w, body.h - preview_h};
  ccord_rect prev_rect = {body.x, body.y + body.h - preview_h, body.w, preview_h};
  b->rail_rect = rail_rect;
  b->rail_view = rail_rect;
  b->hits_clip = grid_area;

  ccord_draw_rect(r, rail_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){grid_area.x, grid_area.y, 1.0f, grid_area.h}, COL(border_subtle));

  const char *query = ccord_text_edit_cstr(&b->search_edit);
  float scroll = ep->scroll_y[EXPR_TAB_STICKERS];

  ui_expr_grid_layout lay;
  ui_expr_grid_layout_for_width(body.w, &lay);
  const int cols = lay.sticker_cols;   // 3 at the default popover width
  const float gap = lay.sticker_gap;
  const float pad = EXPR_GRID_PAD;
  const float cell_sz = lay.sticker_cell;
  const float step_y = cell_sz + gap;
  float gx0 = grid_area.x + pad;
  float gy = grid_area.y + 6.0f - scroll;
  float gw = grid_area.w - pad * 2.0f;

  b->nrail = 0;
  float ry = rail_rect.y + 8.0f - ep->sticker_rail_scroll_y;

  ccord_push_clip(r, grid_area);

  if (query[0] != '\0') {
    draw_section_header(ui, r, "STICKER SEARCH RESULTS", gx0, gy, gw, grid_area);
    gy += 24.0f;
    ensure_sticker_search_cache(ui, b, query);
    size_t n = b->sticker_count;
    size_t rows = n > 0 ? (n + (size_t)cols - 1u) / (size_t)cols : 0u;
    float sec_h = (float)rows * step_y;
    if (n > 0 && !(gy + sec_h < grid_area.y || gy > grid_area.y + grid_area.h)) {
      size_t start_row = 0;
      if (gy < grid_area.y) {
        float skip = floorf((grid_area.y - gy) / step_y);
        if (skip > 1.0f) start_row = (size_t)(skip - 1.0f);
      }
      size_t start_i = start_row * (size_t)cols;
      if (start_i > n) start_i = n;
      float row_y = gy + (float)start_row * step_y;
      int col = 0;
      for (size_t i = start_i; i < n; i++) {
        if (row_y > grid_area.y + grid_area.h) break;
        const expr_sticker_hit *sh = &b->sticker_hits[i];
        const ccord_sticker *st = nullptr;
        const char *src_name = nullptr;
        if (sh->is_pack) {
          const ccord_sticker_pack *pk = ccord_store_sticker_pack_at(ui->d.store, sh->container_idx);
          if (pk && sh->sticker_idx < pk->sticker_count) {
            st = &pk->stickers[sh->sticker_idx];
            src_name = pk->name;
          }
        } else {
          const ccord_guild *g = ccord_store_guild_at(ui->d.store, sh->container_idx);
          if (g) {
            st = ccord_store_guild_sticker_at(ui->d.store, g->id, sh->sticker_idx);
            src_name = g->name;
          }
        }
        if (st) {
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), row_y, cell_sz, cell_sz};
          draw_sticker_cell(ui, r, b, st, src_name, cell, grid_area);
        }
        if (++col >= cols) { col = 0; row_y += step_y; }
      }
    }
    gy += sec_h;
  } else {
    int col = 0;
    // 1. FREQUENTLY USED
    size_t nfreq = ccord_store_frequent_sticker_count(ui->d.store);
    if (nfreq > 0) {
      if (b->nrail < EXPR_RAIL_MAX) {
        b->rail[b->nrail++] = (expr_rail_item){
          .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
          .target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
          .section_id = -1,
          .label = "Frequently Used",
        };
        ry += 32.0f;
      }
      draw_section_header(ui, r, "FREQUENTLY USED", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t fi = 0; fi < nfreq && fi < 6; fi++) {
        ccord_id sid = ccord_store_frequent_sticker_at(ui->d.store, fi);
        const ccord_sticker *st = ccord_store_find_sticker(ui->d.store, sid);
        if (!st) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, b, st, "Frequently Used", cell, grid_area);
        if (++col >= cols) { col = 0; gy += step_y; }
      }
      if (col > 0) { col = 0; gy += step_y; }
      gy += 6.0f;
    }

    // 2. FAVORITES
    size_t nfav = ccord_store_favorite_sticker_count(ui->d.store);
    if (nfav > 0) {
      if (b->nrail < EXPR_RAIL_MAX) {
        b->rail[b->nrail++] = (expr_rail_item){
          .rect = {rail_rect.x + 6.0f, ry, 28.0f, 28.0f},
          .target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f),
          .section_id = -2,
          .label = "Favorites",
        };
        ry += 32.0f;
      }
      draw_section_header(ui, r, "FAVORITE STICKERS", gx0, gy, gw, grid_area);
      gy += 24.0f;
      for (size_t fi = 0; fi < nfav; fi++) {
        ccord_id sid = ccord_store_favorite_sticker_at(ui->d.store, fi);
        const ccord_sticker *st = ccord_store_find_sticker(ui->d.store, sid);
        if (!st) continue;
        ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), gy, cell_sz, cell_sz};
        draw_sticker_cell(ui, r, b, st, "Favorites", cell, grid_area);
        if (++col >= cols) { col = 0; gy += step_y; }
      }
      if (col > 0) { col = 0; gy += step_y; }
      gy += 6.0f;
    }

    // 3. GUILD STICKERS (cull off-screen sections/rows)
    size_t ng = ccord_store_guild_count(ui->d.store);
    for (size_t gi = 0; gi < ng; gi++) {
      const ccord_guild *g = ccord_store_guild_at(ui->d.store, gi);
      if (!g) continue;
      size_t ns = ccord_store_guild_sticker_count(ui->d.store, g->id);
      if (ns == 0) continue;
      if (b->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &b->rail[b->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = 100 + (int)gi;
        ri->guild_id = g->id;
        snprintf(ri->label, sizeof ri->label, "%.31s", g->name ? g->name : "Server");
        ry += 32.0f;
      }
      draw_section_header(ui, r, g->name ? g->name : "SERVER STICKERS", gx0, gy, gw, grid_area);
      gy += 24.0f;
      size_t rows = (ns + (size_t)cols - 1u) / (size_t)cols;
      float sec_h = (float)rows * step_y;
      if (!(gy + sec_h < grid_area.y || gy > grid_area.y + grid_area.h)) {
        size_t start_row = 0;
        if (gy < grid_area.y) {
          float skip = floorf((grid_area.y - gy) / step_y);
          if (skip > 1.0f) start_row = (size_t)(skip - 1.0f);
        }
        size_t start_si = start_row * (size_t)cols;
        if (start_si > ns) start_si = ns;
        float row_y = gy + (float)start_row * step_y;
        col = 0;
        for (size_t si = start_si; si < ns; si++) {
          if (row_y > grid_area.y + grid_area.h) break;
          const ccord_sticker *st = ccord_store_guild_sticker_at(ui->d.store, g->id, si);
          if (!st) continue;
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), row_y, cell_sz, cell_sz};
          draw_sticker_cell(ui, r, b, st, g->name, cell, grid_area);
          if (++col >= cols) { col = 0; row_y += step_y; }
        }
        col = 0;
      }
      gy += sec_h + 6.0f;
    }

    // 4. STANDARD STICKER PACKS (cull off-screen packs/rows)
    size_t np = ccord_store_sticker_pack_count(ui->d.store);
    for (size_t pi = 0; pi < np; pi++) {
      const ccord_sticker_pack *pk = ccord_store_sticker_pack_at(ui->d.store, pi);
      if (!pk || pk->sticker_count == 0) continue;
      if (b->nrail < EXPR_RAIL_MAX) {
        expr_rail_item *ri = &b->rail[b->nrail++];
        ri->rect = (ccord_rect){rail_rect.x + 6.0f, ry, 28.0f, 28.0f};
        ri->target_scroll_y = ep_maxf((gy + scroll) - (grid_area.y + 6.0f), 0.0f);
        ri->section_id = 200 + (int)pi;
        ri->guild_id = pk->id;
        snprintf(ri->label, sizeof ri->label, "%.31s", pk->name);
        ry += 32.0f;
      }
      draw_section_header(ui, r, pk->name, gx0, gy, gw, grid_area);
      gy += 24.0f;
      size_t rows = (pk->sticker_count + (size_t)cols - 1u) / (size_t)cols;
      float sec_h = (float)rows * step_y;
      if (!(gy + sec_h < grid_area.y || gy > grid_area.y + grid_area.h)) {
        size_t start_row = 0;
        if (gy < grid_area.y) {
          float skip = floorf((grid_area.y - gy) / step_y);
          if (skip > 1.0f) start_row = (size_t)(skip - 1.0f);
        }
        size_t start_si = start_row * (size_t)cols;
        if (start_si > pk->sticker_count) start_si = pk->sticker_count;
        float row_y = gy + (float)start_row * step_y;
        col = 0;
        for (size_t si = start_si; si < pk->sticker_count; si++) {
          if (row_y > grid_area.y + grid_area.h) break;
          const ccord_sticker *st = &pk->stickers[si];
          ccord_rect cell = {gx0 + (float)col * (cell_sz + gap), row_y, cell_sz, cell_sz};
          draw_sticker_cell(ui, r, b, st, pk->name, cell, grid_area);
          if (++col >= cols) { col = 0; row_y += step_y; }
        }
        col = 0;
      }
      gy += sec_h + 6.0f;
    }
  }

  b->content_h[EXPR_TAB_STICKERS] = ep_maxf((gy + scroll) - grid_area.y + 12.0f, grid_area.h);
  b->rail_content_h = ep_maxf((ry + ep->sticker_rail_scroll_y) - rail_rect.y + 8.0f, 0.0f);
  ep_expr_rail_set_scroll(ep, ep->sticker_rail_scroll_y);
  ccord_pop_clip(r);

  // Left rail icons for Stickers tab (clipped: the shared rail cap allows more items than fit)
  ccord_push_clip(r, rail_rect);
  for (size_t i = 0; i < b->nrail; i++) {
    const expr_rail_item *ri = &b->rail[i];
    if (ri->rect.y + ri->rect.h < rail_rect.y || ri->rect.y > rail_rect.y + rail_rect.h) continue;
    bool active = (i + 1 == b->nrail || (scroll >= ri->target_scroll_y - 16.0f &&
                   scroll < b->rail[i + 1].target_scroll_y - 16.0f));
    bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rail_rect) && ui_in_rect(ui->mx, ui->my, ri->rect);
    uis_list_row_fill(ui, r, ri->rect, (uis_state){.hover = hov, .selected = active}, "erail", (uint64_t)i);
    float cx = ri->rect.x + ri->rect.w * 0.5f;
    float cy = ri->rect.y + ri->rect.h * 0.5f;
    if (ri->section_id == -1) {
      ep_draw_clock_icon(r, cx, cy, 8.5f, active ? COL(text_header) : COL(text_secondary),
                      active ? COL(bg_item_selected) : COL(bg_tertiary));
    } else if (ri->section_id == -2) {
      ep_draw_star_icon(r, cx, cy, 8.5f, (ccord_color){0xf0, 0xb2, 0x32, 255});
    } else {
      draw_rail_guild_icon(ui, r, ri, cx, cy);
    }
  }
  ccord_pop_clip(r);

  // Bottom hover preview bar for Stickers
  ccord_draw_rect(r, prev_rect, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){prev_rect.x, prev_rect.y, prev_rect.w, 1.0f}, COL(border_subtle));
  const expr_hit_cell *preview = nullptr;
  if (b->hovered_hit >= 0 && (size_t)b->hovered_hit < b->nhits) {
    preview = &b->hits[b->hovered_hit];
  } else if (b->nhits > 0) {
    preview = &b->hits[0];
  }
  if (preview) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float nw = ccord_text_draw_ellipsis(r, preview->name, strlen(preview->name), prev_rect.x + 14.0f,
                                        prev_rect.y + (prev_rect.h - ui_line_h(ui, ns.size)) * 0.5f,
                                        prev_rect.w * 0.55f, &ns);
    if (preview->sub[0]) {
      ccord_text_draw_ellipsis(r, preview->sub, strlen(preview->sub), prev_rect.x + 24.0f + nw,
                               prev_rect.y + (prev_rect.h - ui_line_h(ui, ss.size)) * 0.5f,
                               prev_rect.w - (36.0f + nw), &ss);
    }
  }
}

static void ensure_soundboard_cache(const ccord_ui *ui, ui_expr_body *b, ccord_id guild, const char *query)
{
  uint64_t srev = ui->d.store ? ccord_store_revision(ui->d.store) : 0u;
  uint64_t erev = ui->d.store ? ccord_store_expr_revision(ui->d.store) : 0u;
  if (b->sb_cache_valid && b->sb_cache_srev == srev && b->sb_cache_erev == erev &&
      b->sb_cache_guild == guild && strncmp(b->sb_cache_query, query, sizeof b->sb_cache_query) == 0) {
    return;
  }
  b->sb_count = 0;
  size_t ns = (ui->d.store && guild != CCORD_ID_NONE) ? ccord_store_guild_soundboard_count(ui->d.store, guild) : 0;
  for (size_t i = 0; i < ns; i++) {
    const ccord_soundboard_sound *snd = ccord_store_guild_soundboard_at(ui->d.store, guild, i);
    if (!snd || !snd->available) continue;
    if (query[0] != '\0' && !contains_ci_str(snd->name, query)) continue;
    if (b->sb_count == b->sb_cap) {
      size_t ncap = b->sb_cap ? b->sb_cap * 2u : 16u;
      size_t *nv = ccord_realloc(b->sb_hits, ncap * sizeof *nv);
      if (!nv) break;
      b->sb_hits = nv;
      b->sb_cap = ncap;
    }
    b->sb_hits[b->sb_count++] = i;
  }
  b->sb_cache_srev = srev;
  b->sb_cache_erev = erev;
  b->sb_cache_guild = guild;
  snprintf(b->sb_cache_query, sizeof b->sb_cache_query, "%.127s", query);
  b->sb_cache_valid = true;
}

// M5 soundboard: the current server's sounds, playable only while connected to its voice. Playback is
// a session intent (the server answers availability / USE_SOUNDBOARD); nothing is mutated locally.
void ep_draw_soundboard_tab(ccord_ui *ui, ccord_renderer *r, struct ui_expr_picker *ep, ccord_rect body)
{
  ui_expr_body *b = ep->body;
  const char *query = ccord_text_edit_cstr(&b->search_edit);
  float scroll = ep->scroll_y[EXPR_TAB_SOUNDBOARD];
  b->nrail = 0;
  ccord_id guild = ccord_store_selected_guild(ui->d.store);
  ccord_id avc = ccord_store_active_voice_channel(ui->d.store);
  const ccord_channel *avch = avc != CCORD_ID_NONE ? ccord_store_channel(ui->d.store, avc) : nullptr;
  if (avch && avch->guild_id != CCORD_DM_GUILD_ID) guild = avch->guild_id;
  ccord_id self = ccord_store_self_id(ui->d.store);
  const ccord_voice_state *sv = guild != CCORD_ID_NONE ? ccord_store_voice_user(ui->d.store, guild, self) : nullptr;
  bool in_voice = sv && sv->channel_id != CCORD_ID_NONE;
  bool can_play = in_voice && ccord_store_has_permission(ui->d.store, guild, avch ? avch->id : CCORD_ID_NONE,
                                                         self, CCORD_PERM_USE_SOUNDBOARD);

  ccord_push_clip(r, body);
  b->hits_clip = body;
  float gx = body.x + 12.0f, gw = body.w - 24.0f;
  float gy = body.y + 8.0f - scroll;
  if (!in_voice || !can_play) {
    const char *hint = !in_voice ? "Join a voice channel to play sounds."
                                 : "You need Use Soundboard permission in this channel.";
    ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    ccord_text_draw(r, hint, strlen(hint), gx, gy, &hs, gw);
    gy += 24.0f;
  }
  ensure_soundboard_cache(ui, b, guild, query);
  float play_w = b->sb_count > 0 ? uis_label_width(ui, r, "Play") : 0.0f;
  size_t shown = b->sb_count;
  for (size_t idx = 0; idx < b->sb_count; idx++) {
    ccord_rect row = {gx, gy, gw, 40.0f};
    bool row_visible = (row.y + row.h >= body.y && row.y <= body.y + body.h);
    if (row_visible) {
      const ccord_soundboard_sound *snd = ccord_store_guild_soundboard_at(ui->d.store, guild, b->sb_hits[idx]);
      if (snd) {
        if (b->nhits < EXPR_HIT_MAX) {
          expr_hit_cell *h = &b->hits[b->nhits++];
          memset(h, 0, sizeof *h);
          h->rect = row;
          h->kind = EXPR_ITEM_SOUNDBOARD;
          h->id = snd->id;
          h->guild_id = guild;
          snprintf(h->name, sizeof h->name, "%s", snd->name ? snd->name : "Sound");
        }
        bool hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, row);
        ccord_draw_rrect(r, row, uis_tokens_of(ui).r_control, COL(bg_tertiary));
        uis_list_row_fill(ui, r, row, (uis_state){.hover = hov}, "esnd", snd->id);
        ui_icon_speaker(r, row.x + 20.0f, row.y + row.h * 0.5f, 16.0f,
                        can_play ? COL(status_online) : COL(text_muted));
        ccord_text_style ns2 = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
        const char *sname = snd->name ? snd->name : "Sound";
        ccord_text_draw_ellipsis(r, sname, strlen(sname),
                                 row.x + 38.0f, row.y + (row.h - ui_line_h(ui, ns2.size)) * 0.5f, gw - 110.0f, &ns2);
        // The Play pill is the shared primary button at the small height, dimmed while this user cannot play.
        ccord_rect play = {row.x + row.w - UIS_SP_2 - play_w, row.y + (row.h - UIS_H_BTN_SM) * 0.5f, play_w, UIS_H_BTN_SM};
        bool play_hov = ui->mouse_in && ui_in_rect(ui->mx, ui->my, play);
        uis_button(ui, r, play, "Play", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
                   (uis_state){.hover = play_hov, .disabled = !can_play}, "esndplay", snd->id);
      }
    }
    gy += 46.0f;
  }
  if (shown == 0) {
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
    const char *msg = query[0] ? "No sounds match your search." : "No soundboard sounds in this server.";
    ccord_text_draw(r, msg, strlen(msg), gx, gy, &es, gw);
    gy += 24.0f;
  }
  b->content_h[EXPR_TAB_SOUNDBOARD] = ep_maxf((gy + scroll) - body.y + 12.0f, body.h);
  ccord_pop_clip(r);
}

