// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include "ui_interaction_harness.h"

static void test_expr_picker_fetch_and_emoji_rail(void)
{
  ccord_error err = {0};
  ui_menu_close(F.ui);
  const ccord_id g1 = 2001;
  ccord_id ch = find_channel(g1, "off-topic", CCORD_CHANNEL_TEXT);
  CT_CHECK(ch != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, ch, &err) == CCORD_OK);
  frame(2);
  ui_expr_picker_close(F.ui);

  // ---- 1. SETTINGS_PROTO is requested even when local frequent emojis exist (no one-shot latch) ----
  ccord_store_seed_emoji_frecency(F.store, "fire", 99);
  CT_CHECK(ccord_store_frequent_emoji_count(F.store) > 0);
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_FAILED);
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);   // 2 = Emoji tab
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_PENDING);
  frame(3);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY);

  // ---- 2. FAILED blobs retry after the >=2000ms cooldown, never per frame ----
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  draw_only();
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_FAILED);
  F.now += 2100;
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_PENDING);
  frame(3);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY);
  // The GIF catalog blobs recover the same way.
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_GIF_TRENDING, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_expr_blob(F.store, CCORD_EXPR_GIF_CATEGORIES, nullptr, nullptr, 0, false, &err) == CCORD_OK);
  F.now += 2100;
  draw_only();
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_TRENDING, nullptr) == CCORD_EXPR_BLOB_PENDING);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_CATEGORIES, nullptr) == CCORD_EXPR_BLOB_PENDING);
  frame(3);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_TRENDING, nullptr) == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_expr_blob_state(F.store, CCORD_EXPR_GIF_CATEGORIES, nullptr) == CCORD_EXPR_BLOB_READY);

  // ---- 3. GIF hit cells hold full 512-byte ccord_gif_item URLs ----
  ui_expr_inspect ins;
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.hit_url_cap == CCORD_URL_MAX_BYTES + 1u && ins.hit_url_cap >= 513u);
  CT_CHECK(ins.hit_preview_url_cap == CCORD_URL_MAX_BYTES + 1u);
  ccord_gif_item lg = {0};
  snprintf(lg.url, sizeof lg.url, "https://tenor.com/view/long-url-gif-");
  size_t base = strlen(lg.url);
  memset(lg.url + base, 'x', 500 - base);
  lg.url[500] = '\0';
  snprintf(lg.src, sizeof lg.src, "%s", lg.url);
  lg.width = 320; lg.height = 200;
  CT_CHECK(ccord_store_toggle_favorite_gif(F.store, &lg));
  ui_expr_picker_close(F.ui);
  ui_expr_picker_open(F.ui, 0, ch, CCORD_ID_NONE);   // 0 = GIFs tab
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  ccord_rect fav_card = {0, 0, 0, 0};
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 4) { fav_card = hi.rect; break; }   // 4 = Favorites card
  }
  CT_CHECK(fav_card.w > 0.0f);
  click(fav_card.x + fav_card.w * 0.5f, fav_card.y + fav_card.h * 0.5f);
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.gif_subview == 1);
  bool long_url_intact = false;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 2 && strlen(hi.url) == 500 && strcmp(hi.url, lg.url) == 0) long_url_intact = true;
  }
  CT_CHECK(long_url_intact);
  // Removing every favorite draws the Favorites empty state instead of tiles (no crash, no tile hits).
  while (ccord_store_favorite_gif_count(F.store) > 0) {
    const ccord_gif_item *f0 = ccord_store_favorite_gif_at(F.store, 0);
    CT_CHECK(f0 != nullptr);
    ccord_gif_item copy = *f0;
    (void)ccord_store_toggle_favorite_gif(F.store, &copy);   // returns false when removing
  }
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  size_t tile_hits = 0;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 2) tile_hits++;
  }
  CT_CHECK(tile_hits == 0);

  // ---- 4. Every GIF category renders with its cover art (no 4-tile cap) ----
  ui_expr_picker_close(F.ui);
  ui_expr_picker_open(F.ui, 0, ch, CCORD_ID_NONE);
  frame(2);
  shot("picker_gifs_categories");
  size_t ncat = ccord_store_gif_category_count(F.store);
  CT_CHECK(ncat >= 8);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  size_t cat_hits = 0;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind == 3) cat_hits++;   // 3 = GIF category tile
  }
  CT_CHECK(cat_hits == ncat);
  // Every category cover went through ui_ensure_media: after pumping, each has a texture. (The mock's
  // disk media cache survives runs, so a texture check is used instead of the request log.)
  ui_pump_media(F.ui, F.renderer);
  frame(2);
  ui_pump_media(F.ui, F.renderer);
  for (size_t i = 0; i < ncat; i++) {
    const ccord_gif_category *cat = ccord_store_gif_category_at(F.store, i);
    CT_CHECK(cat && cat->src[0]);
    CT_CHECK(ccord_render_has_texture(F.renderer, cat->src, nullptr, nullptr));
  }

  // ---- 5. Emoji rail: server icons, >=256 capacity, independent scroll, pinned Unicode button ----
  // Fill the store to its guild cap; each new guild gets one custom emoji so it earns a rail button.
  // The last two guilds are special: one has a unique icon URL the nav rail never drew (it culls
  // off-screen slots), one has no icon at all and must fall back to initials.
  size_t ng0 = ccord_store_guild_count(F.store);
  CT_CHECK(ng0 < CCORD_MAX_REAL_GUILDS);
  F.ui->server_scroll = 0.0f;      // nav rail at the top: the new guilds stay off-screen there
  F.ui->server_scroll_vel = 0.0f;
  ccord_id icon_gid = 0, plain_gid = 0;
  char namebuf[40];
  for (size_t i = ng0; i < CCORD_MAX_REAL_GUILDS; i++) {
    ccord_id gid = 900000 + (ccord_id)i;
    ccord_guild ng = {0};
    ng.id = gid;
    snprintf(namebuf, sizeof namebuf, "Rail Guild %zu", i);
    ng.name = namebuf;
    ng.icon_url = "";
    if (i == CCORD_MAX_REAL_GUILDS - 2) {
      icon_gid = gid;
      ng.icon_url = "https://cdn.discordapp.com/icons/999001/exprail.png";
    } else if (i == CCORD_MAX_REAL_GUILDS - 1) {
      plain_gid = gid;
    }
    CT_CHECK(ccord_store_upsert_guild(F.store, &ng, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
    ccord_emoji ne = {0};
    ne.id = 950000 + (ccord_id)i;
    ne.guild_id = gid;
    snprintf(namebuf, sizeof namebuf, "rail%zu", i);
    ne.name = namebuf;
    ne.available = true;
    CT_CHECK(ccord_store_set_guild_emojis(F.store, gid, &ne, 1, &err) == CCORD_OK);
  }
  CT_CHECK(icon_gid != 0 && plain_gid != 0);
  CT_CHECK(!ccord_render_has_texture(F.renderer, "https://cdn.discordapp.com/icons/999001/exprail.png", nullptr, nullptr));

  ui_expr_picker_close(F.ui);
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.tab == 2);
  CT_CHECK(ins.rail_cap >= 256);
  CT_CHECK(ins.nrail > 200);                          // the old 24 cap would clamp this
  CT_CHECK(ins.rail_view.h > 0.0f && ins.rail_view.h < ins.rail_rect.h);
  CT_CHECK(fabsf((ins.rail_rect.h - ins.rail_view.h) - 36.0f) < 0.01f);
  CT_CHECK(ins.rail_content_h > ins.rail_view.h);
  CT_CHECK(ins.rail_pinned_rect.w == 28.0f && ins.rail_pinned_rect.h == 28.0f);
  CT_CHECK(ins.rail_pinned_rect.y >= ins.rail_view.y + ins.rail_view.h);
  CT_CHECK(ins.rail_pinned_rect.y + 28.0f <= ins.rail_rect.y + ins.rail_rect.h + 0.01f);

  size_t unicode_idx = (size_t)-1, icon_idx = (size_t)-1, plain_idx = (size_t)-1;
  for (size_t i = 0; i < ins.nrail; i++) {
    ui_expr_rail_info ri;
    CT_CHECK(ui_expr_rail_at(F.ui, i, &ri));
    if (ri.section_id == 0 && unicode_idx == (size_t)-1) unicode_idx = i;
    if (ri.guild_id == icon_gid) icon_idx = i;
    if (ri.guild_id == plain_gid) plain_idx = i;
  }
  CT_CHECK(unicode_idx != (size_t)-1 && icon_idx != (size_t)-1 && plain_idx != (size_t)-1);

  // Wheel over the 40px rail scrolls ONLY the rail; the emoji grid stays put.
  ui_expr_rail_info icon_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, icon_idx, &icon_ri));
  float grid_before = ins.grid_scroll_y;
  float need = icon_ri.rect.y - ins.rail_view.y - 4.0f;   // rail scroll that brings the icon button into view
  CT_CHECK(need > 0.0f);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -need / 105.0f,
                           .x = ins.rail_rect.x + 20.0f, .y = ins.rail_view.y + 40.0f});
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(fabsf(ins.rail_scroll_y - need) < 1.0f);
  CT_CHECK(ins.grid_scroll_y == grid_before);
  CT_CHECK(ui_expr_rail_at(F.ui, icon_idx, &icon_ri));
  CT_CHECK(icon_ri.rect.y >= ins.rail_view.y && icon_ri.rect.y + 28.0f <= ins.rail_view.y + ins.rail_view.h);
  // The picker pulled the server icon through ui_ensure_media (initials fallback until bytes arrive)...
  frame(2);
  ui_pump_media(F.ui, F.renderer);
  frame(1);
  CT_CHECK(ccord_render_has_texture(F.renderer, "https://cdn.discordapp.com/icons/999001/exprail.png", nullptr, nullptr));
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ui_expr_rail_at(F.ui, icon_idx, &icon_ri));
  size_t icons_drawn_before = F.ui->dbg_expr_rail_icons_drawn;
  // ...and draws it once a real texture is uploaded.
  static const uint8_t k_rail_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
    0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
    0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
    0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
    0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
    0x02, 0x00, 0x3b,
  };
  CT_CHECK(ccord_render_upload_image(F.renderer, "https://cdn.discordapp.com/icons/999001/exprail.png",
                                     k_rail_gif, sizeof k_rail_gif, nullptr, nullptr, &err) == CCORD_OK);
  draw_only();
  CT_CHECK(F.ui->dbg_expr_rail_icons_drawn > icons_drawn_before);
  // The icon-less guild button is visible next to it (initials fallback, no texture request).
  ui_expr_rail_info plain_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, plain_idx, &plain_ri));
  CT_CHECK(plain_ri.rect.y >= ins.rail_view.y - 28.0f && plain_ri.rect.y <= ins.rail_view.y + ins.rail_view.h);
  shot("picker_emoji_rail_icons");

  // No ghost clicks: item 0 scrolled far out of the view; clicking the visible icon button jumps the
  // grid to THAT section, not to item 0's target.
  ui_expr_rail_info first_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, 0, &first_ri));
  CT_CHECK(first_ri.rect.y + first_ri.rect.h < ins.rail_view.y);
  click(icon_ri.rect.x + 14.0f, icon_ri.rect.y + 14.0f);
  frame(1);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_scroll_y == icon_ri.target_scroll_y && ins.grid_scroll_y > 0.0f);

  // Wheel over the grid scrolls ONLY the grid; the rail stays put.
  float rail_before = ins.rail_scroll_y;
  ccord_rect pbox = F.ui->emoji_picker_rect;
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = 3.0f,
                           .x = pbox.x + 150.0f, .y = pbox.y + 200.0f});
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_scroll_y < icon_ri.target_scroll_y);
  CT_CHECK(ins.rail_scroll_y == rail_before);

  // ---- 6. Pinned Default Unicode button: visible during search, clears it and jumps to Unicode ----
  ccord_rect pinned = ins.rail_pinned_rect;
  type_text("zz");
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 2 && ins.nrail == 0);
  CT_CHECK(ins.rail_pinned_rect.x == pinned.x && ins.rail_pinned_rect.y == pinned.y &&
           ins.rail_pinned_rect.w == 28.0f && ins.rail_pinned_rect.h == 28.0f);
  // P1 regression: reduced motion snaps every fade, so no animation schedules a follow-up frame.
  // The jump resolves mid-draw (after the grid was drawn at the old scroll) and must mark the UI
  // dirty itself. Raw events + a hand-run app loop: click() would force frames and mask a stall.
  for (int i = 0; i < 128; i++) ui_pump_media(F.ui, F.renderer);   // drain uploads so media can't mask dirty
  bool rm_setting = F.ui->settings_reduced_motion, rm_anim = F.ui->anim.reduced_motion;
  F.ui->settings_reduced_motion = true;
  F.ui->anim.reduced_motion = true;
  mouse(CCORD_IN_MOUSE_MOVE, 0, pinned.x + 14.0f, pinned.y + 14.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 1, pinned.x + 14.0f, pinned.y + 14.0f);
  mouse(CCORD_IN_MOUSE_UP, 1, pinned.x + 14.0f, pinned.y + 14.0f);
  F.now += 50;
  CT_CHECK(ccord_ui_update(F.ui, F.now));   // the click asked for a redraw
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);          // this draw resolves the pending Unicode jump
  ccord_render_end(F.renderer);
  CT_CHECK(F.ui->dirty);                    // P1: the jump must schedule its own follow-up frame
  F.now += 50;
  CT_CHECK(ccord_ui_update(F.ui, F.now));   // ...and the app loop honors it
  ccord_render_begin(F.renderer, F.theme.c.bg_primary);
  ccord_ui_draw(F.ui, F.renderer);
  ccord_render_end(F.renderer);
  F.ui->settings_reduced_motion = rm_setting;
  F.ui->anim.reduced_motion = rm_anim;
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 0);
  ui_expr_rail_info uni_ri;
  CT_CHECK(ui_expr_rail_at(F.ui, unicode_idx, &uni_ri));
  CT_CHECK(ins.grid_scroll_y == uni_ri.target_scroll_y && ins.grid_scroll_y > 0.0f);
  // The Unicode category button was brought into the rail view as well.
  CT_CHECK(uni_ri.rect.y >= ins.rail_view.y && uni_ri.rect.y + 28.0f <= ins.rail_view.y + ins.rail_view.h);
  shot("picker_emoji_pinned");

  // ---- 7. After the jump, visible Unicode emojis own the hit slots (not 160 off-screen cells) ----
  CT_CHECK(ins.grid_view.w > 0.0f && ins.grid_view.h > 0.0f);
  CT_CHECK(ins.nhits > 0);
  size_t uni_visible = 0;
  ccord_rect uni_cell = {0, 0, 0, 0};
  char uni_utf8[32] = {0};
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    // Every recorded hit intersects the grid viewport: nothing off-screen earns a slot.
    CT_CHECK(hi.rect.y + hi.rect.h >= ins.grid_view.y && hi.rect.y <= ins.grid_view.y + ins.grid_view.h);
    CT_CHECK(hi.rect.x >= ins.grid_view.x &&
             hi.rect.x + hi.rect.w <= ins.grid_view.x + ins.grid_view.w + 0.01f);
    if (hi.kind == 0 && hi.rect.y >= ins.grid_view.y &&
        hi.rect.y + hi.rect.h <= ins.grid_view.y + ins.grid_view.h) {
      uni_visible++;
      if (uni_cell.w == 0.0f) {
        uni_cell = hi.rect;
        snprintf(uni_utf8, sizeof uni_utf8, "%s", hi.utf8);
      }
    }
  }
  CT_CHECK(uni_visible >= 8);
  CT_CHECK(uni_utf8[0]);
  // Clicking a visible Unicode emoji inserts exactly it and closes the picker.
  size_t draft_len_before = strlen(ccord_text_edit_cstr(&F.ui->edit));
  click(uni_cell.x + uni_cell.w * 0.5f, uni_cell.y + uni_cell.h * 0.5f);
  CT_CHECK(!F.ui->emoji_picker_open);
  const char *draft_after = ccord_text_edit_cstr(&F.ui->edit);
  CT_CHECK(strlen(draft_after) > draft_len_before);
  CT_CHECK(strstr(draft_after, uni_utf8) != nullptr);

  // ---- 8. No ghost clicks through the preview bar onto partially visible cells ----
  ccord_text_edit_clear(&F.ui->edit);
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);
  frame(2);   // grid scroll persists across close/open: still at the Unicode section
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_scroll_y == uni_ri.target_scroll_y);
  // Find a cell straddling the grid's bottom edge; nudge the scroll until one exists.
  ccord_rect straddle = {0, 0, 0, 0};
  for (int attempt = 0; attempt < 12 && straddle.w == 0.0f; attempt++) {
    if (attempt > 0) {
      ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -0.1f,
                               .x = ins.grid_view.x + 100.0f, .y = ins.grid_view.y + 100.0f});
      draw_only();
      CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
    }
    for (size_t i = 0; i < ins.nhits; i++) {
      ui_expr_hit_info hi;
      CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
      if (hi.kind == 0 && hi.rect.y < ins.grid_view.y + ins.grid_view.h &&
          hi.rect.y + hi.rect.h > ins.grid_view.y + ins.grid_view.h) {
        straddle = hi.rect;
        break;
      }
    }
  }
  CT_CHECK(straddle.w > 0.0f);
  float grid_bottom = ins.grid_view.y + ins.grid_view.h;
  float overhang = straddle.y + straddle.h - grid_bottom;
  float ghost_y = grid_bottom + fminf(6.0f, overhang * 0.5f);   // inside the preview bar AND the cell rect
  mouse(CCORD_IN_MOUSE_MOVE, 0, straddle.x + 21.0f, ghost_y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, straddle.x + 21.0f, ghost_y);
  mouse(CCORD_IN_MOUSE_UP, 1, straddle.x + 21.0f, ghost_y);
  CT_CHECK(F.ui->emoji_picker_open);                            // the clipped part is not clickable
  CT_CHECK(ccord_text_edit_cstr(&F.ui->edit)[0] == '\0');
  // The visible part of the same cell is still live.
  click(straddle.x + 21.0f, straddle.y + fminf(20.0f, (grid_bottom - straddle.y) * 0.5f));
  CT_CHECK(!F.ui->emoji_picker_open);
  CT_CHECK(ccord_text_edit_cstr(&F.ui->edit)[0] != '\0');
  frame(1);

  // ---- 9. Search caret/selection, grid scrollbar thumb, and width-adaptive Emoji columns ----
  ui_expr_picker_open(F.ui, 2, ch, CCORD_ID_NONE);
  frame(2);
  F.ui->caret_epoch_ms = F.now;   // caret visible phase
  type_text("fi");
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 2);
  CT_CHECK(ins.search_caret_rect.w > 0.0f && ins.search_caret_rect.h > 0.0f);
  CT_CHECK(ins.search_caret_rect.x >= ins.search_rect.x &&
           ins.search_caret_rect.x + ins.search_caret_rect.w <= ins.search_rect.x + ins.search_rect.w + 0.01f);
  CT_CHECK(ins.search_caret_rect.y >= ins.search_rect.y &&
           ins.search_caret_rect.y + ins.search_caret_rect.h <= ins.search_rect.y + ins.search_rect.h + 0.01f);
  key(CCORD_KEY_LEFT, CCORD_MOD_SHIFT);
  draw_only();
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_selection_rect.w > 0.0f);
  CT_CHECK(ins.search_selection_rect.x >= ins.search_rect.x &&
           ins.search_selection_rect.x + ins.search_selection_rect.w <= ins.search_rect.x + ins.search_rect.w + 0.01f);
  // Clear the query: the full Emoji grid overflows and shows its thumb at the right edge of grid_view.
  key(CCORD_KEY_A, CCORD_MOD_CTRL);
  key(CCORD_KEY_BACKSPACE, 0);
  frame(1);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.search_len == 0);
  CT_CHECK(ins.grid_content_h > ins.grid_view.h);
  CT_CHECK(ins.grid_scrollbar_rect.w == 4.0f && ins.grid_scrollbar_rect.h > 0.0f);
  CT_CHECK(ins.grid_scrollbar_rect.x >= ins.grid_view.x &&
           ins.grid_scrollbar_rect.x + ins.grid_scrollbar_rect.w <= ins.grid_view.x + ins.grid_view.w + 0.01f);
  CT_CHECK(ins.grid_scrollbar_rect.y >= ins.grid_view.y &&
           ins.grid_scrollbar_rect.y + ins.grid_scrollbar_rect.h <= ins.grid_view.y + ins.grid_view.h + 0.01f);

  // A 280 px window narrows the picker: fewer than 8 columns, and every recorded cell stays inside grid_view.
  ui_send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 280.0f, .y = 800.0f});
  frame(2);
  CT_CHECK(ui_expr_get_inspect(F.ui, &ins));
  CT_CHECK(ins.grid_view.w > 0.0f);
  float col_x[16];
  size_t ncol = 0, cells = 0;
  for (size_t i = 0; i < ins.nhits; i++) {
    ui_expr_hit_info hi;
    CT_CHECK(ui_expr_hit_at(F.ui, i, &hi));
    if (hi.kind != 0 && hi.kind != 1) continue;
    cells++;
    CT_CHECK(hi.rect.x >= ins.grid_view.x - 0.01f &&
             hi.rect.x + hi.rect.w <= ins.grid_view.x + ins.grid_view.w + 0.01f);
    bool seen = false;
    for (size_t c = 0; c < ncol; c++) {
      if (fabsf(col_x[c] - hi.rect.x) < 0.5f) seen = true;
    }
    if (!seen && ncol < 16) col_x[ncol++] = hi.rect.x;
  }
  CT_CHECK(cells > 0);
  CT_CHECK(ncol >= 4 && ncol < 8);
  ui_send((ccord_input_event){.type = CCORD_IN_RESIZE, .x = 1280.0f, .y = 800.0f});
  frame(2);
  ui_expr_picker_close(F.ui);
  frame(1);
}

// ---- Regressions: search timeline, scrollable Friends/stage, DM call bar inset/cursor ----

static ui_voice_surfaces_debug vsdbg(void)
{
  ui_voice_surfaces_debug d;
  ui_voice_surfaces_get_debug(F.ui, &d);
  return d;
}

// An active search keeps every channel message in the timeline while the results panel populates.
static void test_search_preserves_timeline(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_session_send_message(F.session, 3111, "quokka-unique-search-token", 26, &err) == CCORD_OK);
  frame(4);
  size_t total = ccord_store_message_count(F.store, 3111);

  click_rect(dbg().search_btn);
  frame(2);
  CT_CHECK(dbg().search_open);
  type_text("quokka-unique-search-token");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.message_count == total);          // the timeline is not filtered away
  CT_CHECK(F.ui->view_count == total);
  CT_CHECK(d.search_panel_open && d.search_panel_cards > 0);
  CT_CHECK(d.search_matches >= 1 && d.search_matches < total);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().search_open);
  CT_CHECK(dbg().message_count == total);
}

// The DM Friends list scrolls under its fixed header with a fading thumb; off-fold rows stay reachable.
static void test_dm_friends_scroll_thumb(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(2);
  const size_t NF = 40;
  for (size_t i = 0; i < NF; i++) {
    ccord_user u = {.id = 60000u + (ccord_id)i, .name = "ScrollFriend", .presence = CCORD_PRESENCE_ONLINE};
    CT_CHECK(ccord_store_upsert_user(F.store, &u, &err) == CCORD_OK);
    CT_CHECK(ccord_store_set_relationship(F.store, 60000u + (ccord_id)i, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  }
  F.ui->dm_friends_view = true;
  F.ui->dm_friends_tab = 1;   // All Friends
  F.ui->dm_friends_scroll = 0.0f;
  F.ui->dirty = true;
  frame(2);
  CT_CHECK(ui_is_dm_friends_view(F.ui));
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  CT_CHECK(F.ui->dm_friends_content_h > ma.h);   // enough rows to overflow the fold
  CT_CHECK(F.ui->dm_friends_scroll == 0.0f);

  float wx = ma.x + ma.w * 0.5f, wy = ma.y + 100.0f;
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -5.0f, .x = wx, .y = wy});
  frame(3);   // let the 160ms thumb fade ramp in
  CT_CHECK(F.ui->dm_friends_scroll > 0.0f);
  CT_CHECK(F.ui->dm_friends_thumb.w > 0.0f);     // thumb appears right after scrolling
  CT_CHECK(F.ui->dm_friends_thumb.y >= ma.y &&
           F.ui->dm_friends_thumb.y + F.ui->dm_friends_thumb.h <= ma.y + ma.h + 1.0f);

  // Scroll to the bottom: the last friend becomes fully visible and clickable.
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = wx, .y = wy});
  frame(1);
  bool found_last = false;
  for (size_t i = 0; i < F.ui->nfriend_row_hits; i++) {
    if (F.ui->friend_row_hits[i].user_id == 60000u + NF - 1) found_last = true;
  }
  CT_CHECK(found_last);

  F.ui->dm_friends_view = false;
  F.ui->dirty = true;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
}

// Voice/stage participant tiles wrap and scroll as one viewport; off-fold tiles become reachable.
static void test_stage_tiles_scroll_thumb(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
  ccord_id voice = find_channel(2001, "General", CCORD_CHANNEL_VOICE);
  CT_CHECK(voice != CCORD_ID_NONE);
  CT_CHECK(ccord_session_join_voice(F.session, 2001, voice, &err) == CCORD_OK);
  frame(3);
  F.ui->stage_channel = voice;
  F.ui->stage_scroll = 0.0f;
  F.ui->dirty = true;
  frame(2);
  const size_t NU = 18;
  for (size_t i = 0; i < NU; i++) {
    ccord_id uid = 61000u + (ccord_id)i;
    ccord_user u = {.id = uid, .name = "StageUser", .presence = CCORD_PRESENCE_ONLINE};
    CT_CHECK(ccord_store_upsert_user(F.store, &u, &err) == CCORD_OK);
    ccord_voice_state vs = {.guild_id = 2001, .channel_id = voice, .user_id = uid};
    CT_CHECK(ccord_store_set_voice_state(F.store, &vs, &err) == CCORD_OK);
  }
  frame(2);
  CT_CHECK(F.ui->stage_content_h > F.ui->stage_view_h);   // the tile grid overflows the fold
  ccord_id last = 61000u + NU - 1;
  ui_voice_surfaces_debug d = vsdbg();
  bool last_before = false;
  for (size_t i = 0; i < d.stage.ctl_count; i++) if (d.stage.ctl[i].user_id == last) last_before = true;
  CT_CHECK(!last_before);   // the bottom tile starts off-fold

  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  float wx = ma.x + ma.w * 0.5f, wy = ma.y + ma.h * 0.7f;
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -5.0f, .x = wx, .y = wy});
  frame(3);   // let the 160ms thumb fade ramp in
  CT_CHECK(F.ui->stage_scroll > 0.0f);
  CT_CHECK(F.ui->stage_thumb.w > 0.0f);
  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .wheel = -2000.0f, .x = wx, .y = wy});
  frame(1);
  d = vsdbg();
  bool last_after = false;
  for (size_t i = 0; i < d.stage.ctl_count; i++) if (d.stage.ctl[i].user_id == last) last_after = true;
  CT_CHECK(last_after);   // scrolled into view: its controls are now exposed

  CT_CHECK(ccord_session_leave_voice(F.session, 2001, &err) == CCORD_OK);
  frame(3);
  F.ui->stage_channel = CCORD_ID_NONE;
  F.ui->dirty = true;
  frame(2);
}

// The active DM call bar reserves a 36px strip above the message area: the timeline starts below it,
// the pointer over the bar is never a text cursor, and pressing the bar never starts a chat selection.
static void test_call_bar_inset_and_cursor(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  frame(2);
  ccord_id call_ch = 0;
  CT_CHECK(ccord_mock_push_incoming_call(F.transport, 1002u, &call_ch, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ccord_session_select_channel(F.session, call_ch, &err) == CCORD_OK);
  frame(2);
  ccord_rect ma_before = ui_rect(F.ui->layout.message_area);
  CT_CHECK(!ui_call_bar_active(F.ui));

  ui_voice_surfaces_debug d = vsdbg();
  CT_CHECK(d.ring_visible && d.ring_accept.w > 0.0f);
  click(d.ring_accept.x + d.ring_accept.w * 0.5f, d.ring_accept.y + d.ring_accept.h * 0.5f);
  frame(3);
  CT_CHECK(ui_call_bar_active(F.ui));
  ccord_rect ma = ui_rect(F.ui->layout.message_area);
  CT_CHECK(fabsf(ma.y - (ma_before.y + UI_CALL_BAR_H)) < 0.5f);
  CT_CHECK(fabsf(ma.h - (ma_before.h - UI_CALL_BAR_H)) < 0.5f);
  ccord_rect bar = ui_call_bar_rect(F.ui);
  CT_CHECK(bar.h == UI_CALL_BAR_H && bar.y + bar.h <= ma.y + 0.5f);

  // Over the bar the cursor is a button pointer, never the text I-beam.
  float bx = bar.x + 60.0f, by = bar.y + bar.h * 0.5f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, bx, by);
  frame(1);
  CT_CHECK(F.ui->cursor != CCORD_CURSOR_TEXT);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_POINTER);

  // In the timeline below, the I-beam shows over a line of message text and nowhere else: not in the empty
  // space above the messages, and not in the gap to the right of where a line ends.
  ccord_id cur_mid = append_rich_message(call_ch, "cursor probe", nullptr);
  ccord_rect line = {0, 0, 0, 0};
  for (size_t i = 0; i < F.ui->ntext_hits; i++) {
    ccord_rect t = F.ui->text_hits[i];
    if (t.w > 20.0f && t.y > ma.y && t.y + t.h < ma.y + ma.h && t.y > line.y) line = t;   // the lowest line
  }
  CT_CHECK(F.ui->ntext_hits > 0 && line.w > 20.0f && line.w < ma.w * 0.5f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, line.x + line.w * 0.5f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_TEXT);
  mouse(CCORD_IN_MOUSE_MOVE, 0, line.x + line.w + 60.0f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->cursor != CCORD_CURSOR_TEXT);
  mouse(CCORD_IN_MOUSE_MOVE, 0, ma.x + ma.w * 0.5f, ma.y + 20.0f);
  frame(1);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_DEFAULT);
  // Dragging a selection keeps the I-beam even where the pointer leaves the text.
  mouse(CCORD_IN_MOUSE_DOWN, 1, line.x + 2.0f, line.y + line.h * 0.5f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, line.x + line.w + 60.0f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->drag == DRAG_CHAT_SELECT && F.ui->cursor == CCORD_CURSOR_TEXT);
  mouse(CCORD_IN_MOUSE_UP, 1, line.x + line.w + 60.0f, line.y + line.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->cursor != CCORD_CURSOR_TEXT);
  key(CCORD_KEY_ESCAPE, 0);
  remove_message(call_ch, cur_mid);
  frame(1);

  // Pressing on the bar must not begin a chat text selection.
  mouse(CCORD_IN_MOUSE_DOWN, 1, bx, by);
  CT_CHECK(F.ui->drag != DRAG_CHAT_SELECT);
  mouse(CCORD_IN_MOUSE_UP, 1, bx, by);
  frame(1);

  // Leaving removes the inset entirely.
  d = vsdbg();
  CT_CHECK(d.call_bar_visible && d.call_bar_leave.w > 0.0f);
  click(d.call_bar_leave.x + d.call_bar_leave.w * 0.5f, d.call_bar_leave.y + d.call_bar_leave.h * 0.5f);
  frame(3);
  CT_CHECK(!ui_call_bar_active(F.ui));
  CT_CHECK(fabsf(ui_rect(F.ui->layout.message_area).y - ma_before.y) < 0.5f);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  frame(2);
}

// ---- Result 1: messaging parity (Forward / Crosspost / Copy Link / Mark Unread / Save), /tts, Saved Messages
//      popover + persistence, slash subcommand trees, and USER/MESSAGE context-menu application commands. ----

// Open a context menu for a known context and click one labelled row; returns false if the row is absent.
static bool open_menu_and_click(const ccord_menu_context *ctx, const char *label)
{
  ui_menu_open(F.ui, 420.0f, 300.0f, ctx);
  frame(1);
  if (!F.ui->menu.open) return false;
  return menu_click_label(label);
}

static ccord_id send_confirmed(ccord_id ch, const char *text)
{
  clear_composer_input();
  type_text(text);
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  size_t n = ccord_store_message_count(F.store, ch);
  const ccord_message *m = n ? ccord_store_message_at(F.store, ch, n - 1) : nullptr;
  CT_CHECK(m && m->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(m->content, text) == 0);
  return m ? m->id : CCORD_ID_NONE;
}

static void test_copy_link_mark_unread_save(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(3111, "parity target message");
  CT_CHECK(msg != CCORD_ID_NONE);
  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                            .message_id = msg, .user_id = self};

  // Copy Message Link -> discord.com/channels/<guild>/<channel>/<message>
  F.clip[0] = '\0';
  CT_CHECK(open_menu_and_click(&ctx, "Copy Message Link"));
  frame(1);
  char want_link[128];
  snprintf(want_link, sizeof want_link, "https://discord.com/channels/2001/3111/%llu", (unsigned long long)msg);
  CT_CHECK(strcmp(F.clip, want_link) == 0);

  // Mark Unread moves the read position back; the channel reports unread again.
  size_t acks0 = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK);
  CT_CHECK(open_menu_and_click(&ctx, "Mark Unread"));
  frame(1);
  const ccord_channel *c = ccord_store_channel(F.store, 3111);
  CT_CHECK(c && c->unread_count >= 1 && c->last_read_id < msg);
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) == acks0 + 1);

  // Save Message bookmarks it; the menu then offers Unsave.
  CT_CHECK(!ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(open_menu_and_click(&ctx, "Save Message"));
  frame(1);
  CT_CHECK(ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(ccord_store_message_saved(F.store, 3111, msg));
  CT_CHECK(F.ui->nsaved_msgs == 1);
  ui_menu_open(F.ui, 420.0f, 300.0f, &ctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Unsave Message", nullptr));
  CT_CHECK(!ccord_ui_context_menu_find(F.ui, "Save Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(open_menu_and_click(&ctx, "Unsave Message"));
  frame(1);
  CT_CHECK(!ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(!ccord_store_message_saved(F.store, 3111, msg));
  CT_CHECK(F.ui->nsaved_msgs == 0);
}

static void test_saved_messages_popover_and_persistence(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  ccord_id msg = send_confirmed(3111, "bookmark me");
  CT_CHECK(msg != CCORD_ID_NONE);
  ui_saved_messages_save(F.ui, 3111, msg);
  frame(1);
  CT_CHECK(F.ui->nsaved_msgs == 1);

  // Move to another channel, then open the header popover and jump back.
  ccord_id general = find_channel(2001, "general", CCORD_CHANNEL_TEXT);
  CT_CHECK(general != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(3);
  ui_saved_msgs_debug sd;
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(!sd.open && sd.header_btn.w > 0.0f);
  click(sd.header_btn.x + sd.header_btn.w * 0.5f, sd.header_btn.y + sd.header_btn.h * 0.5f);
  frame(1);
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(sd.open && sd.count == 1 && sd.hit_count >= 1);
  CT_CHECK(sd.hits[0].channel_id == 3111 && sd.hits[0].message_id == msg);
  CT_CHECK(sd.popover_rect.w > 0.0f && sd.hits[0].jump.w > 0.0f);
  click(sd.hits[0].jump.x + sd.hits[0].jump.w * 0.5f, sd.hits[0].jump.y + sd.hits[0].jump.h * 0.5f);
  frame(3);
  CT_CHECK(ccord_store_selected_channel(F.store) == 3111);
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(!sd.open);

  // Remove from the popover clears the bookmark.
  ui_saved_messages_get_debug(F.ui, &sd);
  click(sd.header_btn.x + sd.header_btn.w * 0.5f, sd.header_btn.y + sd.header_btn.h * 0.5f);
  frame(1);
  ui_saved_messages_get_debug(F.ui, &sd);
  CT_CHECK(sd.open && sd.hit_count >= 1 && sd.hits[0].remove.w > 0.0f);
  click(sd.hits[0].remove.x + sd.hits[0].remove.w * 0.5f, sd.hits[0].remove.y + sd.hits[0].remove.h * 0.5f);
  frame(1);
  CT_CHECK(F.ui->nsaved_msgs == 0 && !ui_saved_messages_contains(F.ui, 3111, msg));
  key(CCORD_KEY_ESCAPE, 0);

  // Persistence: save -> settings.ini on disk -> reload restores the list and the store.
  ui_saved_messages_save(F.ui, 3111, msg);
  frame(1);
  ui_settings_save(F.ui);
  FILE *ini = fopen(ui_test_file("settings.ini"), "rb");
  CT_CHECK(ini != nullptr);
  if (ini) {
    char buf[8192];
    size_t got = fread(buf, 1, sizeof buf - 1, ini);
    buf[got] = '\0';
    fclose(ini);
    CT_CHECK(strstr(buf, "[saved_messages]") != nullptr);
    char idstr[32];
    snprintf(idstr, sizeof idstr, "%llu", (unsigned long long)msg);
    CT_CHECK(strstr(buf, idstr) != nullptr);
  }
  F.ui->nsaved_msgs = 0;   // simulate a fresh process: drop the in-memory list, then reload from disk
  ui_settings_init(F.ui);
  frame(1);
  CT_CHECK(F.ui->nsaved_msgs == 1 && ui_saved_messages_contains(F.ui, 3111, msg));
  CT_CHECK(ccord_store_message_saved(F.store, 3111, msg));
  ui_saved_messages_unsave(F.ui, 3111, msg);   // clean up for later tests
  ui_settings_save(F.ui);
  frame(1);
}

static void test_forward_picker(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(3111, "forward this note");
  CT_CHECK(msg != CCORD_ID_NONE);
  ccord_id dest = find_channel(2001, "off-topic", CCORD_CHANNEL_TEXT);
  CT_CHECK(dest != CCORD_ID_NONE);
  size_t dest_before = ccord_store_message_count(F.store, dest);

  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                            .message_id = msg, .user_id = self};
  CT_CHECK(open_menu_and_click(&ctx, "Forward"));
  frame(1);
  CT_CHECK(ui_forward_picker_active(F.ui));
  ui_forward_picker_debug fd;
  ui_forward_picker_get_debug(F.ui, &fd);
  CT_CHECK(fd.open && fd.source_channel == 3111 && fd.source_message == msg && fd.item_count > 0);

  // Filter to the destination channel and confirm with Enter.
  type_text("off-topic");
  frame(1);
  ui_forward_picker_get_debug(F.ui, &fd);
  CT_CHECK(fd.item_count >= 1);
  bool found = false;
  for (size_t i = 0; i < fd.item_count; i++) if (fd.items[i].channel_id == dest) found = true;
  CT_CHECK(found);
  key(CCORD_KEY_RETURN, 0);
  frame(5);
  CT_CHECK(!ui_forward_picker_active(F.ui));
  CT_CHECK(ccord_store_selected_channel(F.store) == dest);   // forward navigates to the target
  size_t dest_after = ccord_store_message_count(F.store, dest);
  CT_CHECK(dest_after == dest_before + 1);
  const ccord_message *fm = ccord_store_message_at(F.store, dest, dest_after - 1);
  CT_CHECK(fm && fm->rich && fm->rich->reference.kind == CCORD_REF_FORWARD &&
           fm->rich->reference.message_id == msg && fm->rich->forward.present);
  CT_CHECK(strcmp(fm->rich->forward.content, "forward this note") == 0);
}

static void test_tts_command_and_badge(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);

  // /tts is offered in the slash picker alongside the application commands.
  clear_composer_input();
  type_text("/");
  frame(3);
  CT_CHECK(autocomplete_has("/tts"));
  clear_composer_input();

  type_text("/tts speak this aloud");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  size_t n = ccord_store_message_count(F.store, 3111);
  const ccord_message *m = ccord_store_message_at(F.store, 3111, n - 1);
  CT_CHECK(m && m->tts && strcmp(m->content, "speak this aloud") == 0);
  CT_CHECK(m->author_id == ccord_store_self_id(F.store));
  frame(1);
  CT_CHECK(F.ui->dbg_tts_badges >= 1);   // the TTS badge is drawn next to the author

  // Empty /tts is refused and keeps the text (Esc dismisses the slash picker so Enter sends).
  clear_composer_input();
  type_text("/tts");
  key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "/tts") == 0);
  CT_CHECK(strstr(F.ui->notice, "needs a message") != nullptr);
  clear_composer_input();
}

static void test_crosspost_announcement(void)
{
  ccord_error err;
  ccord_id ann = find_channel(2001, "announcements", CCORD_CHANNEL_TEXT);
  CT_CHECK(ann != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, ann, &err) == CCORD_OK);
  frame(4);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(ann, "publish to followers");
  CT_CHECK(msg != CCORD_ID_NONE);
  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = ann,
                            .message_id = msg, .user_id = self};

  // Own announcement message offers Publish; after crossposting the flag arrives and the row disappears.
  ui_menu_open(F.ui, 420.0f, 300.0f, &ctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "Publish Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  CT_CHECK(open_menu_and_click(&ctx, "Publish Message"));
  frame(4);
  const ccord_message *m = ccord_store_find_message(F.store, ann, msg);
  CT_CHECK(m && (ccord_message_flags(m) & CCORD_MSGFLAG_CROSSPOSTED) != 0u);
  ui_menu_open(F.ui, 420.0f, 300.0f, &ctx);
  frame(1);
  CT_CHECK(!ccord_ui_context_menu_find(F.ui, "Publish Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);

  // A normal (non-announcement) channel never offers Publish.
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(3);
  ccord_id plain = send_confirmed(3111, "not an announcement");
  ccord_menu_context pctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                             .message_id = plain, .user_id = self};
  ui_menu_open(F.ui, 420.0f, 300.0f, &pctx);
  frame(1);
  CT_CHECK(!ccord_ui_context_menu_find(F.ui, "Publish Message", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
}

static void test_slash_subcommand_tree(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  (void)ccord_session_ensure_slash_commands(F.session, 2001, 3111, F.now);
  frame(6);

  // Subcommand / group names are offered while picking; USER/MESSAGE commands are not slash entries.
  clear_composer_input();
  type_text("/");
  frame(3);
  CT_CHECK(autocomplete_has("/config") && !autocomplete_has("UserInfo") && !autocomplete_has("QuoteMessage"));
  type_text("config ");
  frame(2);
  CT_CHECK(autocomplete_has("show") && autocomplete_has("theme"));
  type_text("theme ");
  frame(2);
  CT_CHECK(autocomplete_has("set") && autocomplete_has("reset"));
  clear_composer_input();

  // /config show -> ephemeral answer.
  size_t ic = ccord_mock_interaction_count(F.transport);
  type_text("/config show");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic + 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "config show") == 0);
  const ccord_message *m = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(m && ccord_message_is_ephemeral(m) && strcmp(m->content, "Config: theme=dark, compact=false") == 0);

  // /config theme set preset:dark -> nested group + subcommand with a named option.
  clear_composer_input();
  type_text("/config theme set preset:dark");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "config theme set") == 0);
  m = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(m && strcmp(m->content, "Config theme set: dark") == 0);

  // /config theme reset -> group + subcommand, no options.
  clear_composer_input();
  type_text("/config theme reset");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "config theme reset") == 0);
  m = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(m && strcmp(m->content, "Config theme reset to default") == 0);

  // A bare tree command without a subcommand is refused client-side.
  clear_composer_input();
  ic = ccord_mock_interaction_count(F.transport);
  type_text("/config");
  key(CCORD_KEY_ESCAPE, 0);
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic);
  CT_CHECK(strstr(F.ui->notice, "requires a subcommand") != nullptr);
  clear_composer_input();
}

static void test_context_menu_app_commands(void)
{
  ccord_error err;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  (void)ccord_session_ensure_slash_commands(F.session, 2001, 3111, F.now);
  frame(6);
  ccord_id self = ccord_store_self_id(F.store);
  ccord_id msg = send_confirmed(3111, "quote me please");
  CT_CHECK(msg != CCORD_ID_NONE);

  // MESSAGE (type 3) command in the message menu runs against the message.
  ccord_menu_context mctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 2001, .channel_id = 3111,
                             .message_id = msg, .user_id = self};
  ui_menu_open(F.ui, 420.0f, 300.0f, &mctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "QuoteMessage", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  size_t ic = ccord_mock_interaction_count(F.transport);
  CT_CHECK(open_menu_and_click(&mctx, "QuoteMessage"));
  frame(4);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic + 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "QuoteMessage") == 0);
  char tbuf[32];
  snprintf(tbuf, sizeof tbuf, "%llu", (unsigned long long)msg);
  CT_CHECK(ccord_mock_last_interaction_value_count(F.transport) == 1 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 0), tbuf) == 0);
  const ccord_message *qm = ccord_store_message_at(F.store, 3111, ccord_store_message_count(F.store, 3111) - 1);
  CT_CHECK(qm && strncmp(qm->content, "Quote (", 7) == 0);

  // USER (type 2) command in the member menu runs against the member.
  ccord_menu_context uctx = {.target = CCORD_MENU_MEMBER, .guild_id = 2001, .channel_id = 3111, .user_id = 1002};
  ui_menu_open(F.ui, 420.0f, 300.0f, &uctx);
  frame(1);
  CT_CHECK(ccord_ui_context_menu_find(F.ui, "UserInfo", nullptr));
  key(CCORD_KEY_ESCAPE, 0);
  ic = ccord_mock_interaction_count(F.transport);
  CT_CHECK(open_menu_and_click(&uctx, "UserInfo"));
  frame(4);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic + 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "UserInfo") == 0);
  CT_CHECK(ccord_mock_last_interaction_value_count(F.transport) == 1 &&
           strcmp(ccord_mock_last_interaction_value(F.transport, 0), "1002") == 0);
}

// Membership screening. Joining a guild with member verification (the mock's invite "mv-test" -> "Rules Lounge") leaves
// the self member pending: the rules form opens on its own and the composer shuts. Submit stays dead until the rules
// are agreed to, then goes to Discord, and only Discord's confirmation (the pending flag clearing) opens the composer.
static ccord_id join_rules_lounge(void)
{
  ccord_error err;
  // An earlier test fills the store to its server cap; make room for the guild the invite adds.
  while (ccord_store_guild_count(F.store) >= CCORD_MAX_REAL_GUILDS) {
    const ccord_guild *filler = ccord_store_guild_at(F.store, ccord_store_guild_count(F.store) - 1u);
    CT_CHECK(filler != nullptr && filler->id != 2001 && filler->id != CCORD_DM_GUILD_ID);
    if (!filler || filler->id == 2001 || filler->id == CCORD_DM_GUILD_ID) break;
    CT_CHECK(ccord_store_delete_guild(F.store, filler->id, &err) == CCORD_OK);
  }
  ccord_admin_request req;
  CT_CHECK(ccord_admin_accept_invite(&req, "mv-test", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(F.session, &req, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_guild(F.store, 9101) != nullptr);
  ccord_id general = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 9101); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 9101, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT) {
      general = c->id;
      break;
    }
  }
  CT_CHECK(general != CCORD_ID_NONE);
  return general;
}

static void test_membership_screening_modal_and_composer_lock(void)
{
  ccord_error err;
  ccord_transport *tr = F.transport;
  ccord_ui_set_viewport(F.ui, 1280.0f, 800.0f);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ui_composer_lock_reason(F.ui) == nullptr && F.ui->mv.composer_btn.w == 0.0f);

  // 1. Joining a guild with screening: the form opens on its own, is fetched, and the composer is locked.
  ccord_id general = join_rules_lounge();
  CT_CHECK(ui_self_pending(F.ui, 9101));
  CT_CHECK(ccord_session_select_guild(F.session, 9101, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, general, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION && F.ui->mv.guild_id == 9101);
  const ccord_member_verification *form = ccord_session_member_verification(F.session, 9101);
  CT_CHECK(form != nullptr && form->field_count >= 1 && form->fields[0].field_type == CCORD_MEMBER_VERIFICATION_TERMS);
  const char *why = ui_composer_lock_reason(F.ui);
  CT_CHECK(why != nullptr && strstr(why, "rules") != nullptr);
  CT_CHECK(F.ui->mv.box.w > 0.0f && F.ui->mv.checks[0].w > 0.0f && F.ui->mv.submit.w > 0.0f);
  CT_CHECK(!F.ui->mv.submit_enabled && !F.ui->mv.agreed[0]);

  // 2. Submit is shut until the box is ticked: clicking it sends nothing and keeps the form open.
  size_t base = ccord_mock_admin_count(tr);
  click_rect(F.ui->mv.submit);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_MEMBER_VERIFICATION && ccord_mock_admin_count(tr) == base);
  CT_CHECK(ui_self_pending(F.ui, 9101));

  // 3. Ticking the box enables Submit; Submit asks Discord (PUT /guilds/{g}/requests/@me) and closes the form, but
  //    the composer stays shut until Discord confirms.
  click_rect(F.ui->mv.checks[0]);
  CT_CHECK(F.ui->mv.agreed[0] && F.ui->mv.submit_enabled);
  ccord_rect submit = F.ui->mv.submit;
  mouse(CCORD_IN_MOUSE_DOWN, 1, submit.x + submit.w * 0.5f, submit.y + submit.h * 0.5f);
  mouse(CCORD_IN_MOUSE_UP, 1, submit.x + submit.w * 0.5f, submit.y + submit.h * 0.5f);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  CT_CHECK(ccord_mock_admin_count(tr) == base + 1);
  const ccord_admin_request *last = ccord_mock_last_admin(tr);
  CT_CHECK(last && last->op == CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION && last->guild_id == 9101);
  CT_CHECK(ui_composer_lock_reason(F.ui) != nullptr);   // nothing local changed before Discord answered
  frame(4);
  CT_CHECK(!ui_self_pending(F.ui, 9101) && ui_composer_lock_reason(F.ui) == nullptr);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE && F.ui->mv.composer_btn.w == 0.0f);
}

static void test_issue3_auto_ack_and_input_guards(void)
{
  ccord_error err;
  // 1. ui_rgb_color(0) returns opaque black, and ui_user_banner_url uses ?size=512.
  ccord_color black = ui_rgb_color(0);
  CT_CHECK(black.r == 0 && black.g == 0 && black.b == 0 && black.a == 0xFFu);

  ccord_user u_hash = {.id = 42, .banner_url = "abc123"};
  char bbuf[256];
  CT_CHECK(ui_user_banner_url(&u_hash, bbuf, sizeof bbuf));
  CT_CHECK(strcmp(bbuf, "https://cdn.discordapp.com/banners/42/abc123.png?size=512") == 0);

  ccord_user u_cdn64 = {.id = 42, .banner_url = "https://cdn.discordapp.com/banners/42/abc123.png?size=64"};
  CT_CHECK(ui_user_banner_url(&u_cdn64, bbuf, sizeof bbuf));
  CT_CHECK(strcmp(bbuf, "https://cdn.discordapp.com/banners/42/abc123.png?size=512") == 0);

  // 2. auto_ack must not ack already-read or older messages (never roll last_read_id backwards).
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(45);   // settle and ack up to current newest message
  const ccord_channel *ch3111 = ccord_store_channel(F.store, 3111);
  CT_CHECK(ch3111 != nullptr);
  ccord_id cur_last = ch3111->last_message_id;
  CT_CHECK(cur_last != CCORD_ID_NONE);
  // Simulate Discord's read_state already being ahead of the newest local message.
  ccord_id ahead_id = cur_last + 100u;
  CT_CHECK(ccord_store_apply_read(F.store, 3111, ahead_id, 0, &err) == CCORD_OK);
  F.ui->ack_channel = CCORD_ID_NONE;
  F.ui->acked_message = CCORD_ID_NONE;
  F.ui->ack_next_ms = 0;
  size_t acks_before = ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK);
  frame(45);   // > UI_ACK_MIN_GAP_MS
  CT_CHECK(ccord_mock_call_count(F.transport, CCORD_MOCK_CALL_ACK) == acks_before);

  // 3. CCORD_IN_TEXT is guarded in Friends view and sync_text_input deactivates.
  ccord_text_edit_clear(&F.ui->edit);
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  F.ui->dm_friends_view = true;
  frame(2);
  CT_CHECK(!F.text_input_active);
  bool handled = ccord_ui_handle_input(F.ui, &(ccord_input_event){.type = CCORD_IN_TEXT, .text = "ghost"});
  CT_CHECK(!handled);
  CT_CHECK(F.ui->edit.len == 0);
  F.ui->dm_friends_view = false;
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(F.text_input_active);

  // 4. Bare-key keybind (Mute) and printable PTT suppress their accompanying CCORD_IN_TEXT.
  int prev_mute_sc = F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].scancode;
  uint8_t prev_mute_mods = F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].mods;
  int prev_voice_mode = F.ui->settings_voice_mode;
  int prev_ptt_sc = F.ui->settings_ptt_scancode;

  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].scancode = SDL_SCANCODE_M;
  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].mods = 0;
  key(CCORD_KEY_M, 0);
  type_text("m");
  CT_CHECK(F.ui->edit.len == 0);

  F.ui->settings_voice_mode = 1;
  F.ui->settings_ptt_scancode = SDL_SCANCODE_T;
  key(CCORD_KEY_T, 0);
  type_text("t");
  CT_CHECK(F.ui->edit.len == 0);

  // Synthetic CCORD_IN_TEXT without a preceding suppressed KEY_DOWN still inserts normally.
  type_text("ok");
  CT_CHECK(strcmp(ccord_text_edit_cstr(&F.ui->edit), "ok") == 0);
  ccord_text_edit_clear(&F.ui->edit);

  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].scancode = prev_mute_sc;
  F.ui->settings_keybinds[CCORD_UI_KEYBIND_MUTE].mods = prev_mute_mods;
  F.ui->settings_voice_mode = prev_voice_mode;
  F.ui->settings_ptt_scancode = prev_ptt_sc;
}

static void test_dm_profile_right_click_menu(void)
{
  ccord_error err;
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_id target_uid = other_member(g, 0);
  CT_CHECK(target_uid != CCORD_ID_NONE);
  CT_CHECK(ccord_session_open_dm(F.session, target_uid, &err) == CCORD_OK);
  frame(4);

  const ccord_channel *dm = nullptr;
  size_t ndm = ccord_store_channel_count(F.store, CCORD_DM_GUILD_ID);
  for (size_t i = 0; i < ndm; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, i);
    const ccord_user *du = c ? ui_dm_recipient(F.ui, c) : nullptr;
    if (du && du->id == target_uid) {
      dm = c;
      break;
    }
  }
  CT_CHECK(dm != nullptr);
  if (!dm) return;

  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  F.ui->dm_friends_view = false;
  clear_counts(CCORD_DM_GUILD_ID);
  const ccord_user_settings *us = ccord_store_user_settings(F.store);
  bool prev_dev = us && us->developer_mode;
  bool prev_anim_e = us ? us->animate_emoji : true;
  uint8_t prev_anim_s = us ? us->animate_stickers : 0;
  seed_user_settings(false, prev_anim_e, prev_anim_s);
  frame(4);

  // Right-click the DM row directly in the DM sidebar.
  float dm_y = scan_hover_channel(200.0f, dm->id);
  CT_CHECK(dm_y > 0.0f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);

  CT_CHECK(dbg().context_menu_open);
  int mark_idx = menu_index("Mark As Read");
  CT_CHECK(mark_idx == 0 && F.ui->menu.items[mark_idx].disabled);
  CT_CHECK(menu_index("Pin") > 0);
  CT_CHECK(menu_index("Add to Favorites") > 0);
  CT_CHECK(menu_index("Profile") > 0);
  CT_CHECK(menu_index("Start a Call") > 0 || menu_index("Leave Call") > 0);
  int note_idx = menu_index("Add Note");
  if (note_idx < 0) note_idx = menu_index("Edit Note");
  CT_CHECK(note_idx > 0 && F.ui->menu.items[note_idx].sublabel != nullptr &&
           strcmp(F.ui->menu.items[note_idx].sublabel, "Only visible to you") == 0);
  CT_CHECK(menu_index("Add Friend Nickname") > 0 || menu_index("Edit Friend Nickname") > 0);
  CT_CHECK(menu_index("Close DM") > 0);
  int apps_idx = menu_index("Apps");
  CT_CHECK(apps_idx > 0 && F.ui->menu.items[apps_idx].has_sub);
  CT_CHECK(menu_index("Invite to Server") > 0);
  CT_CHECK(menu_index("Ignore") > 0 || menu_index("Unignore") > 0);
  int blk_idx = menu_index("Block");
  CT_CHECK(blk_idx > 0 && F.ui->menu.items[blk_idx].danger);
  CT_CHECK(menu_index("Mute @") > 0 || menu_index("Unmute @") > 0);
  CT_CHECK(menu_index("Copy User ID") < 0 && menu_index("Copy Channel ID") < 0);

  // Pin toggles state and updates menu label to Unpin.
  menu_click_index(menu_index("Pin"));
  frame(2);
  CT_CHECK(ui_dm_is_pinned(F.ui, dm->id));
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Unpin") > 0 && menu_index("Pin") < 0);
  menu_click_index(menu_index("Unpin"));
  frame(2);
  CT_CHECK(!ui_dm_is_pinned(F.ui, dm->id));

  // Add to Favorites toggles state and label.
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  menu_click_index(menu_index("Add to Favorites"));
  frame(2);
  CT_CHECK(ui_dm_is_favorite(F.ui, dm->id));
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Remove from Favorites") > 0);
  menu_click_index(menu_index("Remove from Favorites"));
  frame(2);
  CT_CHECK(!ui_dm_is_favorite(F.ui, dm->id));

  // Add Friend Nickname modal updates ui_friend_nickname and DM display name, and Edit/Clear removes it.
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  menu_click_index(menu_index("Add Friend Nickname"));
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_FRIEND_NICK && F.ui->modal_target_id == target_uid);
  type_text("Bestie");
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_NONE);
  const char *fn = ui_friend_nickname(F.ui, target_uid);
  CT_CHECK(fn != nullptr && strcmp(fn, "Bestie") == 0);
  CT_CHECK(strcmp(ui_dm_display_name(F.ui, dm), "Bestie") == 0);

  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Edit Friend Nickname") > 0);
  menu_click_index(menu_index("Edit Friend Nickname"));
  frame(1);
  ccord_text_edit_clear(&F.ui->modal_edit1);
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(ui_friend_nickname(F.ui, target_uid) == nullptr);

  // Add/Edit Note modal opens UI_MODAL_USER_NOTE and submits CCORD_ADMIN_SET_USER_NOTE.
  size_t adm_before = ccord_mock_admin_count(F.transport);
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  note_idx = menu_index("Add Note");
  if (note_idx < 0) note_idx = menu_index("Edit Note");
  menu_click_index(note_idx);
  frame(1);
  CT_CHECK(F.ui->modal_kind == UI_MODAL_USER_NOTE && F.ui->modal_target_id == target_uid);
  ccord_text_edit_clear(&F.ui->modal_edit1);
  type_text("met at conf");
  key(CCORD_KEY_RETURN, 0);
  frame(4);
  CT_CHECK(ccord_mock_admin_count(F.transport) > adm_before);

  // Ignore / Unignore toggles state.
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  menu_click_index(menu_index("Ignore"));
  frame(1);
  CT_CHECK(ui_user_is_ignored(F.ui, target_uid));
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  CT_CHECK(menu_index("Unignore") > 0);
  menu_click_index(menu_index("Unignore"));
  frame(1);
  CT_CHECK(!ui_user_is_ignored(F.ui, target_uid));

  // Unread DM enables Mark As Read; Developer Mode exposes Copy User ID & Copy Channel ID.
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm->id, 3, 0, &err) == CCORD_OK);
  seed_user_settings(true, prev_anim_e, prev_anim_s);
  frame(2);
  dm_y = scan_hover_channel(200.0f, dm->id);
  mouse(CCORD_IN_MOUSE_DOWN, 3, 200.0f, dm_y);
  frame(1);
  mark_idx = menu_index("Mark As Read");
  int copy_uid_idx = menu_index("Copy User ID");
  int copy_cid_idx = menu_index("Copy Channel ID");
  CT_CHECK(mark_idx == 0 && !F.ui->menu.items[mark_idx].disabled);
  CT_CHECK(copy_uid_idx > 0 && copy_cid_idx == copy_uid_idx + 1);
  F.clip[0] = '\0';
  menu_click_index(copy_uid_idx);
  frame(1);
  char uid_str[32];
  ccord_id_format(target_uid, uid_str);
  CT_CHECK(strcmp(F.clip, uid_str) == 0);

  // Right-clicking a row in the Friends center view also opens the full DM/profile menu.
  CT_CHECK(ccord_store_set_relationship(F.store, target_uid, CCORD_RELATIONSHIP_FRIEND, &err) == CCORD_OK);
  F.ui->dm_friends_view = true;
  frame(2);
  if (F.ui->nfriend_row_hits > 0) {
    ccord_rect fr = F.ui->friend_row_hits[0].row_rect;
    mouse(CCORD_IN_MOUSE_DOWN, 3, fr.x + 80.0f, fr.y + fr.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && menu_index("Profile") >= 0 && menu_index("Block") >= 0);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }

  seed_user_settings(prev_dev, prev_anim_e, prev_anim_s);
  F.ui->dm_friends_view = false;
  clear_counts(CCORD_DM_GUILD_ID);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(2);
}

static void test_invite_preview_cards_and_url_opener(void)
{
  ccord_error err;
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  const ccord_channel *ch = nullptr;
  for (size_t ci = 0; ci < ccord_store_channel_count(F.store, g); ci++) {
    const ccord_channel *cand = ccord_store_channel_at(F.store, g, ci);
    if (cand && cand->kind == CCORD_CHANNEL_TEXT) { ch = cand; break; }
  }
  CT_CHECK(ch != nullptr);
  if (!ch) return;
  ccord_id ch_id = ch->id;

  // An earlier test fills the store to CCORD_MAX_REAL_GUILDS; free a slot so accepting "synthwave" can upsert guild 9002.
  while (ccord_store_guild_count(F.store) >= CCORD_MAX_REAL_GUILDS) {
    const ccord_guild *filler = ccord_store_guild_at(F.store, ccord_store_guild_count(F.store) - 1u);
    CT_CHECK(filler != nullptr && filler->id != g && filler->id != 2001 && filler->id != CCORD_DM_GUILD_ID);
    if (!filler || filler->id == g || filler->id == 2001 || filler->id == CCORD_DM_GUILD_ID) break;
    CT_CHECK(ccord_store_delete_guild(F.store, filler->id, &err) == CCORD_OK);
  }

  // 1. Sending a Discord invite link in chat renders an interactive Invite Preview card with a Join button.
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, ch_id, &err) == CCORD_OK);
  frame(2);
  type_text("Check this out: https://discord.gg/synthwave");
  key(CCORD_KEY_RETURN, 0);
  frame(6);
  CT_CHECK(dbg().invite_cards_drawn >= 1 && dbg().invite_btn_rect.w > 0.0f);

  // Clicking Join directly on the chat invite card accepts the invite inside C-Cord without leaving the app.
  size_t adm_before_join = ccord_mock_admin_count(F.transport);
  click_rect(dbg().invite_btn_rect);
  frame(4);
  const ccord_admin_request *join_req = ccord_mock_last_admin(F.transport);
  CT_CHECK(ccord_mock_admin_count(F.transport) > adm_before_join && join_req &&
           join_req->op == CCORD_ADMIN_ACCEPT_INVITE && strcmp(join_req->code, "synthwave") == 0);
  CT_CHECK(ccord_store_guild(F.store, 9002) != nullptr);

  // 2. Activating / opening a Discord invite link opens Join Server modal in-app instead of spawning a browser.
  int urls_before = F.open_url_calls;
  CT_CHECK(ccord_ui_open_url(F.ui, "https://discord.gg/vulkan-lab"));
  frame(4);
  CT_CHECK(F.open_url_calls == urls_before);
  CT_CHECK(dbg().add_server_modal_open && dbg().modal_has_invite_preview);
  CT_CHECK(strcmp(dbg().modal_preview_guild_name, "Vulkan & Graphics Lab") == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  // 2b. Opening a ccord://plugins/install/<id> deep link opens Settings -> Plugins review prompt without spawning a browser.
  CT_CHECK(ccord_ui_open_url(F.ui, "ccord://plugins/install/org.ccord.example-plugin"));
  frame(4);
  CT_CHECK(F.open_url_calls == urls_before);
  CT_CHECK(dbg().settings_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);

  // 3. Clicking a clan tag with an unknown/unavailable guild settles out of loading instead of staying stuck on "Loading server...".
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, ch_id, &err) == CCORD_OK);
  ccord_id self_id = ccord_store_self_id(F.store);
  const ccord_user *self_u = ccord_store_user(F.store, self_id);
  CT_CHECK(self_u != nullptr);
  if (self_u) {
    // Copy string fields owned by the store before upserting so restoring saved_self does not read freed pointers.
    ccord_user saved_self = *self_u;
    char s_name[128], s_handle[128], s_status[256], s_avatar[256], s_banner[256], s_bio[512], s_pronouns[128];
    snprintf(s_name, sizeof s_name, "%s", self_u->name ? self_u->name : "");
    snprintf(s_handle, sizeof s_handle, "%s", self_u->handle ? self_u->handle : "");
    snprintf(s_status, sizeof s_status, "%s", self_u->status_text ? self_u->status_text : "");
    snprintf(s_avatar, sizeof s_avatar, "%s", self_u->avatar_url ? self_u->avatar_url : "");
    snprintf(s_banner, sizeof s_banner, "%s", self_u->banner_url ? self_u->banner_url : "");
    snprintf(s_bio, sizeof s_bio, "%s", self_u->bio ? self_u->bio : "");
    snprintf(s_pronouns, sizeof s_pronouns, "%s", self_u->pronouns ? self_u->pronouns : "");
    saved_self.name = s_name;
    saved_self.handle = s_handle;
    saved_self.status_text = s_status;
    saved_self.avatar_url = s_avatar;
    saved_self.banner_url = s_banner;
    saved_self.bio = s_bio;
    saved_self.pronouns = s_pronouns;

    ccord_user unk_u = saved_self;
    snprintf(unk_u.clan_tag, sizeof unk_u.clan_tag, "UNKN");
    unk_u.clan_guild_id = 999999999;
    CT_CHECK(ccord_store_upsert_user(F.store, &unk_u, &err) == CCORD_OK);
    frame(2);
    ccord_rect tag_rect = {0};
    CT_CHECK(ui_clan_popover_hit_rect(F.ui, "UNKN", &tag_rect));
    if (tag_rect.w > 0.0f) {
      click_rect(tag_rect);
      frame(6);
      CT_CHECK(ui_clan_popover_is_open(F.ui));
      CT_CHECK(ui_clan_popover_box(F.ui).w > 0.0f);
      CT_CHECK(ui_clan_popover_guild_id(F.ui) == 999999999);
      CT_CHECK(!ccord_session_guild_profile_loading(F.session, 999999999));
      const ccord_guild_profile *unk_gp = ccord_session_guild_profile(F.session, 999999999);
      CT_CHECK(unk_gp != nullptr && unk_gp->unavailable);
      key(CCORD_KEY_ESCAPE, 0);
      frame(1);
      CT_CHECK(!ui_clan_popover_is_open(F.ui));
    }

    // Clicking a clan tag in chat/DM works even when prof_roles has not been allocated yet.
    ui_profile_roles_destroy(F.ui);
    CT_CHECK(F.ui->prof_roles == nullptr);
    ccord_user known_u = saved_self;
    snprintf(known_u.clan_tag, sizeof known_u.clan_tag, "KNO");
    known_u.clan_guild_id = g;
    CT_CHECK(ccord_store_upsert_user(F.store, &known_u, &err) == CCORD_OK);
    frame(2);
    ccord_rect known_rect = {0};
    CT_CHECK(ui_clan_popover_hit_rect(F.ui, "KNO", &known_rect));
    if (known_rect.w > 0.0f) {
      click_rect(known_rect);
      frame(6);
      CT_CHECK(ui_clan_popover_is_open(F.ui));
      CT_CHECK(ui_clan_popover_guild_id(F.ui) == g);
      CT_CHECK(ccord_session_guild_profile(F.session, g) != nullptr);
      key(CCORD_KEY_ESCAPE, 0);
      frame(1);
      CT_CHECK(!ui_clan_popover_is_open(F.ui));
    }

    // A transport error on an unavailable guild profile fetch also clears loading instead of sticking.
    ccord_user fail_u = saved_self;
    snprintf(fail_u.clan_tag, sizeof fail_u.clan_tag, "FAIL");
    fail_u.clan_guild_id = 999999998;
    CT_CHECK(ccord_store_upsert_user(F.store, &fail_u, &err) == CCORD_OK);
    frame(2);
    ccord_rect fail_rect = {0};
    CT_CHECK(ui_clan_popover_hit_rect(F.ui, "FAIL", &fail_rect));
    if (fail_rect.w > 0.0f) {
      ccord_mock_fail_next_admin(F.transport, true);
      click_rect(fail_rect);
      frame(6);
      CT_CHECK(ui_clan_popover_is_open(F.ui));
      CT_CHECK(ui_clan_popover_guild_id(F.ui) == 999999998);
      CT_CHECK(!ccord_session_guild_profile_loading(F.session, 999999998));
      key(CCORD_KEY_ESCAPE, 0);
      frame(1);
      CT_CHECK(!ui_clan_popover_is_open(F.ui));
    }

    CT_CHECK(!ccord_session_guild_profile_loading(F.session, CCORD_ID_NONE));
    CT_CHECK(ccord_store_upsert_user(F.store, &saved_self, &err) == CCORD_OK);
  }
}

typedef struct panel_ev_log {
  int count;
  ccord_panel_event_kind last_kind;
  char last_panel[40];
  char last_node[64];
  uint8_t last_button;
  uint8_t last_mods;
  float last_wheel;
} panel_ev_log;

static void record_panel_ev(void *ud, const ccord_panel_contribution *panel, const ccord_panel_event *ev)
{
  panel_ev_log *log = ud;
  log->count++;
  log->last_kind = ev->kind;
  snprintf(log->last_panel, sizeof log->last_panel, "%s", panel->panel_id ? panel->panel_id : "");
  snprintf(log->last_node, sizeof log->last_node, "%s", ev->node_id ? ev->node_id : "");
  log->last_button = ev->button;
  log->last_mods = ev->mods;
  log->last_wheel = ev->wheel;
}

static void test_plugin_ui_panels(void)
{
  ccord_error err = {0};
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(2);

  // Baseline layout with zero mounted panels starts flush at y = 0.
  ccord_layout base = *ccord_ui_get_layout(F.ui);
  CT_CHECK(fabsf(base.server_list.y) < 0.01f && fabsf(base.channel_list.y) < 0.01f &&
           fabsf(base.chat_header.y) < 0.01f);

  panel_ev_log plog = {0};
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){
                                          .ud = &plog, .menu_activate = menu_activate, .panel_event = record_panel_ev});

  // Mount Layer 1 flexbox panel in 'top', Layer 2 vector canvas panel in 'subheader', and status bar in 'bottom'.
  const char *top_spec =
    "{\"type\":\"row\",\"id\":\"strip\",\"bg\":\"bg_tertiary\",\"padding\":[4,8,4,8],\"gap\":6,"
    "\"children\":["
      "{\"type\":\"box\",\"id\":\"pill-1\",\"direction\":\"row\",\"align\":\"center\",\"gap\":6,"
       "\"padding\":[4,8,4,8],\"radius\":6,\"bg\":\"bg_primary\",\"hover_bg\":\"bg_hover\","
       "\"border_color\":\"border\",\"border_width\":1,\"border_sides\":\"no_bottom\","
       "\"cursor\":\"pointer\",\"tooltip\":\"First item\","
       "\"children\":["
         "{\"type\":\"icon\",\"icon\":\"hash\",\"size\":14,\"color\":\"text_muted\"},"
         "{\"type\":\"text\",\"text\":\"dev\",\"font\":\"bold\",\"color\":\"text_primary\"},"
         "{\"type\":\"icon\",\"id\":\"pill-1-close\",\"icon\":\"close\",\"size\":12,\"color\":\"text_muted\","
          "\"hover_color\":\"text_primary\",\"show_on_hover\":true,\"cursor\":\"pointer\"}"
       "]},"
      "{\"type\":\"button\",\"id\":\"add-btn\",\"icon\":\"plus\",\"label\":\"Add\",\"variant\":\"secondary\"},"
      "{\"type\":\"spacer\"},"
      "{\"type\":\"divider\",\"vertical\":true,\"color\":\"divider\"}"
    "]}";
  const char *sub_spec =
    "{\"type\":\"canvas\",\"id\":\"sub-canvas\",\"w\":\"100%\",\"h\":28,"
    "\"draw\":["
      "{\"cmd\":\"rect\",\"x\":0,\"y\":0,\"w\":\"100%\",\"h\":\"100%\",\"color\":\"bg_secondary\"},"
      "{\"cmd\":\"rrect\",\"x\":8,\"y\":4,\"w\":110,\"h\":20,\"radius\":5,\"color\":\"#5865F233\"},"
      "{\"cmd\":\"circle\",\"cx\":18,\"cy\":14,\"r\":4,\"color\":\"online\"},"
      "{\"cmd\":\"text\",\"x\":28,\"y\":7,\"text\":\"Canvas Subheader\",\"color\":\"text_primary\",\"size\":12},"
      "{\"cmd\":\"line\",\"x1\":0,\"y1\":27,\"x2\":\"100%\",\"y2\":27,\"width\":1,\"color\":\"divider\"}"
    "],"
    "\"hits\":["
      "{\"id\":\"canvas-pill\",\"x\":8,\"y\":4,\"w\":110,\"h\":20,\"cursor\":\"pointer\",\"tooltip\":\"Canvas pill\"}"
    "]}";
  const char *bot_spec =
    "{\"type\":\"row\",\"bg\":\"bg_tertiary\",\"padding\":[2,10,2,10],\"align\":\"center\","
    "\"children\":[{\"type\":\"text\",\"text\":\"Ready\",\"color\":\"text_muted\",\"size\":11}]}";

  CT_CHECK(ccord_contrib_set_panel(F.contrib, "dev.test.ui", "top-strip", CCORD_PANEL_TOP, 36.0f, top_spec, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_set_panel(F.contrib, "dev.test.ui", "sub-bar", CCORD_PANEL_SUBHEADER, 28.0f, sub_spec, &err) == CCORD_OK);
  CT_CHECK(ccord_contrib_set_panel(F.contrib, "dev.test.ui", "status", CCORD_PANEL_BOTTOM, 24.0f, bot_spec, &err) == CCORD_OK);
  frame(2);

  // Verify layout reflowed cleanly across all 3 slots (panels mount in the main workspace column; rail/channels stay flush).
  ccord_layout mounted = *ccord_ui_get_layout(F.ui);
  CT_CHECK(fabsf(mounted.server_list.y) < 0.01f);
  CT_CHECK(fabsf(mounted.channel_list.y) < 0.01f);
  CT_CHECK(fabsf(mounted.chat_header.y - 36.0f) < 0.01f);
  CT_CHECK(fabsf(mounted.message_area.y - (base.message_area.y + 36.0f + 28.0f)) < 0.01f);
  CT_CHECK(fabsf((mounted.composer.y + mounted.composer.h) - (base.composer.y + base.composer.h - 24.0f)) < 0.01f);

  // Verify hit rects and event dispatch (click, double_click, middle_click, right_click, wheel, show_on_hover).
  ccord_rect pill_r = {0}, close_r = {0}, canvas_r = {0};
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.ui", "top-strip", "pill-1", &pill_r) && pill_r.w > 20.0f);
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.ui", "sub-bar", "canvas-pill", &canvas_r) &&
           fabsf(canvas_r.w - 110.0f) < 0.5f);
  // Before hovering pill-1, show_on_hover child 'pill-1-close' is hidden (not registered as a hit).
  CT_CHECK(!ui_panels_find_hit_rect(F.ui, "dev.test.ui", "top-strip", "pill-1-close", &close_r));

  // Hover pill-1 -> cursor turns to POINTER and show_on_hover child becomes active!
  mouse(CCORD_IN_MOUSE_MOVE, 0, pill_r.x + 8.0f, pill_r.y + pill_r.h * 0.5f);
  frame(2);
  CT_CHECK(F.ui->cursor == CCORD_CURSOR_POINTER);
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.ui", "top-strip", "pill-1-close", &close_r) && close_r.w > 8.0f);

  // Left-click pill-1 -> CCORD_PANEL_EV_CLICK; double-click -> CCORD_PANEL_EV_DOUBLE_CLICK.
  plog.count = 0;
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  CT_CHECK(plog.count == 1 && plog.last_kind == CCORD_PANEL_EV_CLICK && strcmp(plog.last_node, "pill-1") == 0);

  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 2,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 2,
                           .x = pill_r.x + 8.0f, .y = pill_r.y + pill_r.h * 0.5f});
  CT_CHECK(plog.count == 2 && plog.last_kind == CCORD_PANEL_EV_DOUBLE_CLICK && strcmp(plog.last_node, "pill-1") == 0);

  // Click nested show_on_hover close icon -> deepest hit ('pill-1-close') wins over parent 'pill-1'.
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1,
                           .x = close_r.x + close_r.w * 0.5f, .y = close_r.y + close_r.h * 0.5f});
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1,
                           .x = close_r.x + close_r.w * 0.5f, .y = close_r.y + close_r.h * 0.5f});
  CT_CHECK(plog.count == 3 && plog.last_kind == CCORD_PANEL_EV_CLICK && strcmp(plog.last_node, "pill-1-close") == 0);

  // Middle-click and right-click on canvas hit region + wheel over top strip.
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                           .x = canvas_r.x + 20.0f, .y = canvas_r.y + 10.0f});
  CT_CHECK(plog.count == 4 && plog.last_kind == CCORD_PANEL_EV_MIDDLE_CLICK &&
           strcmp(plog.last_panel, "sub-bar") == 0 && strcmp(plog.last_node, "canvas-pill") == 0);

  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 3, .clicks = 1,
                           .x = canvas_r.x + 20.0f, .y = canvas_r.y + 10.0f});
  CT_CHECK(plog.count == 5 && plog.last_kind == CCORD_PANEL_EV_RIGHT_CLICK &&
           strcmp(plog.last_node, "canvas-pill") == 0 && !dbg().context_menu_open);

  ui_send((ccord_input_event){.type = CCORD_IN_WHEEL, .x = pill_r.x + 8.0f, .y = pill_r.y + 8.0f, .wheel = -1.0f});
  CT_CHECK(plog.count == 6 && plog.last_kind == CCORD_PANEL_EV_WHEEL && plog.last_wheel < 0.0f);

  // Unmount all 3 panels and verify full workspace height is reclaimed.
  CT_CHECK(ccord_contrib_remove_panel(F.contrib, "dev.test.ui", "top-strip"));
  CT_CHECK(ccord_contrib_remove_panel(F.contrib, "dev.test.ui", "sub-bar"));
  CT_CHECK(ccord_contrib_remove_panel(F.contrib, "dev.test.ui", "status"));
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){.menu_activate = menu_activate});
  frame(2);
  ccord_layout after = *ccord_ui_get_layout(F.ui);
  CT_CHECK(fabsf(after.server_list.y) < 0.01f && fabsf(after.message_area.y - base.message_area.y) < 0.01f &&
           fabsf(after.composer.y - base.composer.y) < 0.01f);
}

static void test_composer_upload_progress_indicator(void)
{
  ccord_error err = {0};
  ccord_mock_config mc = {.seed = 7, .latency_ms = 240, .now_ms = fake_clock};
  ccord_transport *tr = ccord_mock_transport_create(&mc, &err);
  CT_CHECK(tr != nullptr);
  if (!tr) return;
  CT_CHECK(ccord_session_set_transport(F.session, tr, &err) == CCORD_OK);
  F.transport = tr;
  frame(8);

  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  ccord_id dev = find_channel(g, "dev", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE);
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(8);

  uint8_t img_bytes[4096], vid_bytes[8192];
  memset(img_bytes, 0xAA, sizeof img_bytes);
  memset(vid_bytes, 0x55, sizeof vid_bytes);
  ccord_ui_clear_attachment(F.ui);
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "render.png", img_bytes, sizeof img_bytes));
  CT_CHECK(ccord_ui_attach_bytes(F.ui, "demo.mp4", vid_bytes, sizeof vid_bytes));
  frame(2);

  ccord_text_edit_clear(&F.ui->edit);
  type_text("uploading media");
  key(CCORD_KEY_RETURN, 0);

  // Immediately after send (t = +50ms < 240ms latency): upload banner is active above composer!
  draw_only();
  ui_upload_debug up = {0};
  ui_upload_debug_get(F.ui, &up);
  CT_CHECK(up.count == 1u);
  CT_CHECK(up.banner_rect.w > 120.0f && up.banner_rect.h > 30.0f);
  CT_CHECK(up.bar_rects[0].w > 80.0f && up.bar_rects[0].h > 2.0f);
  CT_CHECK(strstr(up.label[0], "render.png") != nullptr && strstr(up.label[0], "(1/2)") != nullptr);
  CT_CHECK(ccord_ui_next_wait_ms(F.ui, F.now) <= 100u);   // paces upload progress at 10 Hz instead of spinning VSync
  CT_CHECK(ccord_ui_update(F.ui, F.now + 100u));          // wakes on the 10 Hz upload cadence

  // Advance mid-upload (+100ms -> 166ms / 240ms): file 2/2 is actively uploading with mid-range percentage.
  F.now += 100u;
  draw_only();
  ui_upload_debug_get(F.ui, &up);
  CT_CHECK(up.count == 1u);
  CT_CHECK(up.pct[0] > 20 && up.pct[0] < 100);
  CT_CHECK(up.shown_fraction[0] > 0.0f);
  CT_CHECK(strstr(up.label[0], "demo.mp4") != nullptr && strstr(up.label[0], "(2/2)") != nullptr);

  // Pump past latency so MESSAGE_CREATE arrives -> upload banner disappears cleanly.
  frame(6);
  ui_upload_debug_get(F.ui, &up);
  CT_CHECK(up.count == 0u && up.banner_rect.h == 0.0f);

  // Restore standard 0-latency mock transport for subsequent tests.
  ccord_mock_config fast_mc = {.seed = 7, .now_ms = fake_clock};
  ccord_transport *fast_tr = ccord_mock_transport_create(&fast_mc, &err);
  CT_CHECK(fast_tr != nullptr);
  if (fast_tr) {
    CT_CHECK(ccord_session_set_transport(F.session, fast_tr, &err) == CCORD_OK);
    F.transport = fast_tr;
    frame(4);
  }
}

typedef struct nav_bus_log {
  int aux_count;
  ccord_event last_aux;
  int view_count;
  ccord_event last_view;
} nav_bus_log;

static void on_nav_bus_ev(const ccord_event *ev, void *ud)
{
  nav_bus_log *log = ud;
  if (ev->type == CCORD_EVT_NAV_AUX) {
    log->aux_count++;
    log->last_aux = *ev;
  } else if (ev->type == CCORD_EVT_VIEW_SELECT) {
    log->view_count++;
    log->last_view = *ev;
  }
}

static void test_plugin_nav_hooks_menus_and_aux_events(void)
{
  ccord_error err = {0};
  const ccord_id hq = 2001, lua = 2002;
  ccord_id dev = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  ccord_id forum_ch = find_channel(hq, "design-forum", CCORD_CHANNEL_TEXT);
  ccord_id other_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, hq); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, hq, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->id != dev && c->id != forum_ch &&
        !ccord_discord_type_is_forum(c->discord_type) && ui_channel_visible(F.ui, hq, c)) {
      other_ch = c->id;
      break;
    }
  }
  CT_CHECK(dev != CCORD_ID_NONE && other_ch != CCORD_ID_NONE && forum_ch != CCORD_ID_NONE);

  nav_bus_log nlog = {0};
  ccord_sub_id sub = ccord_bus_subscribe(F.bus, CCORD_EVT_ANY, on_nav_bus_ev, &nlog);
  CT_CHECK(sub != 0);

  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  F.ui->chan_scroll = 0.0f;
  frame(3);

  // 1. Ctrl+Click and Middle-Click on a channel row emit CCORD_EVT_NAV_AUX without leaving #dev.
  const ccord_layout *l = ccord_ui_get_layout(F.ui);
  float cx = l->channel_list.x + l->channel_list.w * 0.5f;
  float oy = scan_hover_channel(cx, other_ch);
  CT_CHECK(oy > 0.0f);
  nlog.aux_count = 0;
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_MOVE, .x = cx, .y = oy});
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .mods = CCORD_MOD_CTRL, .x = cx, .y = oy});
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .mods = CCORD_MOD_CTRL, .x = cx, .y = oy});
  frame(2);
  CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.guild_id == hq && nlog.last_aux.channel_id == other_ch &&
           nlog.last_aux.value == CCORD_NAV_VIEW_CHANNEL);
  CT_CHECK(ccord_store_selected_channel(F.store) == dev);

  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1, .x = cx, .y = oy});
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 2, .clicks = 1, .x = cx, .y = oy});
  frame(2);
  CT_CHECK(nlog.aux_count == 2 && nlog.last_aux.channel_id == other_ch);

  // 2. Middle-Click on server rail icon and DM Home rail icon emit CCORD_EVT_NAV_AUX (CCORD_NAV_VIEW_GUILD).
  size_t lua_slot = 0;
  ui_rail_slot slots[UI_RAIL_SLOTS_MAX];
  size_t nslots = ui_rail_build_slots(F.ui, slots, UI_RAIL_SLOTS_MAX, nullptr);
  bool found_lua = false;
  for (size_t i = 0; i < nslots; i++) {
    if (slots[i].kind == UI_RAIL_GUILD && slots[i].guild_id == lua) {
      lua_slot = i;
      found_lua = true;
      break;
    }
  }
  CT_CHECK(found_lua);
  if (found_lua) {
    ccord_rect lr = ui_guild_icon_rect(F.ui, lua_slot);
    mouse(CCORD_IN_MOUSE_MOVE, 0, lr.x + lr.w * 0.5f, lr.y + lr.h * 0.5f);
    ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                             .x = lr.x + lr.w * 0.5f, .y = lr.y + lr.h * 0.5f});
    frame(2);
    CT_CHECK(nlog.aux_count == 3 && nlog.last_aux.guild_id == lua && nlog.last_aux.value == CCORD_NAV_VIEW_GUILD);
  }
  ccord_rect dmr = ui_guild_icon_rect(F.ui, 0);
  mouse(CCORD_IN_MOUSE_MOVE, 0, dmr.x + dmr.w * 0.5f, dmr.y + dmr.h * 0.5f);
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                           .x = dmr.x + dmr.w * 0.5f, .y = dmr.y + dmr.h * 0.5f});
  frame(2);
  CT_CHECK(nlog.aux_count == 4 && nlog.last_aux.guild_id == CCORD_DM_GUILD_ID &&
           nlog.last_aux.value == CCORD_NAV_VIEW_GUILD);

  // 3. CCORD_MENU_NAV plugin context menu on DM Home rail icon and DM Friends row + ccord_contrib_remove_menu.
  CT_CHECK(ccord_contrib_add_menu(F.contrib, "test.plugin", "nav-item", "Open in Aux", CCORD_MENU_NAV, &err) == CCORD_OK);
  F.menu_activations = 0;
  mouse(CCORD_IN_MOUSE_MOVE, 0, dmr.x + dmr.w * 0.5f, dmr.y + dmr.h * 0.5f);
  mouse(CCORD_IN_MOUSE_DOWN, 3, dmr.x + dmr.w * 0.5f, dmr.y + dmr.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && menu_has("Open in Aux"));
  CT_CHECK(menu_click_label("Open in Aux"));
  frame(1);
  CT_CHECK(F.menu_activations == 1 && F.last_ctx.target == CCORD_MENU_NAV &&
           F.last_ctx.message_id == CCORD_NAV_VIEW_GUILD);
  CT_CHECK(ccord_contrib_remove_menu(F.contrib, "test.plugin", "nav-item"));

  // 4. Middle-click Forum post card emits CCORD_EVT_NAV_AUX and materializes post channel in store.
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, forum_ch, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(ui_forum_is_active(F.ui));
  ccord_id post_tid = CCORD_ID_NONE;
  ccord_rect card0 = ui_forum_post_card_rect(F.ui, 0, &post_tid);
  CT_CHECK(card0.w > 50.0f && post_tid != CCORD_ID_NONE);
  nlog.aux_count = 0;
  mouse(CCORD_IN_MOUSE_MOVE, 0, card0.x + 40.0f, card0.y + 24.0f);
  ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                           .x = card0.x + 40.0f, .y = card0.y + 24.0f});
  frame(2);
  CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.channel_id == post_tid && ccord_store_channel(F.store, post_tid) != nullptr);

  // 5. Inline #channel mention and inline thread card emit CCORD_EVT_NAV_AUX on middle-click and pass parent_id on menu.
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(3);
  size_t dev_mc = ccord_store_message_count(F.store, dev);
  const ccord_message *dev_tail = dev_mc > 0 ? ccord_store_message_at(F.store, dev, dev_mc - 1u) : nullptr;
  ccord_id next_mid = (dev_tail ? dev_tail->id : 1800000000000000000ULL) + 100ULL;
  char body[160];
  snprintf(body, sizeof body, "check out <#%llu> for details", (unsigned long long)other_ch);
  ccord_message_rich rich = {
    .present = CCORD_RICH_HAS_THREAD,
    .thread = {.id = 987654321ULL, .name = "Inline Thread Card", .message_count = 4},
  };
  ccord_message cm = {
    .id = next_mid,
    .channel_id = dev,
    .author_id = 1002,
    .timestamp_ms = (dev_tail ? dev_tail->timestamp_ms : (int64_t)F.ui->now_unix_ms) + 600000,
    .content = body,
    .rich = &rich,
  };
  CT_CHECK(ccord_store_append_message(F.store, &cm, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(3);
  CT_CHECK(F.ui->nchan_hits > 0 && F.ui->nthread_hits > 0);
  if (F.ui->nchan_hits > 0) {
    ccord_rect cr = F.ui->chan_hits[F.ui->nchan_hits - 1u].rect;
    nlog.aux_count = 0;
    ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                             .x = cr.x + cr.w * 0.5f, .y = cr.y + cr.h * 0.5f});
    frame(2);
    CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.channel_id == other_ch);
  }
  if (F.ui->nthread_hits > 0) {
    ccord_rect trc = F.ui->thread_hits[F.ui->nthread_hits - 1u].rect;
    nlog.aux_count = 0;
    ui_send((ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 2, .clicks = 1,
                             .x = trc.x + trc.w * 0.5f, .y = trc.y + trc.h * 0.5f});
    frame(2);
    CT_CHECK(nlog.aux_count == 1 && nlog.last_aux.channel_id == 987654321ULL &&
             nlog.last_aux.message_id == dev && nlog.last_aux.value == CCORD_NAV_VIEW_THREAD);
  }

  // 6. UI hooks registered on contrib drive client views & actions cleanly.
  nlog.view_count = 0;
  CT_CHECK(ccord_contrib_ui_navigate(F.contrib, 0, 0, 0, nullptr, "friends"));
  frame(2);
  CT_CHECK(strcmp(ccord_contrib_ui_view(F.contrib), "friends") == 0 && nlog.view_count >= 1);
  CT_CHECK(ccord_contrib_ui_action(F.contrib, "quick_switcher"));
  CT_CHECK(F.ui->quick_switcher_open);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->quick_switcher_open);

  ccord_bus_unsubscribe(F.bus, sub);
  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(2);
}

static void test_chat_message_vs_profile_right_click_and_admin_tools(void)
{
  ccord_error err = {0};
  const ccord_id hq = 2001, garden = 2003;
  const ccord_id ada = 1002, bjorn = 1003, chen = 1004;
  ccord_id dev = find_channel(hq, "dev", CCORD_CHANNEL_TEXT);
  ccord_id art = find_channel(garden, "art", CCORD_CHANNEL_TEXT);
  CT_CHECK(dev != CCORD_ID_NONE && art != CCORD_ID_NONE);

  CT_CHECK(ccord_session_select_guild(F.session, hq, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dev, &err) == CCORD_OK);
  frame(3);

  // Append a base message from Chen (1004) and a non-compact reply from Ada (1002) mentioning @Bjorn (<@1003>).
  size_t dev_mc = ccord_store_message_count(F.store, dev);
  const ccord_message *dev_tail = dev_mc > 0 ? ccord_store_message_at(F.store, dev, dev_mc - 1u) : nullptr;
  ccord_id base_id = (dev_tail ? dev_tail->id : 1800000000000000000ULL) + 100ULL;
  int64_t base_ts = (dev_tail ? dev_tail->timestamp_ms : (int64_t)F.ui->now_unix_ms) + 600000;
  ccord_message base_m = {
    .id = base_id,
    .channel_id = dev,
    .author_id = chen,
    .timestamp_ms = base_ts,
    .content = "Chen base message for reply target",
  };
  ccord_message reply_m = {
    .id = base_id + 1ULL,
    .channel_id = dev,
    .author_id = ada,
    .reply_to_id = base_m.id,
    .timestamp_ms = base_ts + 600000,
    .content = "Plain message text body mentioning <@1003> right here",
  };
  CT_CHECK(ccord_store_append_message(F.store, &base_m, &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(F.store, &reply_m, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(3);

  // Locate Ada's rendered message row.
  const vis_row *ada_row = nullptr;
  for (size_t i = 0; i < F.ui->nvis; i++) {
    if (F.ui->vis[i].msg_id == reply_m.id) {
      ada_row = &F.ui->vis[i];
      break;
    }
  }
  CT_CHECK(ada_row != nullptr && ada_row->avatar_rect.w > 0.0f && ada_row->author_rect.w > 0.0f);
  if (!ada_row) return;

  // 1. Right-clicking the message body (away from avatar/name/@mention) opens CCORD_MENU_MESSAGE.
  float body_x = ada_row->author_rect.x + 20.0f;
  float body_y = ada_row->y + ada_row->h - 10.0f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, body_x, body_y);
  frame(1);
  CT_CHECK(dbg().hover_message == reply_m.id && dbg().hover_user == CCORD_ID_NONE);
  mouse(CCORD_IN_MOUSE_DOWN, 3, body_x, body_y);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MESSAGE &&
           F.ui->menu.ctx.message_id == reply_m.id);
  CT_CHECK(menu_has("Copy Text") && menu_has("Reply") && !menu_has("Profile") && !menu_has("Kick Member"));
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 2. Hovering & right-clicking Ada's avatar opens CCORD_MENU_MEMBER with profile + server admin tools!
  ccord_rect av = ada_row->avatar_rect;
  mouse(CCORD_IN_MOUSE_MOVE, 0, av.x + av.w * 0.5f, av.y + av.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().hover_user == ada);
  mouse(CCORD_IN_MOUSE_DOWN, 3, av.x + av.w * 0.5f, av.y + av.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == ada);
  CT_CHECK(menu_has("Profile") && menu_has("Mention") && menu_has("Message (Open DM)") &&
           menu_has("Open in Mod View") && menu_has("Roles") && menu_has("Change Nickname") &&
           menu_has("Timeout Member") && menu_has("Kick Member") && menu_has("Ban Member"));
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);

  // 3. Hovering & right-clicking Ada's display name opens CCORD_MENU_MEMBER; clicking "Mention" inserts @Ada!
  ccord_text_edit_clear(&F.ui->edit);
  ccord_rect au = ada_row->author_rect;
  mouse(CCORD_IN_MOUSE_MOVE, 0, au.x + au.w * 0.5f, au.y + au.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().hover_user == ada);
  mouse(CCORD_IN_MOUSE_DOWN, 3, au.x + au.w * 0.5f, au.y + au.h * 0.5f);
  frame(1);
  CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == ada);
  CT_CHECK(menu_click_label("Mention"));
  frame(1);
  CT_CHECK(strstr(ccord_ui_composer_text(F.ui), "@Ada ") != nullptr);
  ccord_text_edit_clear(&F.ui->edit);

  // 4. Hovering & right-clicking inline @Bjorn mention and replied-to author Chen open their respective profile menus!
  ccord_rect bjorn_hit = {0}, chen_reply_hit = {0};
  for (size_t i = 0; i < F.ui->nuser_hits; i++) {
    if (F.ui->user_hits[i].user_id == bjorn && F.ui->user_hits[i].rect.y >= ada_row->y) {
      bjorn_hit = F.ui->user_hits[i].rect;
    }
    if (F.ui->user_hits[i].user_id == chen && F.ui->user_hits[i].rect.y >= ada_row->y &&
        F.ui->user_hits[i].rect.y < au.y) {
      chen_reply_hit = F.ui->user_hits[i].rect;
    }
  }
  CT_CHECK(bjorn_hit.w > 0.0f && chen_reply_hit.w > 0.0f);
  if (bjorn_hit.w > 0.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, bjorn_hit.x + bjorn_hit.w * 0.5f, bjorn_hit.y + bjorn_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().hover_user == bjorn);
    mouse(CCORD_IN_MOUSE_DOWN, 3, bjorn_hit.x + bjorn_hit.w * 0.5f, bjorn_hit.y + bjorn_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == bjorn);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }
  if (chen_reply_hit.w > 0.0f) {
    mouse(CCORD_IN_MOUSE_MOVE, 0, chen_reply_hit.x + chen_reply_hit.w * 0.5f, chen_reply_hit.y + chen_reply_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().hover_user == chen);
    mouse(CCORD_IN_MOUSE_DOWN, 3, chen_reply_hit.x + chen_reply_hit.w * 0.5f, chen_reply_hit.y + chen_reply_hit.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == chen);
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }

  // 5. Server admin tools: "Roles" submenu toggles a role on Ada, and "Open in Mod View" opens Mod View!
  size_t adm_before = ccord_mock_admin_count(F.transport);
  open_menu(CCORD_MENU_MEMBER, hq, dev, ada, CCORD_ID_NONE);
  frame(1);
  int roles_idx = menu_index("Roles");
  CT_CHECK(roles_idx >= 0 && F.ui->menu.items[roles_idx].has_sub);
  menu_point_at(roles_idx);
  CT_CHECK(F.ui->menu.open_sub == roles_idx && dbg().context_menu_sub_items > 0);
  int first_role_child = -1;
  for (size_t i = 0; i < F.ui->menu.count; i++) {
    if (F.ui->menu.items[i].parent == roles_idx) {
      first_role_child = (int)i;
      break;
    }
  }
  CT_CHECK(first_role_child >= 0);
  if (first_role_child >= 0) {
    menu_click_index(first_role_child);
    frame(3);
    CT_CHECK(ccord_mock_admin_count(F.transport) == adm_before + 1u);
    const ccord_admin_request *last = ccord_mock_last_admin(F.transport);
    CT_CHECK(last && (last->op == CCORD_ADMIN_ADD_MEMBER_ROLE || last->op == CCORD_ADMIN_REMOVE_MEMBER_ROLE) &&
             last->user_id == ada);
  }

  open_menu(CCORD_MENU_MEMBER, hq, dev, ada, CCORD_ID_NONE);
  frame(1);
  CT_CHECK(menu_click_label("Open in Mod View"));
  frame(2);
  CT_CHECK(ui_server_settings_is_open(F.ui));
  ui_server_settings_close(F.ui);
  frame(2);
  CT_CHECK(!ui_server_settings_is_open(F.ui));

  // 6. Unprivileged server (Pixel Garden): right-clicking a user's profile shows profile items ONLY, zero admin tools!
  CT_CHECK(ccord_session_select_guild(F.session, garden, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, art, &err) == CCORD_OK);
  frame(3);
  size_t art_mc = ccord_store_message_count(F.store, art);
  const ccord_message *art_tail = art_mc > 0 ? ccord_store_message_at(F.store, art, art_mc - 1u) : nullptr;
  ccord_message g_m = {
    .id = (art_tail ? art_tail->id : 1800000000000000000ULL) + 100ULL,
    .channel_id = art,
    .author_id = chen,
    .timestamp_ms = (art_tail ? art_tail->timestamp_ms : (int64_t)F.ui->now_unix_ms) + 600000,
    .content = "Pixel Garden message from Chen",
  };
  CT_CHECK(ccord_store_append_message(F.store, &g_m, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(3);
  const vis_row *chen_row = nullptr;
  for (size_t i = 0; i < F.ui->nvis; i++) {
    if (F.ui->vis[i].msg_id == g_m.id) {
      chen_row = &F.ui->vis[i];
      break;
    }
  }
  CT_CHECK(chen_row != nullptr);
  if (chen_row) {
    ccord_rect cav = chen_row->avatar_rect;
    mouse(CCORD_IN_MOUSE_MOVE, 0, cav.x + cav.w * 0.5f, cav.y + cav.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().hover_user == chen);
    mouse(CCORD_IN_MOUSE_DOWN, 3, cav.x + cav.w * 0.5f, cav.y + cav.h * 0.5f);
    frame(1);
    CT_CHECK(dbg().context_menu_open && F.ui->menu.ctx.target == CCORD_MENU_MEMBER && F.ui->menu.ctx.user_id == chen);
    CT_CHECK(menu_has("Profile") && menu_has("Mention") && menu_has("Message (Open DM)") && menu_has("Report User"));
    CT_CHECK(!menu_has("Open in Mod View") && !menu_has("Roles") && !menu_has("Change Nickname") &&
             !menu_has("Timeout Member") && !menu_has("Kick Member") && !menu_has("Ban Member"));
    key(CCORD_KEY_ESCAPE, 0);
    frame(1);
  }
}

// Covers three notification edge cases:
// 1) ui_is_reading() is false when full-window overlays (Settings, full-screen voice stage, fullscreen stream) hide chat.
// 2) CCORD_IN_FOCUS(false) updates session reading_channel immediately (before ccord_ui_update) so incoming messages notify.
// 3) ccord_ui_prepare_notification() truncates multi-byte UTF-8 on codepoint boundaries so platform delivery never drops them.
static void test_reading_overlays_immediate_focus_and_utf8_notify(void)
{
  ccord_error err;
  ccord_id g = ccord_store_guild_at(F.store, 0)->id;
  CT_CHECK(ccord_session_select_guild(F.session, g, &err) == CCORD_OK);
  frame(3);
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(ch != CCORD_ID_NONE);
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  ccord_vlist_reset_bottom(&F.ui->vlist);
  frame(3);
  CT_CHECK(ui_is_reading(F.ui));

  // 1. Settings overlay, full-screen voice stage, and fullscreen stream all pause active reading.
  ui_open_settings(F.ui, true);
  CT_CHECK(!ui_is_reading(F.ui));
  ui_open_settings(F.ui, false);
  CT_CHECK(ui_is_reading(F.ui));

  ccord_id voice_ch = CCORD_ID_NONE;
  size_t nch = ccord_store_channel_count(F.store, g);
  for (size_t i = 0; i < nch && voice_ch == CCORD_ID_NONE; i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, g, i);
    if (c && c->kind == CCORD_CHANNEL_VOICE) voice_ch = c->id;
  }
  if (voice_ch != CCORD_ID_NONE) {
    ccord_ui_set_viewport(F.ui, 1280.0f, 800.0f);
    CT_CHECK(ccord_session_join_voice(F.session, g, voice_ch, &err) == CCORD_OK);
    frame(3);
    F.ui->stage_channel = voice_ch;
    F.ui->stage_chat_open = false;
    ccord_ui_set_viewport(F.ui, 1280.0f, 800.0f);
    frame(2);
    CT_CHECK(F.ui->stage_channel == voice_ch && !ui_stage_split_active(F.ui));
    CT_CHECK(!ui_is_reading(F.ui));
    ccord_rect chat_btn = ui_stage_chat_btn(F.ui);
    click(chat_btn.x + chat_btn.w * 0.5f, chat_btn.y + chat_btn.h * 0.5f);
    frame(2);
    CT_CHECK(ui_stage_split_active(F.ui));
    CT_CHECK(ui_is_reading(F.ui));
    CT_CHECK(ccord_session_leave_voice(F.session, g, &err) == CCORD_OK);
    frame(3);
    CT_CHECK(F.ui->stage_channel == CCORD_ID_NONE && ui_is_reading(F.ui));
  }

  F.ui->stream_fullscreen = true;
  CT_CHECK(!ui_is_reading(F.ui));
  F.ui->stream_fullscreen = false;
  CT_CHECK(ui_is_reading(F.ui));

  // 2. Losing focus syncs reading_channel to CCORD_ID_NONE immediately (before ccord_ui_update runs),
  // so a message arriving in the selected channel in the same tick still queues a notification.
  ccord_notification popped;
  while (ccord_session_pop_notification(F.session, &popped)) {}
  ccord_notify_scope all_row = {.guild_id = g, .channel_id = CCORD_ID_NONE, .level = CCORD_NOTIFY_ALL};
  CT_CHECK(ccord_store_set_notify(F.store, g, &all_row, 1, &err) == CCORD_OK);
  frame(2);
  while (ccord_session_pop_notification(F.session, &popped)) {}
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  // Intentionally do NOT call ccord_ui_update() before pumping the incoming message.
  ccord_id posted_id = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, ch, 1002u, "arrived right on unfocus", nullptr, &posted_id, &err) ==
           CCORD_OK);
  CT_CHECK(ccord_session_pump(F.session) >= 1);
  bool saw_ch = false;
  while (ccord_session_pop_notification(F.session, &popped)) {
    if (popped.channel_id == ch && popped.message_id == posted_id) saw_ch = true;
  }
  CT_CHECK(saw_ch);
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  CT_CHECK(ccord_store_set_notify(F.store, g, nullptr, 0, &err) == CCORD_OK);
  frame(2);

  // 3. UTF-8 boundary-safe truncation in ccord_ui_prepare_notification: 4-byte emoji straddling bytes 50, 60, and 240
  // must never leave invalid UTF-8 bytes that ccord_platform_notify_message would reject.
  char uname50[80], uname60[80], cname50[80], body240[300];
  memset(uname50, 'a', 49); memcpy(uname50 + 49, "\xF0\x9F\x98\x80tail", 9);
  memset(uname60, 'b', 59); memcpy(uname60 + 59, "\xF0\x9F\x98\x80tail", 9);
  memset(cname50, 'c', 49); memcpy(cname50 + 49, "\xF0\x9F\x98\x80tail", 9);
  memset(body240, 'x', 239); memcpy(body240 + 239, "\xF0\x9F\x98\x80tail", 9);

  const ccord_id u50_id = 9876501u, u60_id = 9876502u, c50_id = 9876511u, dm_id = 9876512u, m50_id = 9876521u, m60_id = 9876522u;
  ccord_user u50 = {.id = u50_id, .name = uname50, .status_text = ""};
  ccord_user u60 = {.id = u60_id, .name = uname60, .status_text = ""};
  ccord_channel c50 = {.id = c50_id, .guild_id = g, .kind = CCORD_CHANNEL_TEXT, .name = cname50, .topic = ""};
  ccord_channel dm60 = {.id = dm_id, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT, .name = uname60, .topic = "",
                        .user_id = u60_id};
  ccord_message m50 = {.id = m50_id, .channel_id = c50_id, .author_id = u50_id, .timestamp_ms = 1000, .content = body240};
  ccord_message m60 = {.id = m60_id, .channel_id = dm_id, .author_id = u60_id, .timestamp_ms = 1001, .content = body240};
  CT_CHECK(ccord_store_upsert_user(F.store, &u50, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_user(F.store, &u60, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(F.store, &c50, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm60, &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(F.store, &m50, &err) == CCORD_OK);
  CT_CHECK(ccord_store_append_message(F.store, &m60, &err) == CCORD_OK);

  F.ui->settings_notify_desktop = true;
  F.ui->settings_notify_messages = true;
  F.ui->settings_notify_mentions = true;
  ccord_platform_set_window_focused(F.platform, false);

  char title[128], body[256];
  ccord_notification gn = {.guild_id = g, .channel_id = c50_id, .message_id = m50_id, .author_id = u50_id};
  CT_CHECK(ccord_ui_prepare_notification(F.ui, &gn, title, sizeof title, body, sizeof body));
  CT_CHECK(ccord_utf8_valid(title, strlen(title)));
  CT_CHECK(ccord_utf8_valid(body, strlen(body)));
  ccord_notify_target tgt = {.guild_id = g, .channel_id = c50_id, .message_id = m50_id};
  CT_CHECK(ccord_platform_notify_message(F.platform, title, body, &tgt));

  ccord_notification dn = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = dm_id, .message_id = m60_id,
                           .author_id = u60_id, .is_dm = true};
  CT_CHECK(ccord_ui_prepare_notification(F.ui, &dn, title, sizeof title, body, sizeof body));
  CT_CHECK(ccord_utf8_valid(title, strlen(title)));
  CT_CHECK(ccord_utf8_valid(body, strlen(body)));
  ccord_notify_target dtgt = {.guild_id = CCORD_DM_GUILD_ID, .channel_id = dm_id, .message_id = m60_id};
  CT_CHECK(ccord_platform_notify_message(F.platform, title, body, &dtgt));
  ccord_platform_set_window_focused(F.platform, true);
}

static void test_dm_and_substring_mention_autocomplete(void)
{
  ccord_error err;
  // 1. Guild substring matching & mention pill rendering in the composer.
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(4);
  clear_composer_input();
  type_text("@jor");
  frame(2);
  CT_CHECK(dbg().autocomplete_open && dbg().autocomplete_count > 0);
  CT_CHECK(autocomplete_has("Bjorn"));
  ccord_rect cbox = ui_composer_box(F.ui);
  ccord_rect ac = dbg().autocomplete_rect;
  CT_CHECK(ac.w > 0.0f && ac.h > 0.0f && ac.y + ac.h <= cbox.y);
  key(CCORD_KEY_TAB, 0);
  frame(2);
  CT_CHECK(!dbg().autocomplete_open);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "@Bjorn ") == 0);
  ui_cf_trace_piece tr[64];
  size_t ntr = ui_composer_format_trace(tr, 64);
  bool has_mention_pill = false;
  for (size_t i = 0; i < ntr; i++) {
    if ((tr[i].flags & UI_CF_MENTION) && tr[i].has_bg) has_mention_pill = true;
  }
  CT_CHECK(has_mention_pill);

  // Switching channel closes open autocomplete.
  type_text("@Ad");
  frame(1);
  CT_CHECK(dbg().autocomplete_open);
  CT_CHECK(ccord_session_select_channel(F.session, 3113, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(!dbg().autocomplete_open);
  clear_composer_input();

  // 2. 1-on-1 DM @mention autocomplete, completion, composer pill, and serialization.
  ccord_user dm_u = {.id = 919101u, .name = "Zelda", .handle = "zelda_h", .presence = CCORD_PRESENCE_ONLINE};
  CT_CHECK(ccord_store_upsert_user(F.store, &dm_u, &err) == CCORD_OK);
  ccord_channel dm1 = {
    .id = 919201u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
    .name = "Zelda", .topic = "", .user_id = dm_u.id,
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm1, &err) == CCORD_OK);
  F.ui->dm_friends_view = false;
  F.ui->dm_requests_view = false;
  CT_CHECK(ccord_store_select_guild(F.store, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, dm1.id, &err) == CCORD_OK);
  frame(3);
  clear_composer_input();
  type_text("hi @");
  frame(2);
  CT_CHECK(dbg().autocomplete_open && dbg().autocomplete_count >= 1);
  CT_CHECK(autocomplete_has("Zelda"));
  type_text("el");   // substring match on "Zelda"
  frame(1);
  CT_CHECK(dbg().autocomplete_open && autocomplete_has("Zelda"));
  key(CCORD_KEY_TAB, 0);
  frame(2);
  CT_CHECK(!dbg().autocomplete_open);
  CT_CHECK(strcmp(ccord_ui_composer_text(F.ui), "hi @Zelda ") == 0);
  ntr = ui_composer_format_trace(tr, 64);
  bool dm_pill = false;
  for (size_t i = 0; i < ntr; i++) {
    if ((tr[i].flags & UI_CF_MENTION) && tr[i].has_bg) dm_pill = true;
  }
  CT_CHECK(dm_pill);
  char dm_ser[128];
  size_t dm_slen = ccord_ui_serialize_mentions(F.store, CCORD_DM_GUILD_ID,
                                               ccord_ui_composer_text(F.ui), strlen(ccord_ui_composer_text(F.ui)),
                                               dm_ser, sizeof dm_ser);
  CT_CHECK(dm_slen > 0 && strcmp(dm_ser, "hi <@919101> ") == 0);
  clear_composer_input();

  // 3. Group DM @mention autocomplete across all recipients.
  ccord_user g_u1 = {.id = 919102u, .name = "Marley", .handle = "marley_g", .presence = CCORD_PRESENCE_ONLINE};
  ccord_user g_u2 = {.id = 919103u, .name = "Nadia", .handle = "nadia_g", .presence = CCORD_PRESENCE_ONLINE};
  CT_CHECK(ccord_store_upsert_user(F.store, &g_u1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_user(F.store, &g_u2, &err) == CCORD_OK);
  ccord_channel gdm = {
    .id = 919202u, .guild_id = CCORD_DM_GUILD_ID, .kind = CCORD_CHANNEL_TEXT,
    .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM, .name = "Group", .topic = "",
    .user_id = CCORD_ID_NONE, .recipient_ids = {g_u1.id, g_u2.id}, .recipient_count = 2,
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &gdm, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, gdm.id, &err) == CCORD_OK);
  frame(3);
  type_text("@");
  frame(2);
  CT_CHECK(dbg().autocomplete_open && autocomplete_has("Marley") && autocomplete_has("Nadia"));
  type_text("adi");
  frame(1);
  CT_CHECK(dbg().autocomplete_open && autocomplete_has("Nadia") && !autocomplete_has("Marley"));
  key(CCORD_KEY_RETURN, 0);
  frame(2);
  CT_CHECK(!dbg().autocomplete_open && strcmp(ccord_ui_composer_text(F.ui), "@Nadia ") == 0);
  clear_composer_input();

  CT_CHECK(ccord_store_delete_channel(F.store, gdm.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_channel(F.store, dm1.id, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_guild(F.session, 2001, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, 3111, &err) == CCORD_OK);
  frame(2);
}

typedef struct v3_ui_cb_state {
  int panel_evs;
  ccord_panel_event_kind last_panel_kind;
  char last_panel_node[64];
  bool last_bool;
  double last_num;
  char last_text[64];
  int setting_changes;
  char last_setting_key[64];
  char last_setting_val[64];
  int dlg_results;
  bool last_dlg_ok;
  char last_dlg_json[128];
  int kb_triggers;
  int actbtn_clicks;
} v3_ui_cb_state;

static void v3_ui_panel_ev(void *ud, const ccord_panel_contribution *p, const ccord_panel_event *ev)
{
  (void)p;
  v3_ui_cb_state *s = ud;
  s->panel_evs++;
  s->last_panel_kind = ev->kind;
  snprintf(s->last_panel_node, sizeof s->last_panel_node, "%s", ev->node_id ? ev->node_id : "");
  s->last_bool = ev->bool_value;
  s->last_num = ev->num_value;
  snprintf(s->last_text, sizeof s->last_text, "%s", ev->text_value ? ev->text_value : "");
}

static void v3_ui_setting_chg(void *ud, const char *owner, const char *key_name, const char *val_json)
{
  (void)owner;
  v3_ui_cb_state *s = ud;
  s->setting_changes++;
  snprintf(s->last_setting_key, sizeof s->last_setting_key, "%s", key_name ? key_name : "");
  snprintf(s->last_setting_val, sizeof s->last_setting_val, "%s", val_json ? val_json : "");
}

static void v3_ui_dlg_res(void *ud, const char *owner, uint64_t req_id, bool accepted, const char *res_json)
{
  (void)owner; (void)req_id;
  v3_ui_cb_state *s = ud;
  s->dlg_results++;
  s->last_dlg_ok = accepted;
  snprintf(s->last_dlg_json, sizeof s->last_dlg_json, "%s", res_json ? res_json : "");
}

static void v3_ui_kb_trig(void *ud, const char *owner, const char *bind_id)
{
  (void)owner; (void)bind_id;
  ((v3_ui_cb_state *)ud)->kb_triggers++;
}

static void v3_ui_actbtn_clk(void *ud, const ccord_action_btn_contribution *b, const ccord_menu_context *ctx)
{
  (void)b; (void)ctx;
  ((v3_ui_cb_state *)ud)->actbtn_clicks++;
}

static void test_plugin_api_v3_ui(void)
{
  ccord_error err = {0};
  ccord_id g = ccord_store_selected_guild(F.store);
  ccord_id ch = ccord_store_selected_channel(F.store);
  CT_CHECK(g != CCORD_ID_NONE && ch != CCORD_ID_NONE);
  size_t nm = ccord_store_message_count(F.store, ch);
  ccord_id sample_mid = nm ? ccord_store_message_at(F.store, ch, nm - 1)->id : 0;

  v3_ui_cb_state st = {0};
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){
    .ud = &st,
    .menu_activate = menu_activate,
    .panel_event = v3_ui_panel_ev,
    .plugin_setting_changed = v3_ui_setting_chg,
    .keybind_triggered = v3_ui_kb_trig,
    .action_btn_click = v3_ui_actbtn_clk,
    .dialog_result = v3_ui_dlg_res,
  });

  // 1. Panel Engine v2: modal with interactive toggle, slider, input & canvas v3 draw primitives.
  const char *modal_json =
    "{\"type\":\"column\",\"padding\":[12,12,12,12],\"gap\":8,\"children\":["
      "{\"type\":\"toggle\",\"id\":\"v3-tog\",\"label\":\"Auto Mode\",\"value\":false},"
      "{\"type\":\"slider\",\"id\":\"v3-sld\",\"label\":\"Gain\",\"min\":0,\"max\":100,\"value\":20},"
      "{\"type\":\"input\",\"id\":\"v3-inp\",\"placeholder\":\"Type...\",\"value\":\"hi\"},"
      "{\"type\":\"canvas\",\"id\":\"v3-cnv\",\"w\":200,\"h\":40,\"draw\":["
        "{\"cmd\":\"gradient\",\"x\":0,\"y\":0,\"w\":200,\"h\":40,\"from\":\"#223344\",\"to\":\"#556677\",\"vertical\":false},"
        "{\"cmd\":\"rrect_stroke\",\"x\":2,\"y\":2,\"w\":196,\"h\":36,\"radius\":6,\"width\":2,\"color\":\"#ffffff\"},"
        "{\"cmd\":\"circle_stroke\",\"cx\":24,\"cy\":20,\"r\":10,\"width\":2,\"color\":\"#00ff88\"},"
        "{\"cmd\":\"arc\",\"cx\":60,\"cy\":20,\"r\":12,\"start\":0,\"end\":180,\"width\":3,\"color\":\"#ffcc00\",\"filled\":false},"
        "{\"cmd\":\"polygon\",\"points\":[[100,8],[120,20],[100,32],[80,20]],\"color\":\"#ff44aa\",\"filled\":true}"
      "]}"
    "]}";
  ccord_panel_spec mspec = {
    .slot = CCORD_PANEL_MODAL, .width = 360.0f, .height = 240.0f,
    .title = "V3 Controls", .visible = true, .root_json = modal_json,
  };
  CT_CHECK(ccord_contrib_set_panel_ex(F.contrib, "dev.test.v3ui", "m1", &mspec, &err) == CCORD_OK);
  frame(2);

  ccord_rect tog_r = {0}, sld_r = {0}, inp_r = {0};
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.v3ui", "m1", "v3-tog", &tog_r) && tog_r.w > 20.0f);
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.v3ui", "m1", "v3-sld", &sld_r) && sld_r.w > 20.0f);
  CT_CHECK(ui_panels_find_hit_rect(F.ui, "dev.test.v3ui", "m1", "v3-inp", &inp_r) && inp_r.w > 20.0f);

  click(tog_r.x + tog_r.w - 16.0f, tog_r.y + tog_r.h * 0.5f);
  CT_CHECK(st.panel_evs >= 1 && st.last_panel_kind == CCORD_PANEL_EV_CHANGE &&
           strcmp(st.last_panel_node, "v3-tog") == 0 && st.last_bool);

  click(sld_r.x + sld_r.w * 0.75f, sld_r.y + sld_r.h * 0.5f);
  CT_CHECK(st.last_panel_kind == CCORD_PANEL_EV_CHANGE && strcmp(st.last_panel_node, "v3-sld") == 0 && st.last_num > 50.0);

  click(inp_r.x + 12.0f, inp_r.y + inp_r.h * 0.5f);
  type_text("!");
  key(CCORD_KEY_RETURN, 0);
  CT_CHECK(st.last_panel_kind == CCORD_PANEL_EV_SUBMIT && strcmp(st.last_panel_node, "v3-inp") == 0 &&
           strstr(st.last_text, "hi!") != nullptr);
  CT_CHECK(ccord_contrib_remove_panel(F.contrib, "dev.test.v3ui", "m1"));
  frame(1);

  // 2. UI Decorators (message style/badge/accessory, member/channel/guild badge, profile section).
  ccord_decorator_contribution decs[] = {
    {.owner = "dev.test.v3ui", .decor_id = "ms", .target = CCORD_DECOR_MESSAGE_STYLE, .target_id = sample_mid,
     .accent_rgba = 0x5865F2FFu, .tint_rgba = 0x5865F218u, .collapsed = false},
    {.owner = "dev.test.v3ui", .decor_id = "mb", .target = CCORD_DECOR_MESSAGE_BADGE, .target_id = sample_mid,
     .widget_json = "{\"type\":\"badge\",\"id\":\"b-vip\",\"text\":\"VIP\"}"},
    {.owner = "dev.test.v3ui", .decor_id = "ma", .target = CCORD_DECOR_MESSAGE_ACCESSORY, .target_id = sample_mid,
     .widget_json = "{\"type\":\"text\",\"text\":\"Translated: hello\"}"},
    {.owner = "dev.test.v3ui", .decor_id = "mem", .target = CCORD_DECOR_MEMBER_BADGE, .target_id = 0,
     .widget_json = "{\"type\":\"badge\",\"text\":\"DEV\"}"},
    {.owner = "dev.test.v3ui", .decor_id = "chb", .target = CCORD_DECOR_CHANNEL_BADGE, .target_id = ch,
     .widget_json = "{\"type\":\"badge\",\"text\":\"HOT\"}"},
    {.owner = "dev.test.v3ui", .decor_id = "gb", .target = CCORD_DECOR_GUILD_BADGE, .target_id = g,
     .widget_json = "{\"type\":\"badge\",\"text\":\"3\"}"},
    {.owner = "dev.test.v3ui", .decor_id = "prof", .target = CCORD_DECOR_PROFILE_SECTION, .target_id = 0,
     .widget_json = "{\"type\":\"text\",\"text\":\"Karma: 99\"}"},
  };
  for (size_t i = 0; i < sizeof decs / sizeof decs[0]; i++) {
    CT_CHECK(ccord_contrib_set_decorator(F.contrib, &decs[i], &err) == CCORD_OK);
  }
  frame(2);

  // 3. Scoped Action Button in chat header & Local Ephemeral Message card + dismiss.
  ccord_action_btn_contribution hdr_btn = {
    .owner = "dev.test.v3ui", .btn_id = "v3-hdr", .slot = CCORD_ACTBTN_HEADER,
    .label = "V3", .tooltip = "V3 Action", .badge = "2",
  };
  CT_CHECK(ccord_contrib_add_action_btn(F.contrib, &hdr_btn, &err) == CCORD_OK);
  ccord_local_message lmsg = {
    .owner = "dev.test.v3ui", .channel_id = ch, .author_name = "V3Bot",
    .author_badge = "PLUGIN", .content = "Local ephemeral card test", .dismissible = true,
  };
  uint64_t lmsg_id = 0;
  CT_CHECK(ccord_contrib_add_local_message(F.contrib, &lmsg, &lmsg_id, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ccord_contrib_local_message_count(F.contrib, ch) == 1);

  // Click the dismiss X button on the local ephemeral message card (top-right inside card near bottom of message_area).
  const ccord_layout *lay = ccord_ui_get_layout(F.ui);
  for (float ly = lay->message_area.y + lay->message_area.h - 140.0f;
       ly < lay->message_area.y + lay->message_area.h && ccord_contrib_local_message_count(F.contrib, ch) > 0;
       ly += 6.0f) {
    click(lay->message_area.x + lay->message_area.w - 40.0f, ly);
  }
  CT_CHECK(ccord_contrib_local_message_count(F.contrib, ch) == 0);

  // 4. Schema-driven Plugin Settings Modal: open, click toggle, verify plugin_setting_changed fires.
  const char *schema =
    "[{\"key\":\"opt_on\",\"type\":\"toggle\",\"label\":\"Enable Feature\",\"default\":false},"
    " {\"key\":\"opt_n\",\"type\":\"slider\",\"label\":\"Count\",\"min\":1,\"max\":10,\"default\":3}]";
  CT_CHECK(ccord_contrib_set_settings_schema(F.contrib, "dev.test.v3ui", schema, "{\"opt_on\":false}", &err) == CCORD_OK);
  ui_plugin_settings_open(F.ui, "dev.test.v3ui");
  CT_CHECK(ui_plugin_settings_is_open(F.ui));
  frame(2);
  // Scan inside centered modal right-side where toggle lives (modal width = 560, centered at 640 -> right edge ~896).
  for (float sy = 260.0f; sy < 520.0f && st.setting_changes == 0; sy += 8.0f) {
    click(870.0f, sy);
  }
  CT_CHECK(st.setting_changes >= 1 && strcmp(st.last_setting_key, "opt_on") == 0 && strcmp(st.last_setting_val, "true") == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(1);
  CT_CHECK(!ui_plugin_settings_is_open(F.ui));

  // 5. User-initiated Plugin Dialog Modal (prompt): type text + Enter -> dialog_result callback fires.
  ccord_plugin_dialog dlg = {
    .owner = "dev.test.v3ui", .plugin_name = "V3UI", .req_id = 4242, .kind = CCORD_DIALOG_PROMPT,
    .title = "Enter Token Label", .message = "Label:", .default_value = "abc",
  };
  CT_CHECK(ccord_contrib_open_dialog(F.contrib, &dlg, &err) == CCORD_OK);
  frame(2);
  CT_CHECK(ui_plugin_dialog_is_open(F.ui));
  type_text("123");
  key(CCORD_KEY_RETURN, 0);
  frame(1);
  CT_CHECK(!ui_plugin_dialog_is_open(F.ui));
  CT_CHECK(st.dlg_results == 1 && st.last_dlg_ok && strstr(st.last_dlg_json, "abc123") != nullptr);

  // 6. Local Plugin Keybind trigger.
  ccord_keybind_contribution kb = {
    .owner = "dev.test.v3ui", .bind_id = "quick_v3", .label = "Quick V3", .default_combo = "CTRL+SHIFT+K",
  };
  CT_CHECK(ccord_contrib_add_keybind(F.contrib, &kb, &err) == CCORD_OK);
  key(CCORD_KEY_K, CCORD_MOD_CTRL | CCORD_MOD_SHIFT);
  CT_CHECK(st.kb_triggers == 1);

  // 7. Protected Security Surface Immunity: Login Screen uses protected_theme even if active theme is mutated!
  {
    ccord_bus *lbus = ccord_bus_create(&err);
    ccord_store *lstore = ccord_store_create(lbus, &err);
    login_test_transport *lt = calloc(1, sizeof *lt);
    lt->base.vt = &k_ltt_vt;
    ccord_session *lsess = ccord_session_create(lbus, lstore, &lt->base, &err);
    ccord_theme mutated = F.theme;
    mutated.c.bg_tertiary = (ccord_color){255, 0, 255, 255};   // garish magenta plugin mutation
    ccord_ui_deps ldeps = {
      .bus = lbus, .store = lstore, .session = lsess, .contrib = F.contrib,
      .theme = &mutated, .protected_theme = &F.theme, .fixed_now_unix_ms = 1793536200000LL,
    };
    ccord_ui *lui = ccord_ui_create(&ldeps, &err);
    CT_CHECK(lui != nullptr);
    ccord_ui_set_viewport(lui, 1280.0f, 800.0f);
    ccord_ui_open_account_settings(lui);
    (void)ccord_ui_update(lui, 100);
    ccord_render_begin(F.renderer, mutated.c.bg_primary);
    ccord_ui_draw(lui, F.renderer);
    const char *pshot = "ui_v3_protected_test.png";
    CT_CHECK(ccord_render_screenshot(F.renderer, pshot, &err) == CCORD_OK);
    ccord_render_end(F.renderer);
    SDL_Surface *ps = SDL_LoadPNG(pshot);
    CT_CHECK(ps != nullptr);
    if (ps) {
      // Login backdrop MUST match protected F.theme.c.bg_tertiary, NOT mutated magenta (255,0,255)!
      CT_CHECK(surf_pixel_near(ps, 20, 20, F.theme.c.bg_tertiary.r, F.theme.c.bg_tertiary.g, F.theme.c.bg_tertiary.b));
      SDL_DestroySurface(ps);
    }
    remove(pshot);
    ccord_ui_destroy(lui);
    ccord_session_destroy(lsess);
    ccord_store_destroy(lstore);
    ccord_bus_destroy(lbus);
  }

  ccord_contrib_remove_owner(F.contrib, "dev.test.v3ui");
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){.menu_activate = menu_activate});
  frame(2);
}

static float anim_slot_value(const char *ns, uint64_t id, bool *found)
{
  uint32_t key = ccord_anim_key(ns, id);
  for (size_t i = 0; i < CCORD_ANIM_SLOTS; i++) {
    if (F.ui->anim.slots[i].key == key) {
      if (found) *found = true;
      return F.ui->anim.slots[i].value;
    }
  }
  if (found) *found = false;
  return 0.0f;
}

static void test_nameplate_visibility_and_animation(void)
{
  // Minimal 16x8 2-frame VP9 WebM with alpha_mode=1 (10 fps, 100 ms per frame) used to drive real video animation.
  static const uint8_t k_alpha_vp9_webm[] = {
    0x1a, 0x45, 0xdf, 0xa3, 0x9f, 0x42, 0x86, 0x81, 0x01, 0x42, 0xf7, 0x81, 0x01, 0x42, 0xf2, 0x81,
    0x04, 0x42, 0xf3, 0x81, 0x08, 0x42, 0x82, 0x84, 0x77, 0x65, 0x62, 0x6d, 0x42, 0x87, 0x81, 0x02,
    0x42, 0x85, 0x81, 0x02, 0x18, 0x53, 0x80, 0x67, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x84,
    0x11, 0x4d, 0x9b, 0x74, 0xba, 0x4d, 0xbb, 0x8b, 0x53, 0xab, 0x84, 0x15, 0x49, 0xa9, 0x66, 0x53,
    0xac, 0x81, 0xa1, 0x4d, 0xbb, 0x8b, 0x53, 0xab, 0x84, 0x16, 0x54, 0xae, 0x6b, 0x53, 0xac, 0x81,
    0xd6, 0x4d, 0xbb, 0x8c, 0x53, 0xab, 0x84, 0x12, 0x54, 0xc3, 0x67, 0x53, 0xac, 0x82, 0x01, 0x36,
    0x4d, 0xbb, 0x8c, 0x53, 0xab, 0x84, 0x1c, 0x53, 0xbb, 0x6b, 0x53, 0xac, 0x82, 0x02, 0x6e, 0xec,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x15, 0x49, 0xa9, 0x66, 0xb0, 0x2a, 0xd7, 0xb1, 0x83, 0x0f, 0x42, 0x40, 0x4d, 0x80, 0x8c,
    0x4c, 0x61, 0x76, 0x66, 0x36, 0x33, 0x2e, 0x31, 0x2e, 0x31, 0x30, 0x32, 0x57, 0x41, 0x8c, 0x4c,
    0x61, 0x76, 0x66, 0x36, 0x33, 0x2e, 0x31, 0x2e, 0x31, 0x30, 0x32, 0x44, 0x89, 0x88, 0x40, 0x69,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x54, 0xae, 0x6b, 0xdb, 0xae, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x52, 0xd7, 0x81, 0x01, 0x73, 0xc5, 0x88, 0x93, 0x96, 0xa8, 0xab, 0x1d, 0x8b,
    0xde, 0x02, 0x9c, 0x81, 0x00, 0x22, 0xb5, 0x9c, 0x83, 0x75, 0x6e, 0x64, 0x88, 0x81, 0x00, 0x86,
    0x85, 0x56, 0x5f, 0x56, 0x50, 0x39, 0x83, 0x81, 0x01, 0x23, 0xe3, 0x83, 0x84, 0x05, 0xf5, 0xe1,
    0x00, 0xe0, 0x94, 0xb0, 0x81, 0x10, 0xba, 0x81, 0x08, 0x9a, 0x81, 0x02, 0x53, 0xc0, 0x81, 0x01,
    0x55, 0xb0, 0x84, 0x55, 0xb9, 0x81, 0x01, 0x55, 0xee, 0x81, 0x01, 0xec, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x12, 0x54, 0xc3, 0x67, 0xfe, 0x73, 0x73, 0x9f, 0x63, 0xc0,
    0x80, 0x67, 0xc8, 0x99, 0x45, 0xa3, 0x87, 0x45, 0x4e, 0x43, 0x4f, 0x44, 0x45, 0x52, 0x44, 0x87,
    0x8c, 0x4c, 0x61, 0x76, 0x66, 0x36, 0x33, 0x2e, 0x31, 0x2e, 0x31, 0x30, 0x32, 0x73, 0x73, 0xd9,
    0x63, 0xc0, 0x8b, 0x63, 0xc5, 0x88, 0x93, 0x96, 0xa8, 0xab, 0x1d, 0x8b, 0xde, 0x02, 0x67, 0xc8,
    0xa4, 0x45, 0xa3, 0x87, 0x45, 0x4e, 0x43, 0x4f, 0x44, 0x45, 0x52, 0x44, 0x87, 0x97, 0x4c, 0x61,
    0x76, 0x63, 0x36, 0x33, 0x2e, 0x31, 0x2e, 0x31, 0x30, 0x32, 0x20, 0x6c, 0x69, 0x62, 0x76, 0x70,
    0x78, 0x2d, 0x76, 0x70, 0x39, 0x67, 0xc8, 0xa1, 0x45, 0xa3, 0x88, 0x44, 0x55, 0x52, 0x41, 0x54,
    0x49, 0x4f, 0x4e, 0x44, 0x87, 0x93, 0x30, 0x30, 0x3a, 0x30, 0x30, 0x3a, 0x30, 0x30, 0x2e, 0x32,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x1f, 0x43, 0xb6, 0x75, 0x40, 0xaf, 0xe7,
    0x81, 0x00, 0xa0, 0xe5, 0xa1, 0xb4, 0x81, 0x00, 0x00, 0x00, 0x82, 0x49, 0x83, 0x42, 0x00, 0x00,
    0xf0, 0x00, 0x76, 0x00, 0x38, 0x24, 0x1c, 0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x0b, 0x51,
    0xff, 0xff, 0xff, 0x6d, 0x5f, 0x8a, 0x7f, 0xff, 0xff, 0xdc, 0xc9, 0x80, 0x8a, 0xff, 0xff, 0xff,
    0xd9, 0xf9, 0x35, 0x7f, 0xff, 0xff, 0xfb, 0x64, 0x60, 0x00, 0x75, 0xa1, 0xac, 0xa6, 0xaa, 0xee,
    0x81, 0x01, 0xa5, 0xa5, 0x82, 0x49, 0x83, 0x42, 0x00, 0x00, 0xf0, 0x00, 0x76, 0x00, 0x38, 0x24,
    0x1c, 0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x0b, 0x51, 0xff, 0xff, 0xff, 0x75, 0x7e, 0x25,
    0x3f, 0xff, 0xff, 0xfa, 0xfe, 0xa0, 0xc0, 0x00, 0x00, 0xa0, 0xc3, 0xa1, 0xa6, 0x81, 0x00, 0x64,
    0x00, 0x86, 0x00, 0x40, 0x92, 0x9c, 0x00, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x5a, 0xfd, 0x9b,
    0xff, 0xff, 0xed, 0xa4, 0x05, 0x37, 0xff, 0xff, 0xff, 0x6c, 0x8c, 0x1f, 0xff, 0xff, 0xff, 0xe3,
    0x15, 0x00, 0x00, 0x75, 0xa1, 0x95, 0xa6, 0x93, 0xee, 0x81, 0x01, 0xa5, 0x8e, 0x86, 0x00, 0x40,
    0x92, 0x9c, 0x00, 0x40, 0x00, 0x00, 0x02, 0x00, 0x00, 0x1c, 0x48, 0xfb, 0x81, 0x9c, 0x1c, 0x53,
    0xbb, 0x6b, 0x91, 0xbb, 0x8f, 0xb3, 0x81, 0x00, 0xb7, 0x8a, 0xf7, 0x81, 0x01, 0xf1, 0x82, 0x01,
    0xb9, 0xf0, 0x81, 0x03,
  };
  static const char *const k_dm_asset = "nameplates/nameplates/american_flag_dm_test/";
  static const char *const k_dm_static =
      "https://cdn.discordapp.com/assets/collectibles/nameplates/nameplates/american_flag_dm_test/static.png";
  static const char *const k_dm_anim =
      "https://cdn.discordapp.com/assets/collectibles/nameplates/nameplates/american_flag_dm_test/asset.webm";
  static const char *const k_mem_asset = "nameplates/nameplates/american_flag_mem_test/";
  static const char *const k_mem_static =
      "https://cdn.discordapp.com/assets/collectibles/nameplates/nameplates/american_flag_mem_test/static.png";
  static const char *const k_mem_anim =
      "https://cdn.discordapp.com/assets/collectibles/nameplates/nameplates/american_flag_mem_test/asset.webm";

  ccord_error err;
  bool saved_rm_set = F.ui->settings_reduced_motion, saved_rm_anim = F.ui->anim.reduced_motion;
  F.ui->settings_reduced_motion = false;
  F.ui->anim.reduced_motion = false;
  F.ui->member_popover_open = false;
  F.ui->modal_kind = UI_MODAL_NONE;
  F.ui->menu.open = false;
  F.ui->settings_open = false;

  // ---- Part 1: DM list nameplate visibility & animation ----
  ccord_user dm_u = {
    .id = 880101u,
    .name = "PatriotDM",
    .handle = "patriotdm",
    .bio = "",
    .pronouns = "",
    .presence = CCORD_PRESENCE_ONLINE,
  };
  snprintf(dm_u.nameplate_asset, sizeof dm_u.nameplate_asset, "%s", k_dm_asset);
  snprintf(dm_u.nameplate_palette, sizeof dm_u.nameplate_palette, "cobalt");
  CT_CHECK(ccord_store_upsert_user(F.store, &dm_u, &err) == CCORD_OK);

  ccord_channel dm_ch = {
    .id = 880201u,
    .guild_id = CCORD_DM_GUILD_ID,
    .kind = CCORD_CHANNEL_TEXT,
    .name = "PatriotDM",
    .topic = "",
    .user_id = dm_u.id,
    .last_message_id = 999999999999ULL,
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_ch, &err) == CCORD_OK);

  // Switch to DM rail on Friends view (so dm_ch is visible in the DM list, but NOT actively viewed).
  CT_CHECK(ccord_store_select_guild(F.store, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  F.ui->dm_friends_view = true;
  F.ui->dm_requests_view = false;
  F.ui->chan_scroll = 0.0f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);
  frame(3);

  // Locate dm_ch row in the DM list.
  const ccord_layout_rect *cs = &F.ui->layout.channel_scroll;
  float dm_row_x = cs->x + cs->w * 0.5f;
  float dm_row_y = -1.0f;
  for (size_t i = 0; i < F.ui->ncrow; i++) {
    const chan_row *r = &F.ui->crow[i];
    if (r->dm_nav != DM_NAV_NONE || r->is_category || r->is_voice_user) continue;
    const ccord_channel *ch = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, r->index);
    if (ch && ch->id == dm_ch.id) {
      dm_row_y = cs->y + r->y - F.ui->chan_scroll + r->h * 0.5f;
      break;
    }
  }
  CT_CHECK(dm_row_y > cs->y && dm_row_y < cs->y + cs->h);

  // Idle (neither hovered nor selected): DM nameplate must NOT be displayed or fetched at all.
  bool np_found = false;
  float np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v == 0.0f);
  CT_CHECK(ccord_session_get_media(F.session, k_dm_static) == nullptr);
  CT_CHECK(ccord_session_get_media(F.session, k_dm_anim) == nullptr);
  CT_CHECK(!ccord_render_has_texture(F.renderer, k_dm_static, nullptr, nullptr));
  CT_CHECK(!ccord_render_has_texture(F.renderer, k_dm_anim, nullptr, nullptr));

  // Hovering the DM row smoothly fades the nameplate in (does not snap to 1.0) and requests asset.webm.
  mouse(CCORD_IN_MOUSE_MOVE, 0, dm_row_x, dm_row_y);
  frame(1);
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v == 0.0f && ccord_anim_active(&F.ui->anim));
  frame(1);   // +50ms into the 200ms fade-in: strictly intermediate
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v > 0.05f && np_v < 0.95f && ccord_anim_active(&F.ui->anim));
  CT_CHECK(ccord_session_get_media(F.session, k_dm_anim) != nullptr ||
           ccord_render_has_texture(F.renderer, k_dm_anim, nullptr, nullptr));
  frame(4);   // settles at 1.0
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v >= 0.99f);

  // Upload the real 2-frame VP9 alpha WebM under k_dm_anim and verify playback tracks hover & active view.
  ccord_media_opts vopts = {.max_dim = 512, .allow_video = true};
  CT_CHECK(ccord_render_upload_media(F.renderer, k_dm_anim, k_alpha_vp9_webm, sizeof k_alpha_vp9_webm,
                                     &vopts, nullptr, nullptr, &err) == CCORD_OK);
  size_t cur_f = 0, cnt_f = 0;
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, k_dm_anim, &cur_f, &cnt_f) && cnt_f == 2u);

  ccord_render_set_anim_clock(F.renderer, 1000u);
  draw_only();
  CT_CHECK(F.ui->media_animating);

  // Unhover while still on Friends view (not actively viewed): animation stops immediately while opacity eases out.
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);
  frame(1);
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v >= 0.95f && ccord_anim_active(&F.ui->anim) && !F.ui->media_animating);
  frame(1);   // +50ms into the 150ms fade-out: strictly intermediate
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v > 0.05f && np_v < 0.95f && !F.ui->media_animating);
  frame(3);   // settles at 0.0
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v <= 0.01f && !F.ui->media_animating);

  // Reduced motion snaps DM nameplate immediately without fading.
  F.ui->anim.reduced_motion = true;
  mouse(CCORD_IN_MOUSE_MOVE, 0, dm_row_x, dm_row_y);
  frame(1);
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v >= 0.99f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);
  frame(1);
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v <= 0.01f);
  F.ui->anim.reduced_motion = false;

  // Click the DM row to actively view it, then move mouse away: nameplate stays visible (static) without
  // continuously decoding asset.webm while idle; hovering the selected DM row plays the WebM animation.
  click(dm_row_x, dm_row_y);
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);
  frame(5);
  CT_CHECK(ccord_store_selected_channel(F.store) == dm_ch.id && !F.ui->dm_friends_view);
  np_v = anim_slot_value("dmnp", dm_ch.id * 2u, &np_found);
  CT_CHECK(np_found && np_v >= 0.99f);
  ccord_render_set_anim_clock(F.renderer, 1000u);
  draw_only();
  CT_CHECK(!F.ui->media_animating);
  mouse(CCORD_IN_MOUSE_MOVE, 0, dm_row_x, dm_row_y);
  ccord_render_set_anim_clock(F.renderer, 1000u);
  draw_only();
  CT_CHECK(F.ui->media_animating);
  ccord_render_set_anim_clock(F.renderer, 1120u);
  draw_only();
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, k_dm_anim, &cur_f, &cnt_f) && cur_f == 1u);
  // Reduced motion stops DM nameplate animation even while hovered.
  F.ui->anim.reduced_motion = true;
  draw_only();
  CT_CHECK(!F.ui->media_animating);
  F.ui->anim.reduced_motion = false;
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);

  // Async nameplate arrival on an already-selected DM transitions smoothly in instead of snapping.
  ccord_user dm_u2 = {
    .id = 880103u,
    .name = "LateNameplateDM",
    .handle = "latenameplatedm",
    .bio = "",
    .pronouns = "",
    .presence = CCORD_PRESENCE_ONLINE,
  };
  CT_CHECK(ccord_store_upsert_user(F.store, &dm_u2, &err) == CCORD_OK);
  ccord_channel dm_ch2 = {
    .id = 880202u,
    .guild_id = CCORD_DM_GUILD_ID,
    .kind = CCORD_CHANNEL_TEXT,
    .name = "LateNameplateDM",
    .topic = "",
    .user_id = dm_u2.id,
    .last_message_id = 999999999998ULL,
  };
  CT_CHECK(ccord_store_upsert_channel(F.store, &dm_ch2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, dm_ch2.id, &err) == CCORD_OK);
  F.ui->dm_friends_view = false;
  frame(2);
  np_v = anim_slot_value("dmnp", dm_ch2.id * 2u, &np_found);
  CT_CHECK(np_found && np_v == 0.0f);
  snprintf(dm_u2.nameplate_asset, sizeof dm_u2.nameplate_asset, "%s", k_dm_asset);
  snprintf(dm_u2.nameplate_palette, sizeof dm_u2.nameplate_palette, "cobalt");
  CT_CHECK(ccord_store_upsert_user(F.store, &dm_u2, &err) == CCORD_OK);
  frame(1);
  np_v = anim_slot_value("dmnp", dm_ch2.id * 2u, &np_found);
  CT_CHECK(np_found && np_v == 0.0f && ccord_anim_active(&F.ui->anim));
  frame(1);
  np_v = anim_slot_value("dmnp", dm_ch2.id * 2u, &np_found);
  CT_CHECK(np_found && np_v > 0.05f && np_v < 0.95f);
  frame(4);
  np_v = anim_slot_value("dmnp", dm_ch2.id * 2u, &np_found);
  CT_CHECK(np_found && np_v >= 0.99f);

  // ---- Part 2: Members tab nameplate (static on idle/hover, animated only when profile clicked) ----
  ccord_guild np_guild = {.id = 880900u, .name = "Nameplate Guild", .icon_url = ""};
  ccord_channel np_ch = {.id = 880901u, .guild_id = np_guild.id, .kind = CCORD_CHANNEL_TEXT,
                         .discord_type = CCORD_DISCORD_CHANNEL_GUILD_TEXT, .name = "general", .topic = ""};
  ccord_user mem_u = {
    .id = 880102u,
    .name = "PatriotMember",
    .handle = "patriotmember",
    .bio = "",
    .pronouns = "",
    .presence = CCORD_PRESENCE_ONLINE,
  };
  snprintf(mem_u.nameplate_asset, sizeof mem_u.nameplate_asset, "%s", k_mem_asset);
  snprintf(mem_u.nameplate_palette, sizeof mem_u.nameplate_palette, "crimson");
  ccord_member mem_m = {.guild_id = np_guild.id, .user_id = mem_u.id, .role_name = "", .nick = ""};
  CT_CHECK(ccord_store_upsert_guild(F.store, &np_guild, &np_ch, 1, &mem_m, 1, &mem_u, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(F.store, np_guild.id, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(F.store, np_ch.id, &err) == CCORD_OK);
  F.ui->members_hidden = false;
  F.ui->member_scroll = 0.0f;
  mouse(CCORD_IN_MOUSE_MOVE, 0, 5.0f, 5.0f);
  frame(3);
  CT_CHECK(ccord_ui_get_layout(F.ui)->member_list_visible);

  // Locate mem_u row in the Members tab.
  const ccord_layout_rect *ml = &F.ui->layout.member_list;
  float mem_row_x = ml->x + ml->w * 0.5f;
  float mem_row_y = -1.0f;
  for (size_t i = 0; i < F.ui->nmrow; i++) {
    const member_row *mr = &F.ui->mrow[i];
    if (mr->is_header) continue;
    const ccord_member *m = ccord_store_member_at(F.store, np_guild.id, mr->index);
    if (m && m->user_id == mem_u.id) {
      mem_row_y = ml->y + mr->y - F.ui->member_scroll + mr->h * 0.5f;
      break;
    }
  }
  CT_CHECK(mem_row_y > ml->y && mem_row_y < ml->y + ml->h);

  // Idle in Members tab: static.png IS shown/fetched, asset.webm is NOT fetched, and nothing animates.
  CT_CHECK(ccord_session_get_media(F.session, k_mem_static) != nullptr ||
           ccord_render_has_texture(F.renderer, k_mem_static, nullptr, nullptr));
  CT_CHECK(ccord_session_get_media(F.session, k_mem_anim) == nullptr);
  CT_CHECK(!ccord_render_has_texture(F.renderer, k_mem_anim, nullptr, nullptr));
  CT_CHECK(!F.ui->media_animating);

  // Hovering the member row in Members tab eases hover intensity smoothly while staying NON-animated.
  np_v = anim_slot_value("memnp", mem_u.id * 2u, &np_found);
  CT_CHECK(np_found && np_v == 0.0f);
  mouse(CCORD_IN_MOUSE_MOVE, 0, mem_row_x, mem_row_y);
  frame(1);
  np_v = anim_slot_value("memnp", mem_u.id * 2u, &np_found);
  CT_CHECK(np_found && np_v == 0.0f && ccord_anim_active(&F.ui->anim));
  frame(1);   // +50ms into the 160ms hover boost
  np_v = anim_slot_value("memnp", mem_u.id * 2u, &np_found);
  CT_CHECK(np_found && np_v > 0.05f && np_v < 0.95f);
  frame(3);
  np_v = anim_slot_value("memnp", mem_u.id * 2u, &np_found);
  CT_CHECK(np_found && np_v >= 0.99f);
  CT_CHECK(ccord_session_get_media(F.session, k_mem_anim) == nullptr);
  CT_CHECK(!ccord_render_has_texture(F.renderer, k_mem_anim, nullptr, nullptr));
  CT_CHECK(!F.ui->media_animating);

  // Clicking the member opens their profile popover and starts animating their nameplate.
  click(mem_row_x, mem_row_y);
  frame(2);
  CT_CHECK(F.ui->member_popover_open && F.ui->member_popover_user_id == mem_u.id);
  CT_CHECK(ccord_session_get_media(F.session, k_mem_anim) != nullptr ||
           ccord_render_has_texture(F.renderer, k_mem_anim, nullptr, nullptr));

  // Upload real 2-frame WebM for member nameplate and verify animation runs only while profile is open.
  CT_CHECK(ccord_render_upload_media(F.renderer, k_mem_anim, k_alpha_vp9_webm, sizeof k_alpha_vp9_webm,
                                     &vopts, nullptr, nullptr, &err) == CCORD_OK);
  ccord_render_set_anim_clock(F.renderer, 2000u);
  draw_only();
  CT_CHECK(F.ui->media_animating);
  ccord_render_set_anim_clock(F.renderer, 2120u);
  draw_only();
  CT_CHECK(ccord_render_texture_is_animated(F.renderer, k_mem_anim, &cur_f, &cnt_f) && cur_f == 1u);

  // Closing the profile popover returns the member row to non-animated even if hovered.
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!F.ui->member_popover_open);
  mouse(CCORD_IN_MOUSE_MOVE, 0, mem_row_x, mem_row_y);
  draw_only();
  CT_CHECK(!F.ui->media_animating);

  // Opening the full user profile modal for that member also animates their nameplate.
  F.ui->modal_kind = UI_MODAL_USER_PROFILE;
  F.ui->modal_target_id = mem_u.id;
  draw_only();
  CT_CHECK(F.ui->media_animating);
  F.ui->modal_kind = UI_MODAL_NONE;
  F.ui->modal_target_id = CCORD_ID_NONE;
  draw_only();
  CT_CHECK(!F.ui->media_animating);

  ccord_render_set_anim_clock(F.renderer, 0u);
  F.ui->settings_reduced_motion = saved_rm_set;
  F.ui->anim.reduced_motion = saved_rm_anim;
}

// Refocusing while already viewing a DM clears its unread/mention counts, opening the newest message
// via ccord_ui_open_message preserves stick_bottom so auto-ack stays active, and popovers never swallow CCORD_IN_FOCUS.
static void test_same_dm_refocus_and_open_latest_message(void)
{
  ccord_error err;
  ccord_id prev_guild = ccord_store_selected_guild(F.store);
  ccord_id prev_chan = ccord_store_selected_channel(F.store);

  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  CT_CHECK(ccord_session_open_dm(F.session, 1002u, &err) == CCORD_OK);
  frame(8);
  ccord_id dm_ch = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(F.store, CCORD_DM_GUILD_ID); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, CCORD_DM_GUILD_ID, i);
    if (c && c->user_id == 1002u) { dm_ch = c->id; break; }
  }
  CT_CHECK(dm_ch != CCORD_ID_NONE);
  if (dm_ch == CCORD_ID_NONE) return;
  CT_CHECK(ccord_session_select_guild(F.session, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
  CT_CHECK(ccord_session_select_channel(F.session, dm_ch, &err) == CCORD_OK);
  frame(20);
  CT_CHECK(ccord_store_selected_guild(F.store) == CCORD_DM_GUILD_ID && ccord_store_selected_channel(F.store) == dm_ch);
  const ccord_channel *dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count == 0 && dc->mention_count == 0 && dbg().stick_bottom);

  // 1. Unfocus while viewing this DM, receive a message, then refocus: unread & mention counts clear.
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  ccord_id msg1 = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, dm_ch, 1002u, "hello in same DM", nullptr, &msg1, &err) == CCORD_OK);
  frame(4);
  dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count == 1 && dc->mention_count == 1);
  // Popovers (e.g. account status card) must never swallow CCORD_IN_FOCUS.
  F.ui->sc.open = true;
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  F.ui->sc.open = false;
  CT_CHECK(F.ui->window_focused && ui_is_reading(F.ui));
  frame(25);
  dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count == 0 && dc->mention_count == 0 && dc->last_read_id >= msg1);

  // 2. Clicking a notification for the newest message via ccord_ui_open_message keeps stick_bottom=true so auto-ack clears it.
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = false});
  ccord_id msg2 = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, dm_ch, 1002u, "newest dm message", nullptr, &msg2, &err) == CCORD_OK);
  frame(4);
  dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count == 1 && dc->mention_count == 1);
  ccord_ui_open_message(F.ui, CCORD_DM_GUILD_ID, dm_ch, msg2);
  CT_CHECK(dbg().stick_bottom);
  ui_send((ccord_input_event){.type = CCORD_IN_FOCUS, .focused = true});
  CT_CHECK(ui_is_reading(F.ui));
  frame(25);
  dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count == 0 && dc->mention_count == 0 && dc->last_read_id >= msg2 && dbg().stick_bottom);

  // 3. Stale unread/mention counts when last_read_id is already > latest local message still clear without rolling back.
  ccord_id msg3 = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, dm_ch, 1002u, "later dm message", nullptr, &msg3, &err) == CCORD_OK);
  frame(4);
  CT_CHECK(ccord_store_apply_read(F.store, dm_ch, msg3, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_message(F.store, dm_ch, msg3, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(F.store, dm_ch, 1u, 1u, &err) == CCORD_OK);
  F.ui->ack_channel = CCORD_ID_NONE;
  F.ui->acked_message = CCORD_ID_NONE;
  F.ui->ack_next_ms = 0;
  frame(4);
  dc = ccord_store_channel(F.store, dm_ch);
  CT_CHECK(dc && dc->unread_count == 0 && dc->mention_count == 0 && dc->last_read_id == msg3);

  if (prev_guild != CCORD_ID_NONE) (void)ccord_session_select_guild(F.session, prev_guild, &err);
  if (prev_chan != CCORD_ID_NONE) (void)ccord_session_select_channel(F.session, prev_chan, &err);
  frame(4);
}


static void test_power_efficiency_and_wave2_helpers(void)
{
  // Wave 2 state accessors.
  CT_CHECK(!ccord_ui_voice_stage_visible(nullptr));
  CT_CHECK(!ccord_ui_stream_preview_hidden(nullptr));
  CT_CHECK(!ccord_ui_streamer_mode(nullptr));

  bool prev_sm = F.ui->settings_streamer_mode;
  F.ui->settings_streamer_mode = true;
  CT_CHECK(ccord_ui_streamer_mode(F.ui));
  F.ui->settings_streamer_mode = false;
  CT_CHECK(!ccord_ui_streamer_mode(F.ui));
  F.ui->settings_streamer_mode = prev_sm;

  bool prev_hp = F.ui->stream_hide_preview;
  F.ui->stream_hide_preview = true;
  CT_CHECK(ccord_ui_stream_preview_hidden(F.ui));
  F.ui->stream_hide_preview = false;
  CT_CHECK(!ccord_ui_stream_preview_hidden(F.ui));
  F.ui->stream_hide_preview = prev_hp;

  ccord_id prev_stage = F.ui->stage_channel;
  bool prev_set = F.ui->settings_open;
  bool prev_fs = F.ui->stream_fullscreen;
  bool prev_disc = F.ui->discovery_open;
  F.ui->stage_channel = CCORD_ID_NONE;
  CT_CHECK(!ccord_ui_voice_stage_visible(F.ui));
  F.ui->stage_channel = 123456u;
  F.ui->settings_open = false;
  F.ui->stream_fullscreen = false;
  F.ui->discovery_open = false;
  CT_CHECK(ccord_ui_voice_stage_visible(F.ui));
  F.ui->settings_open = true;
  CT_CHECK(!ccord_ui_voice_stage_visible(F.ui));
  F.ui->settings_open = false;
  F.ui->stream_fullscreen = true;
  CT_CHECK(!ccord_ui_voice_stage_visible(F.ui));
  F.ui->stream_fullscreen = prev_fs;
  F.ui->settings_open = prev_set;
  F.ui->discovery_open = prev_disc;
  F.ui->stage_channel = prev_stage;

  // Jump highlight 700 ms static hold sleeps instead of spinning at VSync.
  frame(10);   // settle transitions
  F.ui->jump_highlight_msg_id = 424242u;
  F.ui->jump_highlight_until_ms = F.now + UI_JUMP_FLASH_MS;
  (void)ccord_ui_update(F.ui, F.now);
  uint32_t jw = ccord_ui_next_wait_ms(F.ui, F.now);
  CT_CHECK(jw >= 600u && jw <= UI_JUMP_HOLD_MS);
  // Once past the static hold, fade animates smoothly (0 ms wait).
  (void)ccord_ui_update(F.ui, F.now + UI_JUMP_HOLD_MS + 10u);
  CT_CHECK(ccord_ui_next_wait_ms(F.ui, F.now + UI_JUMP_HOLD_MS + 10u) == 0u);
  // After UI_JUMP_FLASH_MS, highlight clears cleanly.
  (void)ccord_ui_update(F.ui, F.now + UI_JUMP_FLASH_MS + 10u);
  CT_CHECK(F.ui->jump_highlight_msg_id == CCORD_ID_NONE);

  // Status card 1.5 s confirm hold reports exact remaining deadline instead of 0 ms spin.
  F.now += UI_JUMP_FLASH_MS + 50u;
  F.ui->sc.open = true;
  F.ui->sc.confirm_until_ms = F.now + 1500u;
  (void)ccord_ui_update(F.ui, F.now);
  uint32_t scw = ccord_ui_next_wait_ms(F.ui, F.now);
  CT_CHECK(scw > 1000u && scw <= 1500u);
  CT_CHECK(ccord_ui_update(F.ui, F.now + 1500u));
  CT_CHECK(F.ui->sc.confirm_until_ms == 0u);
  F.ui->sc.open = false;

  // Loading screen paces updates at ~33 ms instead of uncapped VSync spin.
  F.now += 1600u;
  ccord_ui_set_loading_screen(F.ui, true);
  (void)ccord_ui_update(F.ui, F.now);
  uint32_t lw = ccord_ui_next_wait_ms(F.ui, F.now);
  CT_CHECK(lw > 0u && lw <= 33u);
  ccord_ui_set_loading_screen(F.ui, false);
  (void)ccord_ui_update(F.ui, F.now + 50u);
  F.now += 50u;
}

typedef struct tl_ev_log {
  int count;
  uint64_t last_lid;
  char last_type[32];
} tl_ev_log;

static void on_tl_local_ev(void *ud, const ccord_local_message *m, const char *ev_type, float x, float y)
{
  (void)x; (void)y;
  tl_ev_log *l = ud;
  l->count++;
  l->last_lid = m ? m->local_id : 0;
  snprintf(l->last_type, sizeof l->last_type, "%s", ev_type ? ev_type : "");
}

static void test_positioned_timeline_messages_and_style(void)
{
  ccord_error err = {0};
  const ccord_guild *g0 = ccord_store_guild_at(F.store, 0);
  ccord_id gid = g0 ? g0->id : CCORD_ID_NONE;
  ccord_id ch = gid != CCORD_ID_NONE ? find_channel(gid, "general", CCORD_CHANNEL_TEXT) : CCORD_ID_NONE;
  if (gid != CCORD_ID_NONE && ch != CCORD_ID_NONE) {
    (void)ccord_store_select_guild(F.store, gid, &err);
    (void)ccord_store_select_channel(F.store, ch, &err);
    frame(2);
  }
  ch = ccord_store_selected_channel(F.store);
  size_t base_n = ccord_store_message_count(F.store, ch);
  CT_CHECK(ch != CCORD_ID_NONE && base_n >= 2);
  if (base_n < 2) return;

  const ccord_message *m_prev = ccord_store_message_at(F.store, ch, base_n - 2);
  const ccord_message *m_last = ccord_store_message_at(F.store, ch, base_n - 1);
  CT_CHECK(m_prev != nullptr && m_last != nullptr);
  if (!m_prev || !m_last) return;

  tl_ev_log evlog = {0};
  ccord_contrib_set_handlers(F.contrib, &(ccord_contrib_handlers){
    .ud = &evlog,
    .local_msg_event = on_tl_local_ev,
  });

  // Upload a tiny 8x8 RGBA texture for inline local attachment preview.
  uint8_t px[8 * 8 * 4];
  for (int i = 0; i < 64; i++) {
    px[i * 4 + 0] = 220; px[i * 4 + 1] = 60; px[i * 4 + 2] = 60; px[i * 4 + 3] = 255;
  }
  CT_CHECK(ccord_contrib_set_texture(F.contrib, "dev.test.tlui", "tl_thumb", px, sizeof px, 8, 8, true, &err) == CCORD_OK);

  // 1. Insert a timeline local message anchored immediately AFTER m_prev (so it sits between m_prev and m_last!)
  //    with same author_id + compact=AUTO within 7 min -> merges compactly under m_prev,
  //    AND break_group=true -> forces m_last to draw a full non-compact header even if same author!
  uint64_t lid_after = 0;
  ccord_local_message lm1 = {
    .owner = "dev.test.tlui", .client_id = "del-mid", .channel_id = ch,
    .position = CCORD_LOCAL_POS_TIMELINE,
    .after_message_id = m_prev->id,
    .author_id = m_prev->author_id, .author_name = "DeletedAuthor",
    .author_color = "#e5484d", .text_color = "#e5484d",
    .author_badge = "DELETED", .badge_color = "#e5484d",
    .content = "This **deleted** message sits right between live rows",
    .attachments_json = "[{\"filename\":\"archived.png\",\"texture_key\":\"tl_thumb\",\"width\":8,\"height\":8}]",
    .embed_json = "[{\"title\":\"Archived Embed\",\"description\":\"Stored locally\",\"color\":15026253}]",
    .timestamp_ms = (uint64_t)(m_prev->timestamp_ms + 10000),
    .avatar_grayscale = true, .has_avatar_opacity = true, .avatar_opacity = 0.45f,
    .compact_mode = CCORD_LOCAL_COMPACT_AUTO, .break_group = true,
    .clip_to_history = true, .dismissible = true, .has_on_event = true,
  };
  CT_CHECK(ccord_contrib_add_local_message(F.contrib, &lm1, &lid_after, &err) == CCORD_OK);

  // 2. Insert a second timeline local message after m_last with compact=NEVER (full native avatar gutter + red username).
  uint64_t lid_tail = 0;
  ccord_local_message lm2 = {
    .owner = "dev.test.tlui", .client_id = "del-tail", .channel_id = ch,
    .position = CCORD_LOCAL_POS_TIMELINE,
    .sort_id = m_last->id + 100u,
    .author_id = m_last->author_id, .author_name = "TailDeleted",
    .author_color = "danger", .text_color = "#e5484d",
    .avatar_grayscale = true, .has_avatar_opacity = true, .avatar_opacity = 0.5f,
    .content = "Tail deleted header message",
    .compact_mode = CCORD_LOCAL_COMPACT_NEVER, .dismissible = true, .has_on_event = true,
  };
  CT_CHECK(ccord_contrib_add_local_message(F.contrib, &lm2, &lid_tail, &err) == CCORD_OK);

  // 3. Apply extended ui.decorate.message_style to m_last (text_color, author_color, avatar_grayscale, dim).
  ccord_decorator_contribution mst = {
    .owner = "dev.test.tlui", .decor_id = "st1", .target = CCORD_DECOR_MESSAGE_STYLE, .target_id = m_last->id,
    .text_color = "#ffa500", .author_color = "danger", .avatar_grayscale = true, .dim = true,
  };
  CT_CHECK(ccord_contrib_set_decorator(F.contrib, &mst, &err) == CCORD_OK);

  frame(3);

  // Total view count = base_n + 2 timeline locals.
  CT_CHECK(ui_view_count(F.ui) == base_n + 2u);
  size_t vi_prev = ui_timeline_view_index_for_msg_id(F.ui, ch, m_prev->id);
  size_t vi_lm1 = ui_timeline_view_index_for_local_id(F.ui, ch, lid_after);
  size_t vi_last = ui_timeline_view_index_for_msg_id(F.ui, ch, m_last->id);
  size_t vi_lm2 = ui_timeline_view_index_for_local_id(F.ui, ch, lid_tail);
  CT_CHECK(vi_prev != SIZE_MAX && vi_lm1 == vi_prev + 1u && vi_last == vi_lm1 + 1u && vi_lm2 == vi_last + 1u);

  // Check compact grouping rules: lm1 merged compactly under m_prev; m_last broke group after lm1; lm2 has full header.
  CT_CHECK(ui_view_is_compact(F.ui, ch, vi_lm1));
  CT_CHECK(!ui_view_is_compact(F.ui, ch, vi_last));
  CT_CHECK(!ui_view_is_compact(F.ui, ch, vi_lm2));

  // Locate lm2 in visible rows -> has real avatar gutter rect AND author rect.
  const vis_row *vr_lm2 = nullptr;
  for (size_t i = 0; i < F.ui->nvis; i++) {
    if (F.ui->vis[i].is_local && F.ui->vis[i].local_id == lid_tail) { vr_lm2 = &F.ui->vis[i]; break; }
  }
  CT_CHECK(vr_lm2 != nullptr && vr_lm2->avatar_rect.w > 20.0f && vr_lm2->author_rect.w > 10.0f);

  if (vr_lm2) {
    float cx = F.ui->layout.message_area.x + 160.0f;
    float cy = vr_lm2->y + vr_lm2->h * 0.5f;
    // Left-click dispatches local_msg_event("click").
    click(cx, cy);
    CT_CHECK(evlog.count >= 1 && evlog.last_lid == lid_tail && strcmp(evlog.last_type, "click") == 0);
    // Right-click dispatches local_msg_event("right_click") and does NOT open native message context menu!
    mouse(CCORD_IN_MOUSE_DOWN, 3, cx, cy);
    mouse(CCORD_IN_MOUSE_UP, 3, cx, cy);
    frame(1);
    CT_CHECK(evlog.last_lid == lid_tail && strcmp(evlog.last_type, "right_click") == 0);
    CT_CHECK(!F.ui->menu.open);
  }

  // Cleanup owner removes both timeline locals, decorator, and texture, restoring exact base_n count.
  ccord_contrib_remove_owner(F.contrib, "dev.test.tlui");
  frame(2);
  CT_CHECK(ui_view_count(F.ui) == base_n);
}


int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_interaction_ext")) {
    fprintf(stderr, "test_ui_interaction_ext: setup failed or no display driver; skipping\n");
    return 77;
  }
  frame(4);
  test_expr_picker_fetch_and_emoji_rail();
  test_search_preserves_timeline();
  test_dm_friends_scroll_thumb();
  test_stage_tiles_scroll_thumb();
  test_call_bar_inset_and_cursor();
  test_copy_link_mark_unread_save();
  test_saved_messages_popover_and_persistence();
  test_forward_picker();
  test_tts_command_and_badge();
  test_crosspost_announcement();
  test_slash_subcommand_tree();
  test_context_menu_app_commands();
  test_membership_screening_modal_and_composer_lock();
  test_issue3_auto_ack_and_input_guards();
  test_dm_profile_right_click_menu();
  test_invite_preview_cards_and_url_opener();
  test_plugin_ui_panels();
  test_composer_upload_progress_indicator();
  test_plugin_nav_hooks_menus_and_aux_events();
  test_chat_message_vs_profile_right_click_and_admin_tools();
  test_reading_overlays_immediate_focus_and_utf8_notify();
  test_dm_and_substring_mention_autocomplete();
  test_plugin_api_v3_ui();
  test_nameplate_visibility_and_animation();
  test_same_dm_refocus_and_open_latest_message();
  test_power_efficiency_and_wave2_helpers();
  test_positioned_timeline_messages_and_style();
  ui_test_teardown();
  ct_failures += ui_test_harness_failures();
  return CT_RESULT("test_ui_interaction_ext");
}
