// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/model/forum.h>
#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define UI_FORUM_MAX_VISIBLE_POSTS 128
#define UI_FORUM_MAX_TAG_HITS      CCORD_FORUM_MAX_TAGS

typedef struct ui_forum_post_hit {
  ccord_id thread_id;
  ccord_rect rect;
} ui_forum_post_hit;

typedef struct ui_forum_tag_hit {
  ccord_id tag_id;
  ccord_rect rect;
} ui_forum_tag_hit;

struct ui_forum_state {
  ccord_id channel_id;
  ccord_text_edit search_edit;
  ccord_text_edit title_edit;
  ccord_text_edit body_edit;
  int active_field;   // 0 = search, 1 = composer title, 2 = composer body
  bool composer_open;
  ccord_forum_sort sort;
  ccord_id filter_tags[CCORD_FORUM_MAX_APPLIED_TAGS];
  uint8_t filter_tag_count;
  ccord_id composer_tags[CCORD_FORUM_MAX_APPLIED_TAGS];
  uint8_t composer_tag_count;
  float scroll_y;
  ccord_scroller glide;   // wheel glide of the post list (scroll.h)
  float content_h;
  float viewport_h;
  ccord_rect thumb_rect;   // drawn post-list thumb (zero when it fits / is hidden)
  float mouse_x;
  float mouse_y;

  // Hit-test rectangles updated during draw
  ccord_rect area_rect;
  ccord_rect search_rect;
  ccord_rect new_post_btn_rect;
  ccord_rect sort_activity_rect;
  ccord_rect sort_creation_rect;
  ccord_rect clear_filter_rect;
  ui_forum_tag_hit filter_tag_hits[UI_FORUM_MAX_TAG_HITS];
  size_t filter_tag_hit_count;

  ccord_rect composer_card_rect;
  ccord_rect composer_title_rect;
  ccord_rect composer_body_rect;
  ccord_rect composer_submit_rect;
  ccord_rect composer_cancel_rect;
  ui_forum_tag_hit composer_tag_hits[UI_FORUM_MAX_TAG_HITS];
  size_t composer_tag_hit_count;

  ui_forum_post_hit post_hits[UI_FORUM_MAX_VISIBLE_POSTS];
  size_t post_hit_count;

  // Cached filtered & sorted post indices into store_posts
  bool cache_valid;
  uint64_t cache_store_rev;
  ccord_id cache_forum_id;
  ccord_forum_sort cache_sort;
  size_t cached_indices[UI_FORUM_MAX_VISIBLE_POSTS];
  size_t cached_count;

  // Chat header breadcrumb when viewing a forum post thread
  ccord_rect breadcrumb_rect;
  ccord_id breadcrumb_forum_id;
};

static struct ui_forum_state *forum_ensure(ccord_ui *ui)
{
  if (ui->forum) return ui->forum;
  struct ui_forum_state *f = ccord_calloc(1, sizeof(*f));
  if (!f) return nullptr;
  ccord_text_edit_init(&f->search_edit, CCORD_FORUM_TITLE_BYTES);
  ccord_text_edit_init(&f->title_edit, CCORD_FORUM_TITLE_BYTES);
  ccord_text_edit_init(&f->body_edit, 2000);
  f->sort = CCORD_FORUM_SORT_ACTIVITY;
  ui->forum = f;
  return f;
}

void ui_forum_destroy(ccord_ui *ui)
{
  if (!ui || !ui->forum) return;
  ccord_text_edit_free(&ui->forum->search_edit);
  ccord_text_edit_free(&ui->forum->title_edit);
  ccord_text_edit_free(&ui->forum->body_edit);
  ccord_free(ui->forum);
  ui->forum = nullptr;
}

// Ticks the post-list glide (scroll.h) by fdt_ms; dropped when the forum is not on screen.
bool ui_forum_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  struct ui_forum_state *f = ui->forum;
  if (!f || f->glide.pending == 0.0f) return false;
  if (!ui_forum_is_active(ui)) {
    ccord_scroller_cancel(&f->glide);
    return false;
  }
  ui->dirty = true;
  return ccord_scroller_tick(&f->glide, &f->scroll_y, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

bool ui_forum_is_active(const ccord_ui *ui)
{
  if (!ui || !ui->d.store) return false;
  if (ui->stage_channel != CCORD_ID_NONE || ui_is_dm_friends_view(ui)) return false;
  ccord_id cid = ccord_store_selected_channel(ui->d.store);
  return cid != CCORD_ID_NONE && ccord_store_channel_is_forum(ui->d.store, cid);
}

void ui_forum_set_breadcrumb_rect(ccord_ui *ui, ccord_rect r, ccord_id forum_id)
{
  if (!ui) return;
  if (forum_id == CCORD_ID_NONE && r.w == 0.0f && r.h == 0.0f) {
    if (!ui->forum) return;
    ui->forum->breadcrumb_rect = (ccord_rect){0};
    ui->forum->breadcrumb_forum_id = CCORD_ID_NONE;
    if (!ui_forum_is_active(ui)) {
      ccord_id sel = ui->d.store ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE;
      const ccord_channel *ch = (sel != CCORD_ID_NONE && ui->d.store) ? ccord_store_channel(ui->d.store, sel) : nullptr;
      bool in_forum_thread =
        ch && ch->parent_id != CCORD_ID_NONE && ccord_store_channel_is_forum(ui->d.store, ch->parent_id);
      if (!in_forum_thread) {
        ui_forum_destroy(ui);
      }
    }
    return;
  }
  struct ui_forum_state *f = forum_ensure(ui);
  if (!f) return;
  f->breadcrumb_rect = r;
  f->breadcrumb_forum_id = forum_id;
}

ccord_rect ui_forum_breadcrumb_rect(const ccord_ui *ui)
{
  if (!ui || !ui->forum) return (ccord_rect){0};
  return ui->forum->breadcrumb_rect;
}

ccord_rect ui_forum_post_card_rect(const ccord_ui *ui, size_t index, ccord_id *out_thread_id)
{
  if (!ui || !ui->forum || index >= ui->forum->post_hit_count) {
    if (out_thread_id) *out_thread_id = CCORD_ID_NONE;
    return (ccord_rect){0};
  }
  if (out_thread_id) *out_thread_id = ui->forum->post_hits[index].thread_id;
  return ui->forum->post_hits[index].rect;
}

ccord_id ui_forum_post_at(const ccord_ui *ui, float x, float y)
{
  if (!ui_forum_is_active(ui) || !ui->forum) return CCORD_ID_NONE;
  const struct ui_forum_state *f = ui->forum;
  if (!ui_in_rect(x, y, f->area_rect)) return CCORD_ID_NONE;
  if (f->composer_open && ui_in_rect(x, y, f->composer_card_rect)) return CCORD_ID_NONE;
  for (size_t i = 0; i < f->post_hit_count; i++) {
    if (ui_in_rect(x, y, f->post_hits[i].rect)) return f->post_hits[i].thread_id;
  }
  return CCORD_ID_NONE;
}

bool ui_forum_materialize_post(ccord_ui *ui, ccord_id thread_id, ccord_id *out_guild_id)
{
  if (out_guild_id) *out_guild_id = CCORD_ID_NONE;
  if (!ui || !ui->d.store || thread_id == CCORD_ID_NONE) return false;
  const ccord_channel *existing = ccord_store_channel(ui->d.store, thread_id);
  if (existing) {
    if (out_guild_id) *out_guild_id = existing->guild_id;
    return true;
  }
  const ccord_forum_post *p = ccord_store_forum_find_post(ui->d.store, thread_id);
  if (!p) return false;
  const ccord_channel *forum = ccord_store_channel(ui->d.store, p->forum_id);
  if (!forum) return false;
  ccord_id forum_gid = forum->guild_id;
  ccord_channel c = {
    .id = p->thread_id,
    .guild_id = forum_gid,
    .parent_id = forum->id,
    .kind = CCORD_CHANNEL_TEXT,
    .name = p->title,
    .topic = "",
    .last_message_id = p->last_message_id,
    .icon_url = "",
    .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
  };
  ccord_error err = {0};
  if (ccord_store_upsert_channel(ui->d.store, &c, &err) != CCORD_OK) return false;
  if (out_guild_id) *out_guild_id = forum_gid;
  return true;
}

// Wheel area, offset and travel of the post list (verify-slice seam).
void ui_forum_scroll_debug(const ccord_ui *ui, ccord_rect *area, float *scroll, float *room)
{
  const struct ui_forum_state *f = ui ? ui->forum : nullptr;
  if (area) *area = f ? f->area_rect : (ccord_rect){0, 0, 0, 0};
  if (scroll) *scroll = f ? f->scroll_y : 0.0f;
  if (room) *room = f ? fmaxf(f->content_h - f->viewport_h, 0.0f) : 0.0f;
}

size_t ui_forum_visible_post_count(const ccord_ui *ui)
{
  return (ui && ui->forum) ? ui->forum->post_hit_count : 0;
}

ccord_rect ui_forum_new_post_btn_rect(const ccord_ui *ui)
{
  return (ui && ui->forum) ? ui->forum->new_post_btn_rect : (ccord_rect){0};
}

ccord_rect ui_forum_filter_tag_rect(const ccord_ui *ui, size_t index)
{
  if (!ui || !ui->forum || index >= ui->forum->filter_tag_hit_count) return (ccord_rect){0};
  return ui->forum->filter_tag_hits[index].rect;
}

ccord_rect ui_forum_composer_submit_rect(const ccord_ui *ui)
{
  return (ui && ui->forum && ui->forum->composer_open) ? ui->forum->composer_submit_rect : (ccord_rect){0};
}

bool ui_forum_handle_header_click(ccord_ui *ui, float mx, float my)
{
  if (!ui || !ui->forum) return false;
  struct ui_forum_state *f = ui->forum;
  if (f->breadcrumb_forum_id != CCORD_ID_NONE && ui_in_rect(mx, my, f->breadcrumb_rect)) {
    ccord_error err = {0};
    if (ui->d.session) {
      (void)ccord_session_select_channel(ui->d.session, f->breadcrumb_forum_id, &err);
    } else if (ui->d.store) {
      (void)ccord_store_select_channel(ui->d.store, f->breadcrumb_forum_id, &err);
    }
    ui_mark_dirty(ui);
    return true;
  }
  return false;
}

static bool tag_list_has(const ccord_id *ids, uint8_t count, ccord_id id)
{
  for (uint8_t i = 0; i < count; i++) {
    if (ids[i] == id) return true;
  }
  return false;
}

static void tag_list_toggle(ccord_id *ids, uint8_t *count, uint8_t cap, ccord_id id)
{
  for (uint8_t i = 0; i < *count; i++) {
    if (ids[i] == id) {
      for (uint8_t j = i; (uint8_t)(j + 1u) < *count; j++) ids[j] = ids[j + 1u];
      (*count)--;
      return;
    }
  }
  if (*count < cap) {
    ids[(*count)++] = id;
  }
}

static bool str_contains_ci(const char *hay, const char *needle)
{
  if (!needle || !needle[0]) return true;
  if (!hay || !hay[0]) return false;
  size_t nlen = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
      i++;
    }
    if (i == nlen) return true;
  }
  return false;
}

static void format_relative_time(int64_t ts_ms, int64_t now_ms, char *out, size_t cap)
{
  if (ts_ms <= 0 || now_ms <= 0 || ts_ms > now_ms) {
    snprintf(out, cap, "just now");
    return;
  }
  int64_t diff_sec = (now_ms - ts_ms) / 1000;
  if (diff_sec < 60) {
    snprintf(out, cap, "just now");
  } else if (diff_sec < 3600) {
    snprintf(out, cap, "%lldm ago", (long long)(diff_sec / 60));
  } else if (diff_sec < 86400) {
    snprintf(out, cap, "%lldh ago", (long long)(diff_sec / 3600));
  } else {
    snprintf(out, cap, "%lldd ago", (long long)(diff_sec / 86400));
  }
}

static const ccord_forum_tag *find_forum_tag(const ccord_forum_meta *meta, ccord_id tag_id)
{
  if (!meta || tag_id == CCORD_ID_NONE) return nullptr;
  for (uint8_t i = 0; i < meta->tag_count; i++) {
    if (meta->tags[i].id == tag_id) return &meta->tags[i];
  }
  return nullptr;
}

static bool post_matches_filter(const ccord_ui *ui, const struct ui_forum_state *f, const ccord_forum_post *post)
{
  if (!post) return false;
  for (uint8_t i = 0; i < f->filter_tag_count; i++) {
    if (!tag_list_has(post->applied_tags, post->applied_tag_count, f->filter_tags[i])) return false;
  }
  const char *q = ccord_text_edit_cstr(&f->search_edit);
  if (q && q[0] != '\0') {
    if (str_contains_ci(post->title, q) || str_contains_ci(post->first_content, q)) return true;
    ccord_id aid = post->first_author_id != CCORD_ID_NONE ? post->first_author_id : post->owner_id;
    const ccord_user *u = ccord_store_user(ui->d.store, aid);
    if (u && (str_contains_ci(u->display_name, q) || str_contains_ci(u->name, q))) return true;
    return false;
  }
  return true;
}

static float draw_tag_pill(ccord_ui *ui, ccord_renderer *r, float x, float y, float h,
                           const ccord_forum_tag *tag, bool selected, bool hovered)
{
  char label[96];
  if (tag->emoji_name[0] != '\0') {
    snprintf(label, sizeof(label), "%s %s", tag->emoji_name, tag->name);
  } else {
    snprintf(label, sizeof(label), "%s", tag->name);
  }
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_header));
  float w = ccord_text_measure(r, label, strlen(label), &st, 0.0f).x + 2.0f * UIS_SP_2;
  uis_chip(ui, r, (ccord_rect){x, y, w, h}, label, (uis_state){.hover = hovered, .selected = selected}, "ftag",
           ui_name_id(tag->name));
  return w;
}

typedef struct forum_sort_entry {
  size_t idx;
  int64_t key;
  bool pinned;
} forum_sort_entry;

static int forum_sort_cmp(const void *pa, const void *pb)
{
  const forum_sort_entry *a = pa;
  const forum_sort_entry *b = pb;
  if (a->pinned != b->pinned) return a->pinned ? -1 : 1;
  if (a->key != b->key) return a->key > b->key ? -1 : 1;
  return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

void ui_draw_forum(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui_forum_is_active(ui)) return;
  struct ui_forum_state *f = forum_ensure(ui);
  if (!f) return;

  ccord_id forum_id = ccord_store_selected_channel(ui->d.store);
  if (f->channel_id != forum_id) {
    f->channel_id = forum_id;
    f->scroll_y = 0.0f;
    ccord_scroller_cancel(&f->glide);
    f->composer_open = false;
    f->filter_tag_count = 0;
    f->composer_tag_count = 0;
    f->active_field = 0;
    ccord_text_edit_clear(&f->search_edit);
    ccord_text_edit_clear(&f->title_edit);
    ccord_text_edit_clear(&f->body_edit);
    const ccord_forum_meta *meta = ccord_store_forum_meta(ui->d.store, forum_id);
    if (meta) f->sort = (ccord_forum_sort)meta->default_sort;
    f->cache_valid = false;
  }

  // Forum view occupies both message_area and composer area
  ccord_rect msg_r = ui_rect(ui->layout.message_area);
  ccord_rect comp_r = ui_rect(ui->layout.composer);
  ccord_rect area = {msg_r.x, msg_r.y, msg_r.w, msg_r.h + comp_r.h};
  f->area_rect = area;
  f->filter_tag_hit_count = 0;
  f->composer_tag_hit_count = 0;
  f->post_hit_count = 0;
  f->clear_filter_rect = (ccord_rect){0};
  f->composer_card_rect = (ccord_rect){0};
  f->composer_title_rect = (ccord_rect){0};
  f->composer_body_rect = (ccord_rect){0};
  f->composer_submit_rect = (ccord_rect){0};
  f->composer_cancel_rect = (ccord_rect){0};

  ccord_draw_rect(r, area, COL(bg_primary));
  ccord_push_clip(r, area);

  const ccord_forum_meta *meta = ccord_store_forum_meta(ui->d.store, forum_id);
  const ccord_forum_state *fstate = ccord_store_forum_state(ui->d.store, forum_id);

  float pad_x = 16.0f;
  float cur_y = area.y + 12.0f;
  float avail_w = area.w - pad_x * 2.0f;
  if (avail_w < 120.0f) {
    ccord_pop_clip(r);
    return;
  }

  // ---- Row 1: Search bar + New Post button ----
  float btn_w = 108.0f;
  float row1_h = UIS_H_INPUT;
  ccord_rect search_r = {area.x + pad_x, cur_y, avail_w - btn_w - 10.0f, row1_h};
  ccord_rect new_btn_r = {search_r.x + search_r.w + 10.0f, cur_y, btn_w, row1_h};
  f->search_rect = search_r;
  f->new_post_btn_rect = new_btn_r;

  bool search_focus = (!f->composer_open && f->active_field == 0);
  uis_input_padded(ui, r, search_r, 32.0f, UIS_SP_3, &f->search_edit, "Search or create a post...",
                   (uis_state){.focused = search_focus}, false, "fsearch", 0);
  ui_icon_search(r, search_r.x + 16.0f, search_r.y + row1_h * 0.5f, 14.0f, COL(text_muted));

  bool new_hover = ui_in_rect(f->mouse_x, f->mouse_y, new_btn_r);
  new_btn_r.y += (row1_h - UIS_H_BTN) * 0.5f;
  new_btn_r.h = UIS_H_BTN;
  f->new_post_btn_rect = new_btn_r;
  uis_button(ui, r, new_btn_r, f->composer_open ? "Close" : "+ New Post",
             f->composer_open ? UIS_BTN_SECONDARY : UIS_BTN_PRIMARY, UIS_BTN_S_MD, (uis_state){.hover = new_hover}, "fnew", 0);

  cur_y += row1_h + 10.0f;

  // ---- Row 2: Sort chips + Filter tag pills ----
  float row2_h = 24.0f;
  float rx = area.x + pad_x;
  float fsz_sm = M(font_size_small);
  float lh_sm = ui_line_h(ui, fsz_sm);

  ccord_text_style chip_st = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_header));
  const char *s1 = "Recent Activity";
  ccord_rect s1_r = {rx, cur_y, ccord_text_measure(r, s1, strlen(s1), &chip_st, 0.0f).x + 2.0f * UIS_SP_2, row2_h};
  f->sort_activity_rect = s1_r;
  uis_chip(ui, r, s1_r, s1,
           (uis_state){.hover = ui_in_rect(f->mouse_x, f->mouse_y, s1_r), .selected = f->sort == CCORD_FORUM_SORT_ACTIVITY},
           "fsort", 0);
  rx += s1_r.w + 6.0f;

  const char *s2 = "Creation Date";
  ccord_rect s2_r = {rx, cur_y, ccord_text_measure(r, s2, strlen(s2), &chip_st, 0.0f).x + 2.0f * UIS_SP_2, row2_h};
  f->sort_creation_rect = s2_r;
  uis_chip(ui, r, s2_r, s2,
           (uis_state){.hover = ui_in_rect(f->mouse_x, f->mouse_y, s2_r), .selected = f->sort == CCORD_FORUM_SORT_CREATION},
           "fsort", 1);
  rx += s2_r.w + 10.0f;

  // Vertical divider between sort chips and filter tags
  ccord_draw_rect(r, (ccord_rect){rx, cur_y + 4.0f, 1.0f, row2_h - 8.0f}, COL(divider));
  rx += 10.0f;

  if (meta) {
    for (uint8_t i = 0; i < meta->tag_count && f->filter_tag_hit_count < UI_FORUM_MAX_TAG_HITS; i++) {
      const ccord_forum_tag *tag = &meta->tags[i];
      bool sel = tag_list_has(f->filter_tags, f->filter_tag_count, tag->id);
      float pw = draw_tag_pill(ui, r, rx, cur_y, row2_h, tag, sel, false);
      ccord_rect pr = {rx, cur_y, pw, row2_h};
      if (ui_in_rect(f->mouse_x, f->mouse_y, pr) && !sel) {
        (void)draw_tag_pill(ui, r, rx, cur_y, row2_h, tag, sel, true);
      }
      f->filter_tag_hits[f->filter_tag_hit_count++] = (ui_forum_tag_hit){.tag_id = tag->id, .rect = pr};
      rx += pw + 6.0f;
      if (rx > area.x + area.w - 80.0f) break;
    }
  }

  if (f->filter_tag_count > 0 || f->search_edit.len > 0) {
    const char *clr = "Clear";
    ccord_text_style clr_st = ui_style(ui, CCORD_FONT_BOLD, fsz_sm, COL(brand));
    ccord_vec2 cm = ccord_text_measure(r, clr, strlen(clr), &clr_st, 0.0f);
    ccord_rect cr = {rx + 2.0f, cur_y, cm.x + 12.0f, row2_h};
    f->clear_filter_rect = cr;
    ccord_text_draw(r, clr, strlen(clr), cr.x + 6.0f, cr.y + (row2_h - lh_sm) * 0.5f, &clr_st, 0.0f);
  }

  cur_y += row2_h + 10.0f;
  ccord_draw_rect(r, (ccord_rect){area.x + pad_x, cur_y, avail_w, 1.0f}, COL(divider));
  cur_y += 10.0f;

  // ---- Expandable New Post Composer Card ----
  if (f->composer_open) {
    float comp_h = 160.0f;
    ccord_rect card = {area.x + pad_x, cur_y, avail_w, comp_h};
    f->composer_card_rect = card;
    ccord_draw_rrect(r, card, 8.0f, COL(border_subtle));
    ccord_draw_rrect(r, (ccord_rect){card.x + 1.0f, card.y + 1.0f, card.w - 2.0f, card.h - 2.0f},
                     7.0f, COL(bg_secondary));

    float cx = card.x + 12.0f;
    float cy = card.y + 10.0f;
    float cw = card.w - 24.0f;

    const char *hdr = "Create a New Post";
    ccord_text_style hdr_st = ui_style(ui, CCORD_FONT_BOLD, fsz_sm, COL(text_header));
    ccord_text_draw(r, hdr, strlen(hdr), cx, cy, &hdr_st, 0.0f);
    cy += lh_sm + 6.0f;

    // Title input
    ccord_rect title_r = {cx, cy, cw, UIS_H_INPUT};
    f->composer_title_rect = title_r;
    bool t_focus = (f->active_field == 1);
    uis_input(ui, r, title_r, &f->title_edit, "Post title...", (uis_state){.focused = t_focus}, false, "ftitle", 0);
    cy += title_r.h + 8.0f;

    // Body input
    ccord_rect body_r = {cx, cy, cw, UIS_H_INPUT};
    f->composer_body_rect = body_r;
    bool b_focus = (f->active_field == 2);
    uis_input(ui, r, body_r, &f->body_edit, "Enter a message to start the discussion...", (uis_state){.focused = b_focus},
              false, "fbody", 0);
    cy += body_r.h + 8.0f;

    // Tags selector + Cancel / Post buttons
    float tx = cx;
    const char *tlbl = "Tags:";
    ccord_text_style tlbl_st = ui_style(ui, CCORD_FONT_BOLD, fsz_sm, COL(text_muted));
    ccord_text_draw(r, tlbl, strlen(tlbl), tx, cy + (26.0f - lh_sm) * 0.5f, &tlbl_st, 0.0f);
    tx += 38.0f;

    if (meta) {
      for (uint8_t i = 0; i < meta->tag_count && f->composer_tag_hit_count < UI_FORUM_MAX_TAG_HITS; i++) {
        const ccord_forum_tag *tag = &meta->tags[i];
        bool sel = tag_list_has(f->composer_tags, f->composer_tag_count, tag->id);
        float pw = draw_tag_pill(ui, r, tx, cy + 1.0f, 24.0f, tag, sel, false);
        ccord_rect pr = {tx, cy + 1.0f, pw, 24.0f};
        f->composer_tag_hits[f->composer_tag_hit_count++] = (ui_forum_tag_hit){.tag_id = tag->id, .rect = pr};
        tx += pw + 6.0f;
        if (tx > cx + cw - 170.0f) break;
      }
    }

    ccord_rect post_btn = {cx + cw - 76.0f, cy, 76.0f, UIS_H_BTN_SM};
    ccord_rect cancel_btn = {post_btn.x - 76.0f - 8.0f, cy, 76.0f, UIS_H_BTN_SM};
    f->composer_submit_rect = post_btn;
    f->composer_cancel_rect = cancel_btn;

    uis_button(ui, r, cancel_btn, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(f->mouse_x, f->mouse_y, cancel_btn)}, "fcancel", 0);
    bool can_post = (f->title_edit.len > 0 && f->body_edit.len > 0);
    uis_button(ui, r, post_btn, "Post", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
               (uis_state){.hover = ui_in_rect(f->mouse_x, f->mouse_y, post_btn), .disabled = !can_post}, "fpost", 0);

    cur_y += comp_h + 12.0f;
  }

  // ---- Error banner (if any) ----
  if (fstate && fstate->error[0] != '\0') {
    ccord_rect eb = {area.x + pad_x, cur_y, avail_w, 26.0f};
    ccord_draw_rrect(r, eb, 5.0f, ui_alpha(COL(danger), 0.22f));
    ccord_text_style err_st = ui_style(ui, CCORD_FONT_BOLD, fsz_sm, COL(danger));
    ccord_text_draw(r, fstate->error, strlen(fstate->error),
                    eb.x + 10.0f, eb.y + (eb.h - lh_sm) * 0.5f, &err_st, 0.0f);
    cur_y += 32.0f;
  }

  // ---- Scrollable Post Cards list ----
  ccord_rect list_rect = {area.x, cur_y, area.w, (area.y + area.h) - cur_y};
  if (list_rect.h <= 10.0f) {
    ccord_pop_clip(r);
    return;
  }
  f->viewport_h = list_rect.h;

  // Gather and sort matching posts (cached across frames until filter, sort, channel, or store revision changes)
  size_t total_store_posts = 0;
  const ccord_forum_post *store_posts = ccord_store_forum_posts(ui->d.store, forum_id, &total_store_posts);
  uint64_t store_rev = ccord_store_revision(ui->d.store);
  if (!f->cache_valid || f->cache_store_rev != store_rev || f->cache_forum_id != forum_id || f->cache_sort != f->sort) {
    forum_sort_entry entries[UI_FORUM_MAX_VISIBLE_POSTS];
    size_t nent = 0;
    for (size_t i = 0; i < total_store_posts && nent < UI_FORUM_MAX_VISIBLE_POSTS; i++) {
      const ccord_forum_post *p = &store_posts[i];
      if (post_matches_filter(ui, f, p)) {
        entries[nent++] = (forum_sort_entry){
          .idx = i,
          .key = (f->sort == CCORD_FORUM_SORT_CREATION) ? p->create_ts_ms : p->last_activity_ms,
          .pinned = p->pinned,
        };
      }
    }
    // Pinned first, then by chosen sort key descending (use qsort for >32 items, insertion sort for small lists)
    if (nent > 32u) {
      qsort(entries, nent, sizeof entries[0], forum_sort_cmp);
    } else {
      for (size_t i = 1; i < nent; i++) {
        forum_sort_entry cur = entries[i];
        size_t j = i;
        while (j > 0 && forum_sort_cmp(&cur, &entries[j - 1]) < 0) {
          entries[j] = entries[j - 1];
          j--;
        }
        entries[j] = cur;
      }
    }
    for (size_t i = 0; i < nent; i++) f->cached_indices[i] = entries[i].idx;
    f->cached_count = nent;
    f->cache_store_rev = store_rev;
    f->cache_forum_id = forum_id;
    f->cache_sort = f->sort;
    f->cache_valid = true;
  }
  size_t nposts = f->cached_count;

  float card_h = 96.0f;
  float card_gap = 10.0f;
  f->content_h = (float)nposts * (card_h + card_gap) + 16.0f;
  float max_scroll = f->content_h > list_rect.h ? (f->content_h - list_rect.h) : 0.0f;
  if (f->scroll_y > max_scroll) f->scroll_y = max_scroll;
  if (f->scroll_y < 0.0f) f->scroll_y = 0.0f;

  // Trigger pagination when scrolled near bottom
  if (fstate && fstate->has_more && !fstate->loading && max_scroll > 0.0f &&
      f->scroll_y >= max_scroll - card_h && ui->d.session) {
    ccord_forum_query q = {
      .forum_id = forum_id,
      .sort = f->sort,
      .offset = fstate->next_offset,
    };
    (void)ccord_session_request_forum_posts(ui->d.session, &q, nullptr);
  }

  ccord_push_clip(r, list_rect);

  int64_t now_ms = ui->d.fixed_now_unix_ms != 0 ? ui->d.fixed_now_unix_ms
                 : (ui->now_unix_ms != 0 ? ui->now_unix_ms : (int64_t)time(nullptr) * 1000);
  float fsz_base = M(font_size_base);
  float fsz_title = fsz_base + 1.5f;
  float lh_title = ui_line_h(ui, fsz_title);
  float lh_base = ui_line_h(ui, fsz_base);

  if (nposts == 0) {
    const char *empty_msg = (fstate && fstate->loading) ? "Loading forum posts..."
                                                         : "No posts match the current filter.";
    ccord_text_style est = ui_style(ui, CCORD_FONT_REGULAR, fsz_base, COL(text_muted));
    ccord_vec2 em = ccord_text_measure(r, empty_msg, strlen(empty_msg), &est, 0.0f);
    ccord_text_draw(r, empty_msg, strlen(empty_msg),
                    list_rect.x + (list_rect.w - em.x) * 0.5f,
                    list_rect.y + 44.0f,
                    &est, 0.0f);
  }

  for (size_t i = 0; i < nposts; i++) {
    size_t idx = f->cached_indices[i];
    if (idx >= total_store_posts) continue;
    const ccord_forum_post *post = &store_posts[idx];
    float cy = list_rect.y + (float)i * (card_h + card_gap) - f->scroll_y;
    ccord_rect card = {area.x + pad_x, cy, avail_w, card_h};
    if (card.y + card.h < list_rect.y || card.y > list_rect.y + list_rect.h) continue;

    // Only fully visible cards are clickable; clipped ones must be scrolled into view first.
    bool fully_visible = card.y >= list_rect.y && card.y + card.h <= list_rect.y + list_rect.h;
    if (fully_visible && f->post_hit_count < UI_FORUM_MAX_VISIBLE_POSTS) {
      f->post_hits[f->post_hit_count++] = (ui_forum_post_hit){.thread_id = post->thread_id, .rect = card};
    }

    bool hovered = ui_in_rect(f->mouse_x, f->mouse_y, card);
    float card_hv = ui_fade(ui, "fcard", post->thread_id, hovered);
    ccord_color border_col = post->pinned ? ui_alpha(COL(brand), 0.70f)
                                          : ui_mix(COL(border_subtle), COL(text_muted), card_hv);
    ccord_color card_bg = ui_mix(COL(bg_secondary), ui_mix(COL(bg_secondary), COL(bg_item_hover), 0.55f), card_hv);

    ccord_draw_rrect(r, card, 8.0f, border_col);
    ccord_draw_rrect(r, (ccord_rect){card.x + 1.0f, card.y + 1.0f, card.w - 2.0f, card.h - 2.0f}, 7.0f, card_bg);

    {
      float bar = post->pinned ? 1.0f : card_hv;
      if (bar > 0.004f) {
        ccord_draw_rrect(r, (ccord_rect){card.x + 1.0f, card.y + 8.0f, 3.0f, card.h - 16.0f}, 1.5f, ui_alpha(COL(brand), bar));
      }
    }

    float inner_x = card.x + 14.0f;
    float inner_w = card.w - 28.0f;
    float row_y = card.y + 10.0f;

    // ---- Card Line 1: Pinned badge + Post Title + Applied Tag pills ----
    float tx = inner_x;
    if (post->pinned) {
      const char *pin_lbl = "PINNED";
      ccord_text_style pst = ui_style(ui, CCORD_FONT_BOLD, fsz_sm - 1.0f, COL(brand));
      ccord_vec2 pm = ccord_text_measure(r, pin_lbl, strlen(pin_lbl), &pst, 0.0f);
      ccord_rect pin_pill = {tx, row_y + 1.0f, pm.x + 24.0f, 20.0f};
      ccord_draw_rrect(r, pin_pill, 10.0f, ui_alpha(COL(brand), 0.20f));
      ui_icon_pin(r, pin_pill.x + 9.0f, pin_pill.y + 10.0f, 10.0f, COL(brand));
      ccord_text_draw(r, pin_lbl, strlen(pin_lbl),
                      pin_pill.x + 17.0f, pin_pill.y + (pin_pill.h - lh_sm) * 0.5f, &pst, 0.0f);
      tx += pin_pill.w + 8.0f;
    }

    // Draw applied tags from right edge first so we know how much width remains for the title
    float right_x = inner_x + inner_w;
    if (meta && post->applied_tag_count > 0) {
      for (int ti = (int)post->applied_tag_count - 1; ti >= 0; ti--) {
        const ccord_forum_tag *tag = find_forum_tag(meta, post->applied_tags[ti]);
        if (!tag) continue;
        char tlabel[96];
        if (tag->emoji_name[0] != '\0') {
          snprintf(tlabel, sizeof(tlabel), "%s %s", tag->emoji_name, tag->name);
        } else {
          snprintf(tlabel, sizeof(tlabel), "%s", tag->name);
        }
        ccord_text_style tst = ui_style(ui, CCORD_FONT_REGULAR, fsz_sm, COL(text_normal));
        ccord_vec2 tm = ccord_text_measure(r, tlabel, strlen(tlabel), &tst, 0.0f);
        float pw = tm.x + 14.0f;
        if (right_x - pw < tx + 140.0f) break;
        right_x -= pw;
        ccord_rect tpill = {right_x, row_y + 1.0f, pw, 20.0f};
        ccord_draw_rrect(r, tpill, 10.0f, COL(bg_tertiary));
        ccord_text_draw(r, tlabel, strlen(tlabel),
                        tpill.x + 7.0f, tpill.y + (tpill.h - lh_sm) * 0.5f, &tst, 0.0f);
        right_x -= 6.0f;
      }
    }

    float title_max_w = right_x - tx - 6.0f;
    const char *ptitle = post->title ? post->title : "";
    if (title_max_w > 20.0f && ptitle[0] != '\0') {
      ccord_text_style title_st = ui_style(ui, CCORD_FONT_BOLD, fsz_title, COL(text_header));
      (void)ccord_text_draw_ellipsis(r, ptitle, strlen(ptitle), tx, row_y, title_max_w, &title_st);
    }

    row_y += lh_title + 6.0f;

    // ---- Card Line 2: Author Avatar + Author Name + Starter Message Preview Excerpt ----
    ccord_id author_id = post->first_author_id != CCORD_ID_NONE ? post->first_author_id : post->owner_id;
    const ccord_user *author = ccord_store_user(ui->d.store, author_id);
    float ax = inner_x;
    ui_draw_avatar(ui, r, author, ax + 9.0f, row_y + lh_base * 0.5f, 18.0f, card_bg, false);
    ax += 24.0f;

    const char *aname = author ? (author->display_name && author->display_name[0] ? author->display_name : author->name) : "Member";
    if (!aname || !aname[0]) aname = "Member";
    bool has_role = false;
    ccord_color acol = ui_role_color(ui, ccord_store_selected_guild(ui->d.store), author_id, &has_role);
    if (!has_role) acol = COL(text_normal);
    ccord_text_style ast = ui_style(ui, CCORD_FONT_BOLD, fsz_sm, acol);
    ccord_vec2 am = ccord_text_measure(r, aname, strlen(aname), &ast, 0.0f);
    ccord_text_draw(r, aname, strlen(aname), ax, row_y + (lh_base - lh_sm) * 0.5f, &ast, 0.0f);
    ax += am.x + 6.0f;

    ccord_text_style dot_st = ui_style(ui, CCORD_FONT_REGULAR, fsz_sm, COL(text_muted));
    ccord_text_draw(r, "•", strlen("•"), ax, row_y + (lh_base - lh_sm) * 0.5f, &dot_st, 0.0f);
    ax += 12.0f;

    char clean_preview[CCORD_FORUM_EXCERPT_BYTES];
    const char *raw_excerpt = post->first_content ? post->first_content : "";
    ccord_forum_strip_markdown(raw_excerpt, strlen(raw_excerpt), clean_preview, sizeof(clean_preview));
    float prev_w = (inner_x + inner_w) - ax;
    if (prev_w > 20.0f && clean_preview[0] != '\0') {
      ccord_text_style prev_st = ui_style(ui, CCORD_FONT_REGULAR, fsz_base, COL(text_muted));
      (void)ccord_text_draw_ellipsis(r, clean_preview, strlen(clean_preview), ax, row_y, prev_w, &prev_st);
    }

    row_y += lh_base + 7.0f;

    // ---- Card Line 3: Reaction badge + Message count pill + Relative activity timestamp ----
    float fx = inner_x;
    float pill_h = 20.0f;
    if (post->reaction_count > 0) {
      char rbuf[64];
      const char *remoji = (post->reaction_emoji && post->reaction_emoji[0] != '\0') ? post->reaction_emoji : "👍";
      snprintf(rbuf, sizeof(rbuf), "%s %u", remoji, (unsigned)post->reaction_count);
      ccord_text_style rst = ui_style(ui, CCORD_FONT_BOLD, fsz_sm, COL(text_normal));
      ccord_vec2 rm = ccord_text_measure(r, rbuf, strlen(rbuf), &rst, 0.0f);
      ccord_rect rpill = {fx, row_y, rm.x + 14.0f, pill_h};
      ccord_draw_rrect(r, rpill, pill_h * 0.5f, COL(bg_tertiary));
      ccord_text_draw(r, rbuf, strlen(rbuf),
                      rpill.x + 7.0f, rpill.y + (pill_h - lh_sm) * 0.5f, &rst, 0.0f);
      fx += rpill.w + 8.0f;
    }

    char mbuf[48];
    snprintf(mbuf, sizeof(mbuf), "%u", (unsigned)post->message_count);
    ccord_text_style mst = ui_style(ui, CCORD_FONT_BOLD, fsz_sm, COL(text_normal));
    ccord_vec2 mm = ccord_text_measure(r, mbuf, strlen(mbuf), &mst, 0.0f);
    ccord_rect mpill = {fx, row_y, mm.x + 28.0f, pill_h};
    ccord_draw_rrect(r, mpill, pill_h * 0.5f, COL(bg_tertiary));
    ui_icon_forum(r, mpill.x + 11.0f, mpill.y + pill_h * 0.5f, 11.0f, COL(text_muted));
    ccord_text_draw(r, mbuf, strlen(mbuf),
                    mpill.x + 20.0f, mpill.y + (pill_h - lh_sm) * 0.5f, &mst, 0.0f);
    fx += mpill.w + 10.0f;

    char rel_buf[48];
    char act_lbl[80];
    format_relative_time(post->last_activity_ms, now_ms, rel_buf, sizeof(rel_buf));
    snprintf(act_lbl, sizeof(act_lbl), "Active %s", rel_buf);
    ccord_text_style act_st = ui_style(ui, CCORD_FONT_REGULAR, fsz_sm, COL(text_muted));
    ccord_text_draw(r, act_lbl, strlen(act_lbl),
                    fx, row_y + (pill_h - lh_sm) * 0.5f, &act_st, 0.0f);
  }

  f->thumb_rect = ui_draw_list_scrollbar(ui, r, list_rect, f->content_h, f->scroll_y, "forumscroll", 0);
  ccord_pop_clip(r);
  ccord_pop_clip(r);
}

static ccord_text_edit *active_forum_edit(struct ui_forum_state *f)
{
  if (!f->composer_open) return &f->search_edit;
  if (f->active_field == 1) return &f->title_edit;
  if (f->active_field == 2) return &f->body_edit;
  return &f->search_edit;
}

static void submit_new_forum_post(ccord_ui *ui, struct ui_forum_state *f)
{
  if (f->title_edit.len == 0 || f->body_edit.len == 0) return;
  ccord_id forum_id = ccord_store_selected_channel(ui->d.store);
  if (forum_id == CCORD_ID_NONE) return;

  const char *title = ccord_text_edit_cstr(&f->title_edit);
  const char *body = ccord_text_edit_cstr(&f->body_edit);
  ccord_error err = {0};

  if (ui->d.session) {
    if (ccord_session_create_forum_post(ui->d.session, forum_id, title, strlen(title),
                                        body, strlen(body), f->composer_tags, f->composer_tag_count, &err) == CCORD_OK) {
      f->composer_open = false;
      f->active_field = 0;
      f->composer_tag_count = 0;
      ccord_text_edit_clear(&f->title_edit);
      ccord_text_edit_clear(&f->body_edit);
    } else {
      ui_notice(ui, "%s", err.message[0] ? err.message : "Failed to create forum post");
    }
  } else if (ui->d.store) {
    // Pure store fallback for unit tests without a session
    size_t existing = 0;
    (void)ccord_store_forum_posts(ui->d.store, forum_id, &existing);
    ccord_id tid = 900000ULL + (ccord_id)existing + 1ULL;
    ccord_id self_id = ccord_store_self_id(ui->d.store);
    ccord_forum_post post = {
      .thread_id = tid,
      .forum_id = forum_id,
      .guild_id = ccord_store_selected_guild(ui->d.store),
      .owner_id = self_id != CCORD_ID_NONE ? self_id : 1ULL,
      .title = title,
      .applied_tag_count = f->composer_tag_count,
      .message_count = 0,
      .create_ts_ms = ui->d.fixed_now_unix_ms ? ui->d.fixed_now_unix_ms : 1700000000000LL,
      .last_activity_ms = ui->d.fixed_now_unix_ms ? ui->d.fixed_now_unix_ms : 1700000000000LL,
      .first_message_id = tid,
      .first_author_id = self_id != CCORD_ID_NONE ? self_id : 1ULL,
      .first_content = body,
    };
    for (uint8_t i = 0; i < f->composer_tag_count; i++) post.applied_tags[i] = f->composer_tags[i];
    (void)ccord_store_forum_upsert_posts(ui->d.store, &post, 1, &err);
    f->composer_open = false;
    f->active_field = 0;
    f->composer_tag_count = 0;
    ccord_text_edit_clear(&f->title_edit);
    ccord_text_edit_clear(&f->body_edit);
  }
  ui_mark_dirty(ui);
}

bool ui_forum_handle_event(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_forum_is_active(ui)) return false;
  struct ui_forum_state *f = forum_ensure(ui);
  if (!f) return false;

  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE:
      f->mouse_x = ev->x;
      f->mouse_y = ev->y;
      if (ui_in_rect(ev->x, ev->y, f->area_rect)) {
        ui_mark_dirty(ui);
      }
      return false;

    case CCORD_IN_WHEEL:
      if (ui_in_rect(ev->x, ev->y, f->area_rect)) {
        ccord_scroller_scroll(&f->glide, &f->scroll_y, -ev->wheel * 42.0f, fmaxf(f->content_h - f->viewport_h, 0.0f),
                              ui_wheel_direct(ui, ev));
        ui_mark_dirty(ui);
        return true;
      }
      return false;

    case CCORD_IN_MOUSE_DOWN: {
      if (ev->button == 2) {
        ccord_id tid = ui_forum_post_at(ui, ev->x, ev->y);
        ccord_id fgid = CCORD_ID_NONE;
        if (tid != CCORD_ID_NONE && ui_forum_materialize_post(ui, tid, &fgid) && fgid != CCORD_ID_NONE) {
          if (ui->d.bus) {
            (void)ccord_bus_post(ui->d.bus, (ccord_event){.type = CCORD_EVT_NAV_AUX,
                                                          .guild_id = fgid,
                                                          .channel_id = tid,
                                                          .value = CCORD_NAV_VIEW_CHANNEL});
          }
          return true;
        }
        return false;
      }
      if (ev->button != 1) return false;
      float mx = ev->x, my = ev->y;
      if (!ui_in_rect(mx, my, f->area_rect)) return false;

      if (ui_in_rect(mx, my, f->new_post_btn_rect)) {
        f->composer_open = !f->composer_open;
        if (f->composer_open) {
          f->active_field = 1;
          if (f->title_edit.len == 0 && f->search_edit.len > 0) {
            ccord_error err = {0};
            (void)ccord_text_edit_insert(&f->title_edit, f->search_edit.data, f->search_edit.len, &err);
            ccord_text_edit_clear(&f->search_edit);
            f->cache_valid = false;
          }
        } else {
          f->active_field = 0;
        }
        ui_mark_dirty(ui);
        return true;
      }

      if (ui_in_rect(mx, my, f->search_rect)) {
        f->active_field = 0;
        ui_mark_dirty(ui);
        return true;
      }

      if (ui_in_rect(mx, my, f->sort_activity_rect)) {
        f->sort = CCORD_FORUM_SORT_ACTIVITY;
        f->cache_valid = false;
        ui_mark_dirty(ui);
        return true;
      }
      if (ui_in_rect(mx, my, f->sort_creation_rect)) {
        f->sort = CCORD_FORUM_SORT_CREATION;
        f->cache_valid = false;
        ui_mark_dirty(ui);
        return true;
      }
      if (f->clear_filter_rect.w > 0.0f && ui_in_rect(mx, my, f->clear_filter_rect)) {
        f->filter_tag_count = 0;
        ccord_text_edit_clear(&f->search_edit);
        f->cache_valid = false;
        ui_mark_dirty(ui);
        return true;
      }

      for (size_t i = 0; i < f->filter_tag_hit_count; i++) {
        if (ui_in_rect(mx, my, f->filter_tag_hits[i].rect)) {
          tag_list_toggle(f->filter_tags, &f->filter_tag_count, CCORD_FORUM_MAX_APPLIED_TAGS,
                          f->filter_tag_hits[i].tag_id);
          f->scroll_y = 0.0f;
          ccord_scroller_cancel(&f->glide);
          f->cache_valid = false;
          ui_mark_dirty(ui);
          return true;
        }
      }

      if (f->composer_open && ui_in_rect(mx, my, f->composer_card_rect)) {
        if (ui_in_rect(mx, my, f->composer_title_rect)) {
          f->active_field = 1;
          ui_mark_dirty(ui);
          return true;
        }
        if (ui_in_rect(mx, my, f->composer_body_rect)) {
          f->active_field = 2;
          ui_mark_dirty(ui);
          return true;
        }
        if (ui_in_rect(mx, my, f->composer_cancel_rect)) {
          f->composer_open = false;
          f->active_field = 0;
          ui_mark_dirty(ui);
          return true;
        }
        if (ui_in_rect(mx, my, f->composer_submit_rect)) {
          submit_new_forum_post(ui, f);
          return true;
        }
        for (size_t i = 0; i < f->composer_tag_hit_count; i++) {
          if (ui_in_rect(mx, my, f->composer_tag_hits[i].rect)) {
            tag_list_toggle(f->composer_tags, &f->composer_tag_count, CCORD_FORUM_MAX_APPLIED_TAGS,
                            f->composer_tag_hits[i].tag_id);
            ui_mark_dirty(ui);
            return true;
          }
        }
        return true;
      }

      for (size_t i = 0; i < f->post_hit_count; i++) {
        if (ui_in_rect(mx, my, f->post_hits[i].rect)) {
          ccord_id tid = f->post_hits[i].thread_id;
          if ((ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0) {
            ccord_id fgid = CCORD_ID_NONE;
            if (ui_forum_materialize_post(ui, tid, &fgid) && fgid != CCORD_ID_NONE) {
              if (ui->d.bus) {
                (void)ccord_bus_post(ui->d.bus, (ccord_event){.type = CCORD_EVT_NAV_AUX,
                                                              .guild_id = fgid,
                                                              .channel_id = tid,
                                                              .value = CCORD_NAV_VIEW_CHANNEL});
              }
              return true;
            }
            return false;
          }
          ccord_error err = {0};
          if (ui->d.session) {
            (void)ccord_session_open_forum_post(ui->d.session, tid, &err);
          } else if (ui->d.store) {
            (void)ccord_store_forum_open_post(ui->d.store, tid, &err);
            (void)ccord_store_select_channel(ui->d.store, tid, &err);
          }
          ui_mark_dirty(ui);
          return true;
        }
      }
      return true;
    }

    case CCORD_IN_TEXT: {
      ccord_text_edit *ed = active_forum_edit(f);
      ccord_error err = {0};
      if (ccord_text_edit_insert(ed, ev->text, strlen(ev->text), &err) == CCORD_OK) {
        if (ed == &f->search_edit) f->cache_valid = false;
        ui_mark_dirty(ui);
        return true;
      }
      return false;
    }

    case CCORD_IN_KEY_DOWN: {
      ccord_text_edit *ed = active_forum_edit(f);
      bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
      switch (ev->key) {
        case CCORD_KEY_BACKSPACE:
          ccord_text_edit_backspace(ed);
          if (ed == &f->search_edit) f->cache_valid = false;
          ui_mark_dirty(ui);
          return true;
        case CCORD_KEY_DELETE:
          ccord_text_edit_delete(ed);
          if (ed == &f->search_edit) f->cache_valid = false;
          ui_mark_dirty(ui);
          return true;
        case CCORD_KEY_LEFT:
          ccord_text_edit_move(ed, -1, shift);
          ui_mark_dirty(ui);
          return true;
        case CCORD_KEY_RIGHT:
          ccord_text_edit_move(ed, +1, shift);
          ui_mark_dirty(ui);
          return true;
        case CCORD_KEY_HOME:
          ccord_text_edit_move(ed, -2, shift);
          ui_mark_dirty(ui);
          return true;
        case CCORD_KEY_END:
          ccord_text_edit_move(ed, +2, shift);
          ui_mark_dirty(ui);
          return true;
        case CCORD_KEY_TAB:
          if (f->composer_open) {
            f->active_field = (f->active_field == 1) ? 2 : 1;
            ui_mark_dirty(ui);
            return true;
          }
          break;
        case CCORD_KEY_ESCAPE:
          if (f->composer_open) {
            f->composer_open = false;
            f->active_field = 0;
            ui_mark_dirty(ui);
            return true;
          }
          break;
        case CCORD_KEY_RETURN:
          if (f->composer_open && (ev->mods & CCORD_MOD_CTRL)) {
            submit_new_forum_post(ui, f);
            return true;
          }
          break;
        default:
          break;
      }
      return false;
    }

    default:
      return false;
  }
}
