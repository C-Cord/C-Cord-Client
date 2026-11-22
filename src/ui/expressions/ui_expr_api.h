// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// ---- expression_picker.c ----
void ui_expr_picker_destroy(ccord_ui *ui);
void ui_expr_picker_open(ccord_ui *ui, int tab, ccord_id channel_id, ccord_id reaction_msg_id);
void ui_expr_picker_close(ccord_ui *ui);
bool ui_expr_wants_text(const ccord_ui *ui);
void ui_expr_picker_draw(ccord_ui *ui, ccord_renderer *r);
void ui_expr_draw_composer_buttons(ccord_ui *ui, ccord_renderer *r, ccord_rect box);
void ui_expr_draw_smiley(ccord_renderer *r, float cx, float cy, float size, ccord_color c);
void ui_expr_draw_custom_emoji(ccord_ui *ui, ccord_renderer *r, ccord_id id, const char *name, size_t nlen,
                               bool animated, ccord_rect rect);
void ui_expr_draw_sticker_art(ccord_ui *ui, ccord_renderer *r, ccord_id id, const char *name, size_t nlen,
                              uint8_t format_type, ccord_rect rect, bool inline_chat);
bool ui_expr_handle_event(ccord_ui *ui, const ccord_input_event *ev);
bool ui_expr_handle_composer_click(ccord_ui *ui, float mx, float my);
size_t ui_expr_replace_shortcodes(const ccord_store *store, ccord_id preferred_guild_id,
                                  const char *in, size_t in_len, char *out, size_t out_cap);
// Read-only inspection of the expression picker for tests & verify slices. All return false (and zero
// the output) when the picker was never opened or the index is out of range; nothing is mutated.
typedef struct ui_expr_rail_info {
  ccord_rect rect;
  int section_id;          // -1 Frequently Used, -2 Favorites, 0..7 Unicode category, 100+ guild, 200+ sticker pack
  ccord_id guild_id;
  float target_scroll_y;   // grid scroll this rail button jumps to
  char label[32];
} ui_expr_rail_info;
typedef struct ui_expr_hit_info {
  ccord_rect rect;
  ccord_rect star_rect;    // favorite star (GIF tiles and stickers; zero otherwise)
  int kind;                // 0 Unicode emoji, 1 custom emoji, 2 GIF tile, 3 GIF category, 4 Favorites card,
                           // 5 Trending card, 6 sticker, 7 soundboard
  ccord_id id;
  ccord_id guild_id;
  char name[64];
  char sub[64];            // source section of a sticker (pack, Favorites, Frequently Used)
  char utf8[32];           // Unicode emoji payload (empty for custom/GIF/sticker items)
  char url[CCORD_URL_MAX_BYTES + 1];
  char preview_url[CCORD_URL_MAX_BYTES + 1];
} ui_expr_hit_info;
typedef struct ui_expr_inspect {
  int tab;                 // 0=GIFs, 1=Stickers, 2=Emoji, 3=Soundboard
  int gif_subview;         // 0=home, 1=favorites
  size_t nhits, nrail, rail_cap;
  size_t search_len;
  float grid_scroll_y, grid_content_h;
  float rail_scroll_y, rail_content_h;
  ccord_rect rail_rect;         // full 40px Emoji rail column
  ccord_rect rail_view;         // scrollable upper region of the Emoji rail
  ccord_rect rail_pinned_rect;  // pinned Default Unicode Emoji button
  ccord_rect grid_view;         // viewport the tab's hit cells are live in (hits_clip)
  ccord_rect search_rect;           // picker search input
  ccord_rect grid_scrollbar_rect;   // drawn grid scrollbar thumb (zero when the tab does not overflow)
  ccord_rect search_caret_rect;     // drawn caret in the search field (zero when unfocused or blinking off)
  ccord_rect search_selection_rect; // drawn selection highlight in the search field (zero when none)
  size_t hit_url_cap, hit_preview_url_cap;
} ui_expr_inspect;
bool ui_expr_get_inspect(const ccord_ui *ui, ui_expr_inspect *out);
bool ui_expr_rail_at(const ccord_ui *ui, size_t index, ui_expr_rail_info *out);
bool ui_expr_hit_at(const ccord_ui *ui, size_t index, ui_expr_hit_info *out);

// Grid geometry derived from the popover width (the one source for the Emoji, GIF and Sticker tabs).
typedef struct ui_expr_grid_layout {
  int emoji_cols;
  float emoji_cell, emoji_gap;
  int gif_cols;
  float gif_col_w, gif_gap;
  float gif_cat_w;         // half-width Favorites/Trending/category tiles on the GIF home view
  int sticker_cols;
  float sticker_cell, sticker_gap;
} ui_expr_grid_layout;
void ui_expr_grid_layout_for_width(float popover_w, ui_expr_grid_layout *out);

