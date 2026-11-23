// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

static bool comp_is_select(ccord_component_kind k)
{
  return k == CCORD_COMP_STRING_SELECT || k == CCORD_COMP_USER_SELECT ||
         k == CCORD_COMP_ROLE_SELECT || k == CCORD_COMP_MENTIONABLE_SELECT ||
         k == CCORD_COMP_CHANNEL_SELECT;
}

// The label a button shows (emoji + label, the link arrow); shared by the item walk and the Section accessory layout.
static size_t comp_button_text(const ccord_component *comp, char *out, size_t cap)
{
  const char *lbl = (comp->label && comp->label[0]) ? comp->label
                    : (comp->style == CCORD_BTN_PREMIUM ? "Upgrade" : "");
  const char *emj = (comp->emoji && comp->emoji[0]) ? comp->emoji : "";
  int n;
  if (emj[0] && lbl[0]) {
    n = snprintf(out, cap, "%s %s%s", emj, lbl, comp->style == CCORD_BTN_LINK ? " \xE2\x86\x97" : "");
  } else if (lbl[0] || emj[0]) {
    n = snprintf(out, cap, "%s%s", lbl[0] ? lbl : emj, comp->style == CCORD_BTN_LINK ? " \xE2\x86\x97" : "");
  } else {
    n = snprintf(out, cap, "%s", comp->style == CCORD_BTN_LINK ? "Open \xE2\x86\x97" : "Button");
  }
  if (n < 0) return 0;
  return (size_t)n < cap ? (size_t)n : cap - 1u;
}

static float comp_button_w(ccord_ui *ui, ccord_renderer *r, const ccord_component *comp, float max_w)
{
  char btxt[220];
  size_t bl = comp_button_text(comp, btxt, sizeof btxt);
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
  ccord_vec2 bsz = ccord_text_measure(r, btxt, bl, &bs, 0.0f);
  return clampf(bsz.x + 24.0f, 56.0f, max_w);
}

// One row of buttons and select menus. v1 action rows and the rows / Section accessories of a Components v2 tree share
// it; a v2 item is found again by its custom_id (there is no v1 row index), a v1 item by row + column.
static float comp_items_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p,
                             const ccord_component *items, size_t nitems,
                             float x, float y, float w, bool draw, bool v2)
{
  if (!p || !p->rich || nitems == 0) return 0.0f;
  ccord_id ch_id = ccord_store_selected_channel(ui->d.store);
  ccord_id app_id = p->rich->application_id != CCORD_ID_NONE ? p->rich->application_id : p->author_id;
  float max_w = maxf(w, 60.0f);
  float cx = x;
  float cy = y;
  float row_h = 0.0f;
  ccord_text_style bs_base = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_on_brand));
  for (size_t ci = 0; ci < nitems; ci++) {
    const ccord_component *comp = &items[ci];
    const char *cid = comp->custom_id ? comp->custom_id : "";
    uint64_t state_key = v2 ? (p->msg_id ^ ui_name_id(cid))
                            : (p->msg_id ^ ((uint64_t)p->row_idx << 40) ^ ((uint64_t)ci << 48));
    if (comp->kind == CCORD_COMP_BUTTON) {
      char btxt[220];
      size_t bl = comp_button_text(comp, btxt, sizeof btxt);
      ccord_vec2 bsz = ccord_text_measure(r, btxt, bl, &bs_base, 0.0f);
      float bw = clampf(bsz.x + 24.0f, 56.0f, max_w);
      float bh = 32.0f;
      if (cx > x && cx + bw > x + max_w) {
        cx = x;
        cy += row_h + 6.0f;
        row_h = 0.0f;
      }
      ccord_rect br = {cx, cy, bw, bh};
      if (draw) {
        ui->dbg_component_buttons++;
        ccord_color bg = COL(brand);
        ccord_color fg = COL(text_on_brand);
        switch ((ccord_button_style)comp->style) {
          case CCORD_BTN_PRIMARY: bg = COL(brand); fg = COL(text_on_brand); break;
          case CCORD_BTN_SECONDARY: bg = COL(bg_tertiary); fg = COL(text_normal); break;
          case CCORD_BTN_SUCCESS: bg = COL(status_online); fg = COL(text_on_brand); break;
          case CCORD_BTN_DANGER: bg = COL(danger); fg = COL(text_on_brand); break;
          case CCORD_BTN_LINK: bg = COL(bg_tertiary); fg = COL(text_normal); break;
          case CCORD_BTN_PREMIUM: bg = COL(brand); fg = COL(text_on_brand); break;
        }
        if (comp->disabled) {
          bg = ui_alpha(bg, 0.5f);
          fg = ui_alpha(fg, 0.5f);
        }
        ccord_draw_rrect(r, br, 4.0f, bg);
        if (!comp->disabled) {
          bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && !ui->select_open &&
                     ui->modal_kind == UI_MODAL_NONE && ui_in_rect(ui->mx, ui->my, br);
          ui_state_fill(ui, r, br, 4.0f, "compbtn", state_key, hov, false);
        }
        ccord_text_style bst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), fg);
        ccord_text_draw_ellipsis(r, btxt, bl, br.x + maxf((br.w - bsz.x) * 0.5f, 8.0f),
                                 br.y + (br.h - bsz.y) * 0.5f, maxf(br.w - 16.0f, 8.0f), &bst);
        if (ui->ncomp_hits < UI_COMP_HIT_MAX) {
          ui_comp_hit *hit = &ui->comp_hits[ui->ncomp_hits++];
          *hit = (ui_comp_hit){
            .rect = br,
            .channel_id = ch_id,
            .message_id = p->msg_id,
            .application_id = app_id,
            .message_flags = p->rich->flags,
            .kind = comp->kind,
            .style = comp->style,
            .min_values = comp->min_values,
            .max_values = comp->max_values,
            .disabled = comp->disabled,
            .custom_id = ui_hit_cstr(ui, comp->custom_id),
            .url = (comp->url && strlen(comp->url) < UI_URL_MAX) ? ui_hit_cstr(ui, comp->url) : "",
            .placeholder = "",
            .row_idx = v2 ? SIZE_MAX : p->row_idx,
            .comp_idx = ci,
          };
        }
      }
      cx += bw + 8.0f;
      if (bh > row_h) row_h = bh;
    } else if (comp_is_select(comp->kind)) {
      if (cx > x) {
        cx = x;
        cy += row_h + 6.0f;
        row_h = 0.0f;
      }
      float sw = fminf(max_w, 400.0f);
      float sh = 36.0f;
      ccord_rect sr = {cx, cy, sw, sh};
      if (draw) {
        ui->dbg_component_selects++;
        char sum[192];
        sum[0] = '\0';
        bool has_sel = false;
        bool target_open = ui->select_open && ui->select_target && ui->select_opts &&
                           ui->select_target->message_id == p->msg_id &&
                           (v2 ? strcmp(ui->select_target->custom_id, cid) == 0
                               : (ui->select_target->row_idx == p->row_idx && ui->select_target->comp_idx == ci));
        if (target_open) {
          size_t slen = 0;
          for (size_t oi = 0; oi < ui->select_opt_count; oi++) {
            if (!ui->select_opts[oi].selected) continue;
            has_sel = true;
            const char *ol = ui->select_opts[oi].label;
            int n = snprintf(sum + slen, sizeof sum - slen, "%s%s", slen > 0 ? ", " : "", ol);
            if (n > 0) slen += (size_t)n < sizeof sum - slen ? (size_t)n : sizeof sum - slen - 1u;
          }
        } else if (comp->kind == CCORD_COMP_STRING_SELECT) {
          size_t slen = 0;
          for (size_t oi = 0; oi < comp->option_count; oi++) {
            if (!comp->options[oi].default_) continue;
            has_sel = true;
            const char *ol = comp->options[oi].label ? comp->options[oi].label : "";
            int n = snprintf(sum + slen, sizeof sum - slen, "%s%s", slen > 0 ? ", " : "", ol);
            if (n > 0) slen += (size_t)n < sizeof sum - slen ? (size_t)n : sizeof sum - slen - 1u;
          }
        }
        const char *ph = (comp->placeholder && comp->placeholder[0]) ? comp->placeholder : "Make a selection";
        const char *shown = has_sel && sum[0] ? sum : ph;
        ccord_draw_rrect(r, (ccord_rect){sr.x - 1.0f, sr.y - 1.0f, sr.w + 2.0f, sr.h + 2.0f}, 5.0f, COL(border_subtle));
        ccord_draw_rrect(r, sr, 4.0f, comp->disabled ? ui_alpha(COL(bg_tertiary), 0.5f) : COL(bg_tertiary));
        if (!comp->disabled) {
          bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && !ui->select_open &&
                     ui->modal_kind == UI_MODAL_NONE && ui_in_rect(ui->mx, ui->my, sr);
          ui_state_fill(ui, r, sr, 4.0f, "compsel", state_key, hov, false);
        }
        ccord_color tc = has_sel ? COL(text_normal) : COL(text_placeholder);
        if (comp->disabled) tc = ui_alpha(tc, 0.5f);
        ccord_text_style sst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), tc);
        ccord_text_style cst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small),
                                        comp->disabled ? ui_alpha(COL(text_muted), 0.5f) : COL(text_muted));
        float lh = ui_line_h(ui, sst.size);
        ccord_text_draw_ellipsis(r, shown, strlen(shown), sr.x + 12.0f, sr.y + (sr.h - lh) * 0.5f,
                                 maxf(sr.w - 38.0f, 12.0f), &sst);
        ccord_text_draw(r, "v", 1, sr.x + sr.w - 20.0f, sr.y + (sr.h - lh) * 0.5f, &cst, 0.0f);
        if (ui->ncomp_hits < UI_COMP_HIT_MAX) {
          ui_comp_hit *hit = &ui->comp_hits[ui->ncomp_hits++];
          *hit = (ui_comp_hit){
            .rect = sr,
            .channel_id = ch_id,
            .message_id = p->msg_id,
            .application_id = app_id,
            .message_flags = p->rich->flags,
            .kind = comp->kind,
            .style = comp->style,
            .min_values = comp->min_values,
            .max_values = comp->max_values > 0 ? comp->max_values : 1u,
            .disabled = comp->disabled,
            .custom_id = ui_hit_cstr(ui, comp->custom_id),
            .url = "",
            .placeholder = ui_hit_cstr(ui, comp->placeholder),
            .row_idx = v2 ? SIZE_MAX : p->row_idx,
            .comp_idx = ci,
          };
        }
      }
      cy += sh + 6.0f;
      cx = x;
      row_h = 0.0f;
    }
  }
  float total_h = (cy - y) + row_h;
  if (cy > y && row_h == 0.0f && total_h >= 6.0f) total_h -= 6.0f;
  return total_h;
}

float dc_components_row_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p,
                                 float x, float y, float w, bool draw)
{
  if (!p || !p->rich || p->row_idx >= p->rich->component_row_count) return 0.0f;
  const ccord_component_row *row = &p->rich->components[p->row_idx];
  if (row->component_count == 0) return 0.0f;
  if (draw) ui->dbg_component_rows++;
  return comp_items_walk(ui, r, p, row->components, row->component_count, x, y, w, draw, false);
}

// ---------------------------------------------------------------- components v2 (message flag IS_COMPONENTS_V2)

// Spoiler reveal keys of v2 nodes: a space apart from the attachment keys, addressed by the node's tree path (child i
// of a node is parent * 64 + i + 1, so a node never shares its path with its parent; a validated tree is at most 4
// levels of at most 40 nodes, so the path stays unique).
static ccord_id cv2_spoiler_key(ccord_id msg_id, size_t path)
{
  return (msg_id ^ 0xC200000000000000ULL) + (ccord_id)(path + 1u);
}

// A Thumbnail: an aspect-fit image in a square box, with its own spoiler tile. Returns the drawn height.
static float cv2_thumbnail(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_media_ref *media,
                           float x, float y, float box, bool draw, ccord_id skey)
{
  float tw = box, th = box;
  if (media->width > 0 && media->height > 0) {
    float sc = fminf(1.0f, fminf(box / (float)media->width, box / (float)media->height));
    tw = clampf((float)media->width * sc, 24.0f, box);
    th = clampf((float)media->height * sc, 24.0f, box);
  }
  if (!draw) return th;
  ccord_rect tr = {x, y, tw, th};
  if (media->spoiler && !dc_is_spoiler_revealed(ui, skey)) {
    dc_draw_spoiler_tile(ui, r, tr, skey);
    return th;
  }
  const char *url = ccord_rich_media_url(p->rich, media);
  ccord_draw_rrect(r, tr, 4.0f, COL(bg_tertiary));
  if (url[0] && ui_ensure_media(ui, r, url)) {
    ccord_draw_texture_rounded_anim(r, url, tr, 4.0f, 255, ui_media_animate(ui, tr));
    ui->dbg_attachment_textures_drawn++;
  }
  if (url[0]) {
    const char *one_url = url;
    const char *one_prev = url;
    uint32_t ow = media->width, oh = media->height;
    dc_img_hit_extra extra = {
      .preview_urls = &one_prev,
      .orig_w = &ow,
      .orig_h = &oh,
      .msg_id = p->msg_id,
      .author_id = p->author_id,
    };
    dc_record_img_hit_ex(ui, tr, &one_url, 1u, 0u, &extra);
  }
  return th;
}

#define CV2_MEMO_MAX 128u
typedef struct cv2_memo {
  float h[CV2_MEMO_MAX];
  uint16_t end_idx[CV2_MEMO_MAX];
  size_t count;
  size_t cursor;
  bool valid;
} cv2_memo;

static float cv2_node(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_component *n,
                      float x, float y, float w, bool draw, size_t path, unsigned depth, cv2_memo *memo);

// One node of the tree and everything under it, laid out (draw=false) or drawn. `path` addresses the node for spoiler
// keys; children continue it. When `memo` is provided, a single O(N) measure pass populates every container/node height
// in pre-order so drawing nested containers never re-measures subtrees (avoiding O(2^D) walks).
static float cv2_node_layout(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_component *n,
                             float x, float y, float w, bool draw, size_t path, unsigned depth, cv2_memo *memo)
{
  if (!n || depth > 8u) return 0.0f;   // a validated tree never gets this deep
  size_t my_idx = SIZE_MAX;
  if (memo) {
    if (!draw) {
      if (memo->count < CV2_MEMO_MAX) {
        my_idx = memo->count++;
      } else {
        memo->valid = false;
      }
    } else if (memo->valid) {
      my_idx = memo->cursor++;
    }
  }
  float res = 0.0f;
  switch (n->kind) {
    case CCORD_COMP_CONTAINER: {
      float pad_l = 12.0f, pad_r = 10.0f, pad_v = 10.0f, gap = 6.0f;
      float inner_w = maxf(w - pad_l - pad_r, 48.0f);
      float h = 0.0f;
      if (draw && memo && memo->valid && my_idx < memo->count) {
        h = memo->h[my_idx];
      } else {
        float ch = 0.0f;
        for (size_t i = 0; i < n->child_count; i++) {
          ch += cv2_node(ui, r, p, &n->children[i], x + pad_l, y + pad_v + ch, inner_w, false,
                         path * 64u + i + 1u, depth + 1u, draw ? nullptr : memo) + gap;
        }
        if (ch > 0.0f) ch -= gap;
        h = ch + 2.0f * pad_v;
      }
      ccord_id skey = cv2_spoiler_key(p->msg_id, path);
      if (n->spoiler && !dc_is_spoiler_revealed(ui, skey)) {
        if (draw) {
          dc_draw_spoiler_tile(ui, r, (ccord_rect){x, y, w, h}, skey);
          if (memo && memo->valid && my_idx < memo->count) memo->cursor = memo->end_idx[my_idx];
        }
        res = h;
        break;
      }
      if (draw) {
        ccord_draw_rrect(r, (ccord_rect){x, y, w, h}, 6.0f, COL(bg_secondary));
        if (n->has_accent_color) {
          ccord_draw_rrect(r, (ccord_rect){x, y, 4.0f, h}, 2.0f, ui_rgb_color(n->accent_color));
        } else {
          ccord_draw_rect(r, (ccord_rect){x, y, w, 1.0f}, COL(border_subtle));
          ccord_draw_rect(r, (ccord_rect){x, y + h - 1.0f, w, 1.0f}, COL(border_subtle));
        }
        float cy = y + pad_v;
        for (size_t i = 0; i < n->child_count; i++) {
          cy += cv2_node(ui, r, p, &n->children[i], x + pad_l, cy, inner_w, true, path * 64u + i + 1u,
                         depth + 1u, memo) + gap;
        }
      }
      res = h;
      break;
    }
    case CCORD_COMP_SECTION: {
      // 1-3 TextDisplay children beside an accessory (a Thumbnail or a Button) pinned to the right edge.
      const ccord_component *acc = n->accessory;
      size_t acc_path = path * 64u + n->child_count + 1u;
      float gap = 12.0f, acc_w = 0.0f, acc_h = 0.0f;
      if (acc && acc->kind == CCORD_COMP_BUTTON) {
        acc_w = comp_button_w(ui, r, acc, maxf(w * 0.5f, 56.0f));
      } else if (acc) {
        acc_w = 80.0f;
      }
      float text_w = maxf(w - (acc_w > 0.0f ? acc_w + gap : 0.0f), 48.0f);
      float ty = y;
      for (size_t i = 0; i < n->child_count; i++) {
        if (n->children[i].kind != CCORD_COMP_TEXT_DISPLAY) continue;
        ty += cv2_node(ui, r, p, &n->children[i], x, ty, text_w, draw, path * 64u + i + 1u, depth + 1u, memo) + 2.0f;
      }
      float th = ty > y ? ty - y - 2.0f : 0.0f;
      if (acc) {
        acc_h = cv2_node(ui, r, p, acc, x + w - acc_w, y, acc_w, draw, acc_path, depth + 1u, memo);
      }
      res = maxf(th, acc_h);
      break;
    }
    case CCORD_COMP_TEXT_DISPLAY: {
      const char *txt = n->content ? n->content : "";
      ccord_id prev_id = ui->current_drawing_msg_id;
      ui->current_drawing_msg_id = p->msg_id;
      res = dc_md_body_walk(ui, r, txt, strlen(txt), x, y, w, COL(text_normal), draw);
      ui->current_drawing_msg_id = prev_id;
      break;
    }
    case CCORD_COMP_BUTTON:   // a Section accessory
      res = comp_items_walk(ui, r, p, n, 1u, x, y, w, draw, true);
      break;
    case CCORD_COMP_THUMBNAIL:
      res = cv2_thumbnail(ui, r, p, &n->media, x, y, 80.0f, draw, cv2_spoiler_key(p->msg_id, path));
      break;
    case CCORD_COMP_MEDIA_GALLERY: {
      ccord_attachment tmp[CCORD_RICH_MAX_V2_GALLERY_ITEMS];
      memset(tmp, 0, sizeof tmp);
      size_t cnt = n->gallery_count < CCORD_RICH_MAX_ATTACHMENTS ? n->gallery_count : CCORD_RICH_MAX_ATTACHMENTS;
      for (size_t k = 0; k < cnt; k++) {
        const ccord_media_ref *g = &n->gallery[k];
        tmp[k].url = ccord_rich_media_url(p->rich, g);
        tmp[k].content_type = (g->content_type && g->content_type[0]) ? g->content_type : "image/png";
        tmp[k].width = g->width;
        tmp[k].height = g->height;
        tmp[k].spoiler = g->spoiler;
        // A per-item reveal key (dc_spoiler_att_key flips the top bit, so pre-flip it).
        tmp[k].id = cv2_spoiler_key(p->msg_id, path * 64u + k + 1u) ^ (1ULL << 63);
      }
      decor_part gp = {.kind = DECOR_GALLERY, .att = tmp, .count = cnt, .msg_id = p->msg_id};
      res = dc_gallery_walk(ui, r, &gp, x, y, w, draw);
      if (draw) {
        ui->dbg_gallery_cards++;
        ui->dbg_attachment_cards += cnt;
      }
      break;
    }
    case CCORD_COMP_FILE: {
      ccord_attachment fa = {0};
      fa.filename = n->file_name;
      fa.size = n->file_size;
      fa.url = ccord_rich_media_url(p->rich, &n->media);
      fa.content_type = n->media.content_type;
      fa.spoiler = n->spoiler;
      ccord_id skey = cv2_spoiler_key(p->msg_id, path);
      if (fa.spoiler && !dc_is_spoiler_revealed(ui, skey)) {
        if (draw) dc_draw_spoiler_tile(ui, r, (ccord_rect){x, y, w, 46.0f}, skey);
        res = 46.0f;
        break;
      }
      if (draw) {
        dc_draw_attachment_card(ui, r, &fa, x, y, w);
        ui->dbg_attachment_cards++;
      }
      res = 46.0f;
      break;
    }
    case CCORD_COMP_SEPARATOR: {
      float h = n->spacing == 2u ? 16.0f : 8.0f;
      if (draw && n->divider) ccord_draw_rect(r, (ccord_rect){x, y + h * 0.5f, w, 1.0f}, COL(border_subtle));
      res = h;
      break;
    }
    case CCORD_COMP_ACTION_ROW:
      if (draw) ui->dbg_component_rows++;
      res = comp_items_walk(ui, r, p, n->children, n->child_count, x, y, w, draw, true);
      break;
    default:
      res = 0.0f;   // a kind this build does not know draws nothing
      break;
  }
  if (memo && !draw && my_idx < CV2_MEMO_MAX) {
    memo->h[my_idx] = res;
    memo->end_idx[my_idx] = (uint16_t)memo->count;
  }
  return res;
}

// cv2_node_layout plus the debug tally of what was drawn per kind (count and the last height), read by the UI tests.
static float cv2_node(ccord_ui *ui, ccord_renderer *r, const decor_part *p, const ccord_component *n,
                      float x, float y, float w, bool draw, size_t path, unsigned depth, cv2_memo *memo)
{
  float h = cv2_node_layout(ui, r, p, n, x, y, w, draw, path, depth, memo);
  if (draw && n && (size_t)n->kind < UI_CV2_KIND_SLOTS) {
    ui->dbg_cv2_drawn[n->kind]++;
    ui->dbg_cv2_height[n->kind] = h;
  }
  return h;
}

// The whole v2 tree of one message: its top-level nodes stacked with a small gap.
float dc_cv2_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw)
{
  if (!p || !p->rich || p->rich->v2_component_count == 0) return 0.0f;
  const ccord_component *nodes = p->rich->v2_components;
  if (!draw) {
    float cy = y;
    for (size_t i = 0; i < p->rich->v2_component_count; i++) {
      cy += cv2_node(ui, r, p, &nodes[i], x, cy, w, false, i + 1u, 1u, nullptr) + 6.0f;
    }
    return cy - y - 6.0f;
  }
  // Pre-measure once in O(N) so drawing nested containers never re-measures subtrees.
  cv2_memo memo = {.valid = true};
  bool has_container = false;
  for (size_t i = 0; i < p->rich->v2_component_count; i++) {
    if (nodes[i].kind == CCORD_COMP_CONTAINER) {
      has_container = true;
      break;
    }
  }
  if (has_container) {
    float my = y;
    for (size_t i = 0; i < p->rich->v2_component_count; i++) {
      my += cv2_node(ui, r, p, &nodes[i], x, my, w, false, i + 1u, 1u, &memo) + 6.0f;
    }
  }
  float cy = y;
  cv2_memo *mp = (has_container && memo.valid) ? &memo : nullptr;
  for (size_t i = 0; i < p->rich->v2_component_count; i++) {
    cy += cv2_node(ui, r, p, &nodes[i], x, cy, w, true, i + 1u, 1u, mp) + 6.0f;
  }
  return cy - y - 6.0f;
}

