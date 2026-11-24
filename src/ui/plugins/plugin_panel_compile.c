// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "plugin_panel_internal.h"

float pp_p_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float pp_p_maxf(float a, float b) { return a > b ? a : b; }
float pp_p_minf(float a, float b) { return a < b ? a : b; }

uint64_t pp_hash_str(const char *s)
{
  uint64_t h = 1469598103934665603ULL;
  for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
  return h;
}

uint64_t pp_widget_key(const char *owner, const char *panel_id, const char *node_id)
{
  return pp_hash_str(owner) ^ (pp_hash_str(panel_id) * 1315423911ULL) ^ (pp_hash_str(node_id) * 2654435761ULL);
}

// ---------------------------------------------------------------- theme token & hex color resolution

static const struct { const char *name; uint8_t token; } k_color_tokens[] = {
#define X(n) {#n, PTOK_##n},
  CCORD_THEME_COLORS(X)
#undef X
};

static int hex_nibble(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool parse_hex_color(const char *s, size_t n, ccord_color *out)
{
  if (!s || s[0] != '#') return false;
  if (n == 4 || n == 5) {
    int r = hex_nibble(s[1]), g = hex_nibble(s[2]), b = hex_nibble(s[3]);
    int a = (n == 5) ? hex_nibble(s[4]) : 15;
    if (r < 0 || g < 0 || b < 0 || a < 0) return false;
    *out = (ccord_color){(uint8_t)(r * 17), (uint8_t)(g * 17), (uint8_t)(b * 17), (uint8_t)(a * 17)};
    return true;
  }
  if (n == 7 || n == 9) {
    int r1 = hex_nibble(s[1]), r2 = hex_nibble(s[2]);
    int g1 = hex_nibble(s[3]), g2 = hex_nibble(s[4]);
    int b1 = hex_nibble(s[5]), b2 = hex_nibble(s[6]);
    int a1 = (n == 9) ? hex_nibble(s[7]) : 15;
    int a2 = (n == 9) ? hex_nibble(s[8]) : 15;
    if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0 || a1 < 0 || a2 < 0) return false;
    *out = (ccord_color){(uint8_t)((r1 << 4) | r2), (uint8_t)((g1 << 4) | g2),
                         (uint8_t)((b1 << 4) | b2), (uint8_t)((a1 << 4) | a2)};
    return true;
  }
  return false;
}

static pcolor parse_pcolor(const ccord_json *j)
{
  size_t n = 0;
  const char *s = j ? ccord_json_string(j, &n) : nullptr;
  if (!s || n == 0) return (pcolor){.kind = PCOL_NONE};
  if (s[0] == '#') {
    ccord_color lit;
    if (parse_hex_color(s, n, &lit)) return (pcolor){.kind = PCOL_LITERAL, .lit = lit};
    return (pcolor){.kind = PCOL_NONE};
  }
  for (size_t i = 0; i < sizeof k_color_tokens / sizeof k_color_tokens[0]; i++) {
    if (strcmp(s, k_color_tokens[i].name) == 0) return (pcolor){.kind = PCOL_TOKEN, .token = k_color_tokens[i].token};
  }
  if (strcmp(s, "text_primary") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_text_normal};
  if (strcmp(s, "bg_hover") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_bg_item_hover};
  if (strcmp(s, "bg_selected") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_bg_item_selected};
  if (strcmp(s, "border") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_border_subtle};
  if (strcmp(s, "online") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_status_online};
  if (strcmp(s, "idle") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_status_idle};
  if (strcmp(s, "dnd") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_status_dnd};
  if (strcmp(s, "offline") == 0) return (pcolor){.kind = PCOL_TOKEN, .token = PTOK_status_offline};
  return (pcolor){.kind = PCOL_NONE};
}

bool pp_resolve_pcolor(const ccord_theme_colors *tc, pcolor pc, ccord_color *out)
{
  if (pc.kind == PCOL_NONE) return false;
  if (pc.kind == PCOL_LITERAL) { *out = pc.lit; return true; }
  switch (pc.token) {
#define X(n) case PTOK_##n: *out = tc->n; return true;
    CCORD_THEME_COLORS(X)
#undef X
    default: return false;
  }
}

static const char *picon_name_str(picon ic)
{
  switch (ic) {
    case PICON_HASH: return "hash"; case PICON_SPEAKER: return "speaker"; case PICON_GEAR: return "gear";
    case PICON_CHEVRON_UP: return "chevron_up"; case PICON_CHEVRON_DOWN: return "chevron_down";
    case PICON_CHEVRON_LEFT: return "chevron_left"; case PICON_CHEVRON_RIGHT: return "chevron_right";
    case PICON_CLOSE: return "close"; case PICON_MEMBERS: return "members";
    case PICON_MIC: return "mic"; case PICON_MIC_MUTED: return "mic_muted";
    case PICON_HEADPHONES: return "headphones"; case PICON_HEADPHONES_DEAF: return "headphones_deafened";
    case PICON_CAMERA: return "camera"; case PICON_SCREEN: return "screen";
    case PICON_DISCONNECT: return "disconnect"; case PICON_SEARCH: return "search";
    case PICON_PIN: return "pin"; case PICON_REPLY: return "reply"; case PICON_EDIT: return "edit";
    case PICON_ATTACHMENT: return "attachment"; case PICON_FILE: return "file";
    case PICON_FORUM: return "forum"; case PICON_USER: return "user"; case PICON_INBOX: return "inbox";
    case PICON_FOLDER: return "folder"; case PICON_PLUS: return "plus"; case PICON_COMPASS: return "compass";
    case PICON_AT: return "at"; default: return "";
  }
}

static picon parse_icon(const ccord_json *j)
{
  size_t n = 0;
  const char *s = j ? ccord_json_string(j, &n) : nullptr;
  if (!s || !n) return PICON_NONE;
  if (strcmp(s, "hash") == 0) return PICON_HASH;
  if (strcmp(s, "speaker") == 0 || strcmp(s, "voice") == 0) return PICON_SPEAKER;
  if (strcmp(s, "gear") == 0 || strcmp(s, "settings") == 0) return PICON_GEAR;
  if (strcmp(s, "chevron_up") == 0) return PICON_CHEVRON_UP;
  if (strcmp(s, "chevron_down") == 0) return PICON_CHEVRON_DOWN;
  if (strcmp(s, "chevron_left") == 0) return PICON_CHEVRON_LEFT;
  if (strcmp(s, "chevron_right") == 0) return PICON_CHEVRON_RIGHT;
  if (strcmp(s, "close") == 0 || strcmp(s, "x") == 0) return PICON_CLOSE;
  if (strcmp(s, "members") == 0 || strcmp(s, "friends") == 0) return PICON_MEMBERS;
  if (strcmp(s, "mic") == 0) return PICON_MIC;
  if (strcmp(s, "mic_muted") == 0) return PICON_MIC_MUTED;
  if (strcmp(s, "headphones") == 0) return PICON_HEADPHONES;
  if (strcmp(s, "headphones_deafened") == 0) return PICON_HEADPHONES_DEAF;
  if (strcmp(s, "camera") == 0) return PICON_CAMERA;
  if (strcmp(s, "screen") == 0) return PICON_SCREEN;
  if (strcmp(s, "disconnect") == 0) return PICON_DISCONNECT;
  if (strcmp(s, "search") == 0) return PICON_SEARCH;
  if (strcmp(s, "pin") == 0) return PICON_PIN;
  if (strcmp(s, "reply") == 0) return PICON_REPLY;
  if (strcmp(s, "edit") == 0) return PICON_EDIT;
  if (strcmp(s, "attachment") == 0) return PICON_ATTACHMENT;
  if (strcmp(s, "file") == 0) return PICON_FILE;
  if (strcmp(s, "forum") == 0) return PICON_FORUM;
  if (strcmp(s, "user") == 0 || strcmp(s, "profile") == 0) return PICON_USER;
  if (strcmp(s, "inbox") == 0) return PICON_INBOX;
  if (strcmp(s, "folder") == 0) return PICON_FOLDER;
  if (strcmp(s, "plus") == 0 || strcmp(s, "add") == 0) return PICON_PLUS;
  if (strcmp(s, "compass") == 0 || strcmp(s, "discover") == 0) return PICON_COMPASS;
  if (strcmp(s, "at") == 0 || strcmp(s, "dm") == 0) return PICON_AT;
  return PICON_NONE;
}

bool ui_panels_try_draw_icon_override(ccord_ui *ui, ccord_renderer *r, const char *icon_name, float cx, float cy, float size)
{
  if (!ui || !r || !ui->d.contrib || !icon_name || !icon_name[0]) return false;
  const char *tex_key = ccord_contrib_find_icon_override(ui->d.contrib, icon_name);
  if (!tex_key || !tex_key[0]) return false;
  if (!ccord_render_has_texture(r, tex_key, nullptr, nullptr)) return false;
  ccord_draw_texture(r, tex_key, (ccord_rect){cx - size * 0.5f, cy - size * 0.5f, size, size}, 255);
  return true;
}

void pp_draw_picon(ccord_ui *ui, ccord_renderer *r, picon ic, const char *raw_name, float cx, float cy, float size,
                       ccord_color col, ccord_color hole_bg)
{
  const char *nm = (raw_name && raw_name[0]) ? raw_name : picon_name_str(ic);
  if (nm[0] && ui_panels_try_draw_icon_override(ui, r, nm, cx, cy, size)) return;
  switch (ic) {
    case PICON_NONE: break;
    case PICON_HASH: ui_icon_hash(r, cx, cy, size, col); break;
    case PICON_SPEAKER: ui_icon_speaker(r, cx, cy, size, col); break;
    case PICON_GEAR: ui_icon_gear(r, cx, cy, size, col, hole_bg); break;
    case PICON_CHEVRON_UP: {
      float s = size * 0.5f, t = size * 0.14f;
      ccord_draw_line(r, (ccord_vec2){cx - s * 0.7f, cy + s * 0.32f}, (ccord_vec2){cx, cy - s * 0.38f}, t, col);
      ccord_draw_line(r, (ccord_vec2){cx, cy - s * 0.38f}, (ccord_vec2){cx + s * 0.7f, cy + s * 0.32f}, t, col);
      break;
    }
    case PICON_CHEVRON_DOWN: ui_icon_chevron(r, cx, cy, size, ICON_DOWN, col); break;
    case PICON_CHEVRON_LEFT: {
      float s = size * 0.5f, t = size * 0.14f;
      ccord_draw_line(r, (ccord_vec2){cx + s * 0.32f, cy - s * 0.7f}, (ccord_vec2){cx - s * 0.38f, cy}, t, col);
      ccord_draw_line(r, (ccord_vec2){cx - s * 0.38f, cy}, (ccord_vec2){cx + s * 0.32f, cy + s * 0.7f}, t, col);
      break;
    }
    case PICON_CHEVRON_RIGHT: ui_icon_chevron(r, cx, cy, size, ICON_RIGHT, col); break;
    case PICON_CLOSE: ui_icon_close(r, cx, cy, size, col); break;
    case PICON_MEMBERS: ui_icon_members(r, cx, cy, size, col, hole_bg); break;
    case PICON_MIC: ui_icon_mic(r, cx, cy, size, col, false); break;
    case PICON_MIC_MUTED: ui_icon_mic(r, cx, cy, size, col, true); break;
    case PICON_HEADPHONES: ui_icon_headphones(r, cx, cy, size, col, false); break;
    case PICON_HEADPHONES_DEAF: ui_icon_headphones(r, cx, cy, size, col, true); break;
    case PICON_CAMERA: ui_icon_camera(r, cx, cy, size, col, false); break;
    case PICON_SCREEN: ui_icon_screen(r, cx, cy, size, col); break;
    case PICON_DISCONNECT: ui_icon_disconnect(r, cx, cy, size, col); break;
    case PICON_SEARCH: ui_icon_search(r, cx, cy, size, col); break;
    case PICON_PIN: ui_icon_pin(r, cx, cy, size, col); break;
    case PICON_REPLY: ui_icon_reply(r, cx, cy, size, col); break;
    case PICON_EDIT: ui_icon_edit(r, cx, cy, size, col); break;
    case PICON_ATTACHMENT: ui_icon_attachment(r, cx, cy, size, col); break;
    case PICON_FILE: ui_icon_file(r, cx, cy, size, col); break;
    case PICON_FORUM: ui_icon_forum(r, cx, cy, size, col); break;
    case PICON_USER: ui_icon_user_profile(r, cx, cy, size, col, hole_bg); break;
    case PICON_INBOX: ui_icon_inbox(r, cx, cy, size, col, hole_bg); break;
    case PICON_FOLDER: ui_icon_folder(r, cx, cy, size, col); break;
    case PICON_PLUS: ui_icon_plus(r, cx, cy, size, col); break;
    case PICON_COMPASS: ui_icon_compass(r, cx, cy, size, col, hole_bg); break;
    case PICON_AT: {
      ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = pp_p_maxf(size, 10.0f), .color = col};
      ccord_vec2 m = ccord_text_measure(r, "@", 1, &st, 0.0f);
      ccord_text_draw(r, "@", 1, cx - m.x * 0.5f, cy - m.y * 0.5f, &st, 0.0f);
      break;
    }
  }
}

// ---------------------------------------------------------------- Texture sync

ui_panel_mgr *pp_ensure_mgr(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (!ui->panel_mgr) ui->panel_mgr = ccord_calloc(1, sizeof *ui->panel_mgr);
  return ui->panel_mgr;
}

void ui_panels_sync_textures(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui->d.contrib) return;
  uint64_t rev = ccord_contrib_textures_revision(ui->d.contrib);
  if (rev == 0) return;
  ui_panel_mgr *m = pp_ensure_mgr(ui);
  if (!m || m->synced_tex_rev == rev) return;
  m->synced_tex_rev = rev;

  // 1. Evict textures removed from contrib (and their derived grayscale counterparts).
  size_t widx = 0;
  for (size_t i = 0; i < m->nsynced_texs; i++) {
    const ccord_plugin_texture *cur = ccord_contrib_find_texture(ui->d.contrib, m->synced_texs[i].key);
    if (!cur) {
      ccord_render_evict_texture(r, m->synced_texs[i].key);
      char gkey[160];
      snprintf(gkey, sizeof gkey, "__gray__:%s", m->synced_texs[i].key);
      ccord_render_evict_texture(r, gkey);
    } else {
      m->synced_texs[widx++] = m->synced_texs[i];
    }
  }
  m->nsynced_texs = widx;

  // 2. Incrementally upload new or updated textures; evict stale grayscale derivations on update.
  size_t n = ccord_contrib_texture_count(ui->d.contrib);
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_texture *t = ccord_contrib_texture_at(ui->d.contrib, i);
    if (!t || !t->key || !t->data || t->len == 0) continue;
    size_t slot = m->nsynced_texs;
    for (size_t j = 0; j < m->nsynced_texs; j++) {
      if (strcmp(m->synced_texs[j].key, t->key) == 0) {
        slot = j;
        break;
      }
    }
    if (slot < m->nsynced_texs && m->synced_texs[slot].rev == t->revision) continue;

    ccord_error err = {0};
    ccord_status st = CCORD_OK;
    if (t->is_rgba) {
      st = ccord_render_upload_rgba(r, t->key, t->data, t->width, t->height, &err);
    } else {
      int w = 0, h = 0;
      st = ccord_render_upload_image(r, t->key, t->data, t->len, &w, &h, &err);
    }
    char gkey[160];
    snprintf(gkey, sizeof gkey, "__gray__:%s", t->key);
    ccord_render_evict_texture(r, gkey);

    if (st == CCORD_OK) {
      if (slot == m->nsynced_texs) {
        if (m->nsynced_texs == m->capsynced_texs) {
          size_t ncap = m->capsynced_texs ? m->capsynced_texs * 2u : 32u;
          synced_tex_entry *g = ccord_realloc_array(m->synced_texs, ncap, sizeof *g);
          if (g) { m->synced_texs = g; m->capsynced_texs = ncap; }
        }
        if (m->nsynced_texs < m->capsynced_texs) {
          snprintf(m->synced_texs[slot].key, sizeof m->synced_texs[slot].key, "%s", t->key);
          m->nsynced_texs++;
        }
      }
      if (slot < m->nsynced_texs) m->synced_texs[slot].rev = t->revision;
    }
  }
}

// ---------------------------------------------------------------- JSON tree compiler (v2)

static float j_num(const ccord_json *obj, const char *key, float def)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  if (!j || ccord_json_type_of(j) != CCORD_JSON_NUMBER) return def;
  return (float)ccord_json_number(j, (double)def);
}

static bool j_bool(const ccord_json *obj, const char *key, bool def)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? ccord_json_bool(j, def) : def;
}

static const char *j_astr(ccord_arena *a, const ccord_json *obj, const char *key)
{
  size_t n = 0;
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  const char *s = j ? ccord_json_string(j, &n) : nullptr;
  return s ? ccord_arena_strndup(a, s, n) : nullptr;
}

static pdim j_dim(const ccord_json *obj, const char *key, float def)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  if (!j) return (pdim){.val = def, .is_pct = false};
  if (ccord_json_type_of(j) == CCORD_JSON_NUMBER) {
    return (pdim){.val = (float)ccord_json_number(j, (double)def), .is_pct = false};
  }
  size_t n = 0;
  const char *s = ccord_json_string(j, &n);
  if (s && n > 1 && s[n - 1] == '%') {
    char *end = nullptr;
    float v = strtof(s, &end);
    if (end == s + (n - 1)) return (pdim){.val = v * 0.01f, .is_pct = true};
  }
  return (pdim){.val = def, .is_pct = false};
}

float pp_eval_dim(pdim d, float span) { return d.is_pct ? d.val * span : d.val; }

static ccord_cursor_kind parse_cursor(const ccord_json *obj, bool has_id)
{
  size_t n = 0;
  const ccord_json *j = obj ? ccord_json_get(obj, "cursor") : nullptr;
  const char *s = j ? ccord_json_string(j, &n) : nullptr;
  if (!s) return has_id ? CCORD_CURSOR_POINTER : CCORD_CURSOR_DEFAULT;
  if (strcmp(s, "pointer") == 0) return CCORD_CURSOR_POINTER;
  if (strcmp(s, "text") == 0) return CCORD_CURSOR_TEXT;
  return CCORD_CURSOR_DEFAULT;
}

static float parse_font_size(const ccord_json *obj)
{
  const ccord_json *j = obj ? ccord_json_get(obj, "size") : nullptr;
  if (!j) return -1.0f;
  if (ccord_json_type_of(j) == CCORD_JSON_NUMBER) {
    return pp_p_clampf((float)ccord_json_number(j, 13.0), 8.0f, 64.0f);
  }
  size_t n = 0;
  const char *s = ccord_json_string(j, &n);
  if (!s) return -1.0f;
  if (strcmp(s, "large") == 0) return -3.0f;
  if (strcmp(s, "base") == 0) return -2.0f;
  return -1.0f;
}

float pp_resolve_font_size(const ccord_ui *ui, float fs)
{
  if (fs > 0.0f) return fs;
  if (fs <= -2.5f) return ui->theme->m.font_size_large;
  if (fs <= -1.5f) return ui->theme->m.font_size_base;
  return ui->theme->m.font_size_small;
}

static void compile_options(ccord_arena *arena, const ccord_json *arr, pkv_opt **out_opts, size_t *out_n)
{
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return;
  size_t n = ccord_json_array_len(arr);
  if (n > PANEL_MAX_OPTS) n = PANEL_MAX_OPTS;
  if (n == 0) return;
  pkv_opt *opts = ccord_arena_alloc(arena, n * sizeof *opts, _Alignof(pkv_opt));
  if (!opts) return;
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *it = ccord_json_array_at(arr, i);
    if (!it) continue;
    if (ccord_json_type_of(it) == CCORD_JSON_STRING) {
      size_t sl = 0; const char *s = ccord_json_string(it, &sl);
      const char *cp = ccord_arena_strndup(arena, s ? s : "", sl);
      opts[cnt++] = (pkv_opt){.label = cp ? cp : "", .value = cp ? cp : ""};
    } else if (ccord_json_type_of(it) == CCORD_JSON_OBJECT) {
      const char *lbl = j_astr(arena, it, "label");
      const char *val = j_astr(arena, it, "value");
      if (!val) val = j_astr(arena, it, "id");
      if (!lbl) lbl = val;
      if (!val) val = lbl;
      opts[cnt++] = (pkv_opt){.label = lbl ? lbl : "", .value = val ? val : ""};
    }
  }
  *out_opts = opts;
  *out_n = cnt;
}

pnode *pp_compile_node_into(ccord_arena *arena, uint32_t *node_count, const ccord_json *j, int depth)
{
  if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT || depth > PANEL_MAX_DEPTH || *node_count >= PANEL_MAX_NODES) {
    return nullptr;
  }
  size_t tl = 0;
  const char *tstr = ccord_json_string(ccord_json_get(j, "type"), &tl);
  if (!tstr) tstr = "box";

  pnode *nd = ccord_arena_alloc(arena, sizeof *nd, _Alignof(pnode));
  if (!nd) return nullptr;
  memset(nd, 0, sizeof *nd);
  nd->opacity = 1.0f;
  (*node_count)++;

  if (strcmp(tstr, "row") == 0) { nd->kind = PN_BOX; nd->is_col = false; }
  else if (strcmp(tstr, "col") == 0) { nd->kind = PN_BOX; nd->is_col = true; }
  else if (strcmp(tstr, "box") == 0) {
    nd->kind = PN_BOX;
    size_t dl = 0; const char *dir = ccord_json_string(ccord_json_get(j, "direction"), &dl);
    nd->is_col = (dir && strcmp(dir, "col") == 0);
  } else if (strcmp(tstr, "scroll") == 0) { nd->kind = PN_SCROLL; nd->is_col = true; nd->clip = true; }
  else if (strcmp(tstr, "text") == 0) nd->kind = PN_TEXT;
  else if (strcmp(tstr, "markdown") == 0) nd->kind = PN_MARKDOWN;
  else if (strcmp(tstr, "icon") == 0) nd->kind = PN_ICON;
  else if (strcmp(tstr, "button") == 0) nd->kind = PN_BUTTON;
  else if (strcmp(tstr, "spacer") == 0) nd->kind = PN_SPACER;
  else if (strcmp(tstr, "divider") == 0) nd->kind = PN_DIVIDER;
  else if (strcmp(tstr, "canvas") == 0) nd->kind = PN_CANVAS;
  else if (strcmp(tstr, "input") == 0) nd->kind = PN_INPUT;
  else if (strcmp(tstr, "textarea") == 0) nd->kind = PN_TEXTAREA;
  else if (strcmp(tstr, "link") == 0) nd->kind = PN_LINK;
  else if (strcmp(tstr, "toggle") == 0) nd->kind = PN_TOGGLE;
  else if (strcmp(tstr, "checkbox") == 0) nd->kind = PN_CHECKBOX;
  else if (strcmp(tstr, "radio") == 0) nd->kind = PN_RADIO;
  else if (strcmp(tstr, "slider") == 0) nd->kind = PN_SLIDER;
  else if (strcmp(tstr, "select") == 0) nd->kind = PN_SELECT;
  else if (strcmp(tstr, "tabs") == 0) nd->kind = PN_TABS;
  else if (strcmp(tstr, "chip") == 0 || strcmp(tstr, "badge") == 0) nd->kind = PN_CHIP;
  else if (strcmp(tstr, "progress") == 0) nd->kind = PN_PROGRESS;
  else if (strcmp(tstr, "image") == 0) nd->kind = PN_IMAGE;
  else if (strcmp(tstr, "avatar") == 0) nd->kind = PN_AVATAR;
  else nd->kind = PN_BOX;

  nd->id = j_astr(arena, j, "id");
  nd->tooltip = j_astr(arena, j, "tooltip");
  nd->cursor = parse_cursor(j, nd->id != nullptr);
  nd->show_on_hover = j_bool(j, "show_on_hover", false);
  nd->hide_on_hover = j_bool(j, "hide_on_hover", false);
  nd->draggable = j_bool(j, "draggable", false);
  nd->track_move = j_bool(j, "track_move", false);
  nd->disabled = j_bool(j, "disabled", false);
  nd->opacity = pp_p_clampf(j_num(j, "opacity", 1.0f), 0.0f, 1.0f);

  size_t posl = 0;
  const char *pos = ccord_json_string(ccord_json_get(j, "pos"), &posl);
  if (pos && strcmp(pos, "absolute") == 0) {
    nd->pos_abs = true;
    nd->left_x = j_num(j, "left", j_num(j, "x", 0.0f));
    nd->top_y = j_num(j, "top", j_num(j, "y", 0.0f));
  }

  nd->w = pp_p_maxf(j_num(j, "w", 0.0f), 0.0f);
  nd->h = pp_p_maxf(j_num(j, "h", 0.0f), 0.0f);
  nd->min_w = pp_p_maxf(j_num(j, "min_w", 0.0f), 0.0f);
  nd->max_w = pp_p_maxf(j_num(j, "max_w", 0.0f), 0.0f);
  nd->min_h = pp_p_maxf(j_num(j, "min_h", 0.0f), 0.0f);
  nd->max_h = pp_p_maxf(j_num(j, "max_h", 0.0f), 0.0f);
  pdim wdim = j_dim(j, "w", 0.0f);
  float def_flex = ((nd->kind == PN_SPACER && nd->w == 0.0f && nd->h == 0.0f) || (wdim.is_pct && wdim.val >= 0.99f))
                     ? 1.0f : 0.0f;
  nd->flex = pp_p_maxf(j_num(j, "flex", def_flex), 0.0f);

  if (nd->kind == PN_BOX || nd->kind == PN_SCROLL) {
    nd->gap = pp_p_maxf(j_num(j, "gap", 0.0f), 0.0f);
    nd->wrap = j_bool(j, "wrap", false);
    const ccord_json *padj = ccord_json_get(j, "padding");
    if (padj && ccord_json_type_of(padj) == CCORD_JSON_ARRAY) {
      size_t pn = ccord_json_array_len(padj);
      if (pn == 2u) {
        float py = pp_p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 0), 0.0), 0.0f);
        float px = pp_p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 1), 0.0), 0.0f);
        nd->pad_t = nd->pad_b = py; nd->pad_l = nd->pad_r = px;
      } else if (pn >= 4u) {
        nd->pad_t = pp_p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 0), 0.0), 0.0f);
        nd->pad_r = pp_p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 1), 0.0), 0.0f);
        nd->pad_b = pp_p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 2), 0.0), 0.0f);
        nd->pad_l = pp_p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 3), 0.0), 0.0f);
      }
    } else {
      float pad = pp_p_maxf(j_num(j, "padding", 0.0f), 0.0f);
      float px = pp_p_maxf(j_num(j, "pad_x", pad), 0.0f);
      float py = pp_p_maxf(j_num(j, "pad_y", pad), 0.0f);
      nd->pad_l = pp_p_maxf(j_num(j, "pad_left", px), 0.0f);
      nd->pad_r = pp_p_maxf(j_num(j, "pad_right", px), 0.0f);
      nd->pad_t = pp_p_maxf(j_num(j, "pad_top", py), 0.0f);
      nd->pad_b = pp_p_maxf(j_num(j, "pad_bottom", py), 0.0f);
    }
    nd->radius = pp_p_maxf(j_num(j, "radius", 0.0f), 0.0f);
    nd->clip = j_bool(j, "clip", nd->kind == PN_SCROLL);
    nd->bg = parse_pcolor(ccord_json_get(j, "bg"));
    nd->hover_bg = parse_pcolor(ccord_json_get(j, "hover_bg"));
    nd->border_color = parse_pcolor(ccord_json_get(j, "border_color"));
    nd->hover_border_color = parse_pcolor(ccord_json_get(j, "hover_border_color"));
    float def_bw = (nd->border_color.kind != PCOL_NONE || nd->hover_border_color.kind != PCOL_NONE) ? 1.0f : 0.0f;
    nd->border_w = pp_p_maxf(j_num(j, "border_w", j_num(j, "border_width", def_bw)), 0.0f);
    size_t bsl = 0; const char *bs = ccord_json_string(ccord_json_get(j, "border_sides"), &bsl);
    if (bs && strcmp(bs, "bottom") == 0) nd->border_sides = PBORD_BOTTOM;
    else if (bs && strcmp(bs, "top") == 0) nd->border_sides = PBORD_TOP;
    else if (bs && strcmp(bs, "left") == 0) nd->border_sides = PBORD_LEFT;
    else if (bs && strcmp(bs, "right") == 0) nd->border_sides = PBORD_RIGHT;
    else if (bs && strcmp(bs, "no_bottom") == 0) nd->border_sides = PBORD_NO_BOTTOM;
    else nd->border_sides = PBORD_ALL;
    size_t al = 0; const char *as = ccord_json_string(ccord_json_get(j, "align"), &al);
    if (as && strcmp(as, "start") == 0) nd->align = PALIGN_START;
    else if (as && strcmp(as, "end") == 0) nd->align = PALIGN_END;
    else if (as && strcmp(as, "stretch") == 0) nd->align = PALIGN_STRETCH;
    else if (as && strcmp(as, "center") == 0) nd->align = PALIGN_CENTER;
    else nd->align = nd->is_col ? PALIGN_STRETCH : PALIGN_CENTER;
    size_t jl = 0; const char *js = ccord_json_string(ccord_json_get(j, "justify"), &jl);
    if (js && strcmp(js, "center") == 0) nd->justify = PJUST_CENTER;
    else if (js && strcmp(js, "end") == 0) nd->justify = PJUST_END;
    else if (js && strcmp(js, "space_between") == 0) nd->justify = PJUST_BETWEEN;
    else nd->justify = PJUST_START;
    const ccord_json *ch = ccord_json_get(j, "children");
    if (ch && ccord_json_type_of(ch) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(ch);
      if (n > 0) {
        nd->children = ccord_arena_alloc(arena, n * sizeof(pnode *), _Alignof(pnode *));
        if (nd->children) {
          for (size_t i = 0; i < n; i++) {
            pnode *c = pp_compile_node_into(arena, node_count, ccord_json_array_at(ch, i), depth + 1);
            if (c) nd->children[nd->nchildren++] = c;
          }
        }
      }
    }
  } else if (nd->kind == PN_TEXT || nd->kind == PN_MARKDOWN) {
    nd->text = j_astr(arena, j, "text");
    if (!nd->text) nd->text = "";
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
    nd->hover_fg = parse_pcolor(ccord_json_get(j, "hover_color"));
    nd->font_size = parse_font_size(j);
    size_t fnl = 0; const char *fns = ccord_json_string(ccord_json_get(j, "font"), &fnl);
    nd->bold = j_bool(j, "bold", (fns && strcmp(fns, "bold") == 0));
    nd->ellipsis = j_bool(j, "ellipsis", nd->kind == PN_TEXT);
    float ml = j_num(j, "max_lines", 0.0f);
    if (ml >= 1.0f) nd->max_lines = (uint16_t)pp_p_clampf(ml, 1.0f, 1000.0f);
    size_t al = 0; const char *as = ccord_json_string(ccord_json_get(j, "align"), &al);
    if (as && strcmp(as, "center") == 0) nd->align = PALIGN_CENTER;
    else if (as && strcmp(as, "right") == 0) nd->align = PALIGN_END;
    else nd->align = PALIGN_START;
  } else if (nd->kind == PN_LINK) {
    nd->url = j_astr(arena, j, "url");
    if (!nd->url) nd->url = j_astr(arena, j, "href");
    if (!nd->url) nd->url = "";
    nd->text = j_astr(arena, j, "text");
    if (!nd->text) nd->text = j_astr(arena, j, "label");
    if (!nd->text || !nd->text[0]) nd->text = nd->url;
    if (!nd->tooltip && nd->url[0]) nd->tooltip = nd->url;
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
    nd->hover_fg = parse_pcolor(ccord_json_get(j, "hover_color"));
    nd->font_size = parse_font_size(j);
    nd->bold = j_bool(j, "bold", false);
    nd->ellipsis = j_bool(j, "ellipsis", true);
    nd->cursor = nd->disabled ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_POINTER;
  } else if (nd->kind == PN_ICON) {
    const ccord_json *icj = ccord_json_get(j, "name");
    if (!icj) icj = ccord_json_get(j, "icon");
    nd->icon = parse_icon(icj);
    nd->icon_name = j_astr(arena, j, "name");
    if (!nd->icon_name) nd->icon_name = j_astr(arena, j, "icon");
    nd->icon_size = pp_p_clampf(j_num(j, "size", 14.0f), 6.0f, 64.0f);
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
    nd->hover_fg = parse_pcolor(ccord_json_get(j, "hover_color"));
  } else if (nd->kind == PN_BUTTON) {
    nd->text = j_astr(arena, j, "label");
    nd->icon = parse_icon(ccord_json_get(j, "icon"));
    nd->icon_name = j_astr(arena, j, "icon");
    nd->icon_size = pp_p_clampf(j_num(j, "icon_size", 12.0f), 6.0f, 32.0f);
    nd->selected = j_bool(j, "selected", j_bool(j, "active", false));
    nd->cursor = nd->disabled ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_POINTER;
    size_t vl = 0; const char *vs = ccord_json_string(ccord_json_get(j, "variant"), &vl);
    if (vs && strcmp(vs, "primary") == 0) nd->btn_variant = PBTN_PRIMARY;
    else if (vs && strcmp(vs, "danger") == 0) nd->btn_variant = PBTN_DANGER;
    else if (vs && strcmp(vs, "secondary") == 0) nd->btn_variant = PBTN_SECONDARY;
    else if (vs && strcmp(vs, "ghost") == 0) nd->btn_variant = PBTN_GHOST;
    else nd->btn_variant = (nd->text && nd->text[0]) ? PBTN_SECONDARY : PBTN_GHOST;
    size_t sl = 0; const char *ss = ccord_json_string(ccord_json_get(j, "size"), &sl);
    if (ss && strcmp(ss, "xs") == 0) nd->btn_sz = PBTN_XS;
    else if (ss && strcmp(ss, "md") == 0) nd->btn_sz = PBTN_MD;
    else nd->btn_sz = PBTN_SM;
  } else if (nd->kind == PN_DIVIDER) {
    size_t ol = 0; const char *os = ccord_json_string(ccord_json_get(j, "orientation"), &ol);
    if (os) { nd->orient_explicit = true; nd->is_vertical = (strcmp(os, "vertical") == 0); }
    else if (ccord_json_get(j, "vertical") != nullptr) { nd->orient_explicit = true; nd->is_vertical = j_bool(j, "vertical", false); }
    nd->inset = pp_p_maxf(j_num(j, "inset", 6.0f), 0.0f);
    nd->thickness = pp_p_clampf(j_num(j, "thickness", 1.0f), 1.0f, 8.0f);
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
  } else if (nd->kind == PN_INPUT) {
    nd->value_str = j_astr(arena, j, "value");
    nd->placeholder = j_astr(arena, j, "placeholder");
    nd->masked = j_bool(j, "masked", j_bool(j, "password", false));
    nd->cursor = nd->disabled ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_TEXT;
  } else if (nd->kind == PN_TEXTAREA) {
    nd->value_str = j_astr(arena, j, "value");
    nd->placeholder = j_astr(arena, j, "placeholder");
    nd->rows = (uint16_t)pp_p_clampf(j_num(j, "rows", 3.0f), 1.0f, 24.0f);
    nd->submit_on_ctrl_enter = j_bool(j, "submit_on_ctrl_enter", true);
    float mh = j_num(j, "max_height", 0.0f);
    if (mh > 0.0f && (nd->max_h <= 0.0f || mh < nd->max_h)) nd->max_h = mh;
    nd->cursor = nd->disabled ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_TEXT;
  } else if (nd->kind == PN_TOGGLE || nd->kind == PN_CHECKBOX || nd->kind == PN_RADIO) {
    nd->text = j_astr(arena, j, "label");
    nd->value_str = j_astr(arena, j, "value");
    nd->checked = j_bool(j, "checked", j_bool(j, "value", false));
    nd->cursor = nd->disabled ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_POINTER;
  } else if (nd->kind == PN_SLIDER) {
    nd->num_min = j_num(j, "min", 0.0f);
    nd->num_max = j_num(j, "max", 100.0f);
    if (nd->num_max <= nd->num_min) nd->num_max = nd->num_min + 1.0f;
    nd->num_val = pp_p_clampf(j_num(j, "value", nd->num_min), nd->num_min, nd->num_max);
    nd->num_step = pp_p_maxf(j_num(j, "step", 0.0f), 0.0f);
    nd->cursor = nd->disabled ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_POINTER;
  } else if (nd->kind == PN_SELECT) {
    nd->value_str = j_astr(arena, j, "value");
    nd->placeholder = j_astr(arena, j, "placeholder");
    nd->searchable = j_bool(j, "searchable", false);
    nd->multiple = j_bool(j, "multiple", false);
    compile_options(arena, ccord_json_get(j, "options"), &nd->opts, &nd->nopts);
    const ccord_json *vals = ccord_json_get(j, "values");
    if (vals && ccord_json_type_of(vals) == CCORD_JSON_ARRAY) {
      size_t vn = ccord_json_array_len(vals);
      if (vn > PANEL_MAX_OPTS) vn = PANEL_MAX_OPTS;
      if (vn > 0) {
        nd->selected_vals = ccord_arena_alloc(arena, vn * sizeof(const char *), _Alignof(const char *));
        if (nd->selected_vals) {
          for (size_t i = 0; i < vn; i++) {
            size_t sl = 0;
            const char *s = ccord_json_string(ccord_json_array_at(vals, i), &sl);
            if (s) {
              const char *cp = ccord_arena_strndup(arena, s, sl);
              if (cp) nd->selected_vals[nd->nselected_vals++] = cp;
            }
          }
        }
      }
    }
    nd->cursor = nd->disabled ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_POINTER;
  } else if (nd->kind == PN_TABS) {
    nd->value_str = j_astr(arena, j, "active");
    if (!nd->value_str) nd->value_str = j_astr(arena, j, "value");
    const ccord_json *arr = ccord_json_get(j, "items");
    if (!arr) arr = ccord_json_get(j, "tabs");
    compile_options(arena, arr, &nd->opts, &nd->nopts);
    nd->gap = pp_p_maxf(j_num(j, "gap", 4.0f), 0.0f);
  } else if (nd->kind == PN_CHIP) {
    nd->text = j_astr(arena, j, "label");
    if (!nd->text) nd->text = j_astr(arena, j, "text");
    if (!nd->text) nd->text = "";
    nd->selected = j_bool(j, "selected", j_bool(j, "active", false));
    nd->bg = parse_pcolor(ccord_json_get(j, "bg"));
    if (nd->bg.kind == PCOL_NONE) nd->bg = parse_pcolor(ccord_json_get(j, "color"));
    nd->fg = parse_pcolor(ccord_json_get(j, "text_color"));
  } else if (nd->kind == PN_PROGRESS) {
    nd->num_val = pp_p_clampf(j_num(j, "value", 0.0f), 0.0f, 1.0f);
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
    nd->bg = parse_pcolor(ccord_json_get(j, "track_color"));
    nd->radius = pp_p_maxf(j_num(j, "radius", 4.0f), 0.0f);
  } else if (nd->kind == PN_IMAGE) {
    nd->texture_key = j_astr(arena, j, "texture");
    if (!nd->texture_key) nd->texture_key = j_astr(arena, j, "src");
    nd->radius = pp_p_maxf(j_num(j, "radius", 0.0f), 0.0f);
    size_t fl = 0; const char *fit = ccord_json_string(ccord_json_get(j, "fit"), &fl);
    nd->cover = !(fit && strcmp(fit, "stretch") == 0);
    nd->grayscale = j_bool(j, "grayscale", false);
  } else if (nd->kind == PN_AVATAR) {
    size_t ul = 0; const char *us = ccord_json_string(ccord_json_get(j, "user_id"), &ul);
    if (us && ul) nd->avatar_uid = (ccord_id)strtoull(us, nullptr, 10);
    nd->icon_size = pp_p_clampf(j_num(j, "size", 28.0f), 14.0f, 128.0f);
    nd->grayscale = j_bool(j, "grayscale", false);
  } else if (nd->kind == PN_CANVAS) {
    nd->clip = j_bool(j, "clip", true);
    const ccord_json *dr = ccord_json_get(j, "draw");
    if (dr && ccord_json_type_of(dr) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(dr);
      if (n > PANEL_MAX_CMDS) n = PANEL_MAX_CMDS;
      if (n > 0) {
        nd->cmds = ccord_arena_alloc(arena, n * sizeof(pcmd), _Alignof(pcmd));
        if (nd->cmds) {
          for (size_t i = 0; i < n; i++) {
            const ccord_json *cj = ccord_json_array_at(dr, i);
            size_t cl = 0; const char *cs = cj ? ccord_json_string(ccord_json_get(cj, "cmd"), &cl) : nullptr;
            if (!cs) continue;
            pcmd cmd = {0};
            cmd.color = parse_pcolor(ccord_json_get(cj, "color"));
            cmd.color2 = parse_pcolor(ccord_json_get(cj, "color2"));
            if (strcmp(cs, "rect") == 0) {
              cmd.kind = PCMD_RECT;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
            } else if (strcmp(cs, "rrect") == 0) {
              cmd.kind = PCMD_RRECT;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
              cmd.r_or_thick = pp_p_maxf(j_num(cj, "r", j_num(cj, "radius", 4.0f)), 0.0f);
            } else if (strcmp(cs, "rrect_stroke") == 0) {
              cmd.kind = PCMD_RRECT_STROKE;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
              cmd.r_or_thick = pp_p_maxf(j_num(cj, "r", j_num(cj, "radius", 4.0f)), 0.0f);
              cmd.stroke_w = pp_p_clampf(j_num(cj, "thickness", 1.5f), 0.5f, 32.0f);
            } else if (strcmp(cs, "circle") == 0) {
              cmd.kind = PCMD_CIRCLE;
              cmd.x = j_dim(cj, "cx", j_num(cj, "x", 0.0f));
              cmd.y = j_dim(cj, "cy", j_num(cj, "y", 0.0f));
              cmd.r_or_thick = pp_p_maxf(j_num(cj, "r", j_num(cj, "radius", 3.0f)), 0.0f);
            } else if (strcmp(cs, "circle_stroke") == 0) {
              cmd.kind = PCMD_CIRCLE_STROKE;
              cmd.x = j_dim(cj, "cx", j_num(cj, "x", 0.0f));
              cmd.y = j_dim(cj, "cy", j_num(cj, "y", 0.0f));
              cmd.r_or_thick = pp_p_maxf(j_num(cj, "r", j_num(cj, "radius", 8.0f)), 0.0f);
              cmd.stroke_w = pp_p_clampf(j_num(cj, "thickness", 2.0f), 0.5f, 32.0f);
            } else if (strcmp(cs, "arc") == 0 || strcmp(cs, "ring") == 0 || strcmp(cs, "pie") == 0) {
              cmd.kind = PCMD_ARC;
              cmd.x = j_dim(cj, "cx", j_num(cj, "x", 0.0f));
              cmd.y = j_dim(cj, "cy", j_num(cj, "y", 0.0f));
              cmd.r_or_thick = pp_p_maxf(j_num(cj, "r", j_num(cj, "radius", 10.0f)), 0.0f);
              cmd.start_deg = j_num(cj, "start_deg", 0.0f);
              cmd.end_deg = j_num(cj, "end_deg", 360.0f);
              cmd.stroke_w = pp_p_clampf(j_num(cj, "thickness", 2.0f), 0.5f, 32.0f);
              cmd.filled = j_bool(cj, "filled", strcmp(cs, "pie") == 0);
            } else if (strcmp(cs, "line") == 0) {
              cmd.kind = PCMD_LINE;
              cmd.x = j_dim(cj, "x1", 0.0f); cmd.y = j_dim(cj, "y1", 0.0f);
              cmd.w = j_dim(cj, "x2", 0.0f); cmd.h = j_dim(cj, "y2", 0.0f);
              cmd.r_or_thick = pp_p_clampf(j_num(cj, "thickness", j_num(cj, "width", 1.5f)), 0.5f, 16.0f);
            } else if (strcmp(cs, "triangle") == 0) {
              cmd.kind = PCMD_TRIANGLE;
              cmd.x = j_dim(cj, "x1", 0.0f); cmd.y = j_dim(cj, "y1", 0.0f);
              cmd.x2 = j_num(cj, "x2", 0.0f); cmd.y2 = j_num(cj, "y2", 0.0f);
              cmd.x3 = j_num(cj, "x3", 0.0f); cmd.y3 = j_num(cj, "y3", 0.0f);
            } else if (strcmp(cs, "polygon") == 0 || strcmp(cs, "polyline") == 0) {
              cmd.kind = PCMD_POLYGON;
              cmd.filled = j_bool(cj, "filled", strcmp(cs, "polygon") == 0);
              cmd.stroke_w = pp_p_clampf(j_num(cj, "thickness", 1.5f), 0.5f, 16.0f);
              const ccord_json *pj = ccord_json_get(cj, "points");
              if (pj && ccord_json_type_of(pj) == CCORD_JSON_ARRAY) {
                size_t pn = ccord_json_array_len(pj);
                if (pn > PANEL_MAX_POLY) pn = PANEL_MAX_POLY;
                if (pn >= 2u) {
                  cmd.pts = ccord_arena_alloc(arena, pn * sizeof(ccord_vec2), _Alignof(ccord_vec2));
                  if (cmd.pts) {
                    for (size_t k = 0; k < pn; k++) {
                      const ccord_json *pt = ccord_json_array_at(pj, k);
                      if (pt && ccord_json_type_of(pt) == CCORD_JSON_ARRAY && ccord_json_array_len(pt) >= 2u) {
                        cmd.pts[cmd.npts++] = (ccord_vec2){
                          (float)ccord_json_number(ccord_json_array_at(pt, 0), 0.0),
                          (float)ccord_json_number(ccord_json_array_at(pt, 1), 0.0),
                        };
                      } else if (pt && ccord_json_type_of(pt) == CCORD_JSON_OBJECT) {
                        cmd.pts[cmd.npts++] = (ccord_vec2){j_num(pt, "x", 0.0f), j_num(pt, "y", 0.0f)};
                      }
                    }
                  }
                }
              }
            } else if (strcmp(cs, "gradient") == 0) {
              cmd.kind = PCMD_GRADIENT;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
              cmd.vertical = j_bool(cj, "vertical", false);
            } else if (strcmp(cs, "image") == 0 || strcmp(cs, "texture") == 0) {
              cmd.kind = PCMD_IMAGE;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
              cmd.r_or_thick = pp_p_maxf(j_num(cj, "r", j_num(cj, "radius", 0.0f)), 0.0f);
              cmd.stroke_w = pp_p_clampf(j_num(cj, "alpha", 1.0f), 0.0f, 1.0f);
              size_t fl = 0; const char *fit = ccord_json_string(ccord_json_get(cj, "fit"), &fl);
              cmd.cover = (fit && strcmp(fit, "cover") == 0);
              cmd.grayscale = j_bool(cj, "grayscale", false);
              cmd.text = j_astr(arena, cj, "texture");
              if (!cmd.text) cmd.text = j_astr(arena, cj, "key");
              if (!cmd.text) cmd.text = "";
            } else if (strcmp(cs, "text") == 0) {
              cmd.kind = PCMD_TEXT;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "max_w", 0.0f);
              cmd.font_size = parse_font_size(cj);
              cmd.bold = j_bool(cj, "bold", false);
              cmd.ellipsis = j_bool(cj, "ellipsis", false);
              cmd.text = j_astr(arena, cj, "text");
              if (!cmd.text) cmd.text = "";
            } else if (strcmp(cs, "icon") == 0) {
              cmd.kind = PCMD_ICON;
              cmd.x = j_dim(cj, "cx", j_num(cj, "x", 0.0f));
              cmd.y = j_dim(cj, "cy", j_num(cj, "y", 0.0f));
              cmd.r_or_thick = pp_p_clampf(j_num(cj, "size", 14.0f), 6.0f, 64.0f);
              const ccord_json *icj = ccord_json_get(cj, "name");
              if (!icj) icj = ccord_json_get(cj, "icon");
              cmd.icon = parse_icon(icj);
              cmd.text = j_astr(arena, cj, "name");
              if (!cmd.text) cmd.text = j_astr(arena, cj, "icon");
            } else if (strcmp(cs, "clip_push") == 0) {
              cmd.kind = PCMD_CLIP_PUSH;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
            } else if (strcmp(cs, "clip_pop") == 0) {
              cmd.kind = PCMD_CLIP_POP;
            } else if (strcmp(cs, "alpha_push") == 0) {
              cmd.kind = PCMD_ALPHA_PUSH;
              cmd.r_or_thick = pp_p_clampf(j_num(cj, "alpha", 1.0f), 0.0f, 1.0f);
            } else if (strcmp(cs, "alpha_pop") == 0) {
              cmd.kind = PCMD_ALPHA_POP;
            } else if (strcmp(cs, "offset_push") == 0) {
              cmd.kind = PCMD_OFFSET_PUSH;
              cmd.x = j_dim(cj, "dx", 0.0f); cmd.y = j_dim(cj, "dy", 0.0f);
            } else if (strcmp(cs, "offset_pop") == 0) {
              cmd.kind = PCMD_OFFSET_POP;
            } else {
              continue;
            }
            nd->cmds[nd->ncmds++] = cmd;
          }
        }
      }
    }
    const ccord_json *ht = ccord_json_get(j, "hits");
    if (ht && ccord_json_type_of(ht) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(ht);
      if (n > 4096u) n = 4096u;
      if (n > 0) {
        nd->hits = ccord_arena_alloc(arena, n * sizeof(phit_spec), _Alignof(phit_spec));
        if (nd->hits) {
          for (size_t i = 0; i < n; i++) {
            const ccord_json *hj = ccord_json_array_at(ht, i);
            const char *hid = j_astr(arena, hj, "id");
            if (!hid || !hid[0]) continue;
            phit_spec hs = {
              .id = hid, .tooltip = j_astr(arena, hj, "tooltip"), .cursor = parse_cursor(hj, true),
              .x = j_dim(hj, "x", 0.0f), .y = j_dim(hj, "y", 0.0f),
              .w = j_dim(hj, "w", 0.0f), .h = j_dim(hj, "h", 0.0f),
              .draggable = j_bool(hj, "draggable", false), .track_move = j_bool(hj, "track_move", false),
            };
            nd->hits[nd->nhits++] = hs;
          }
        }
      }
    }
  }
  return nd;
}

void ui_panels_destroy(ccord_ui *ui)
{
  if (!ui || !ui->panel_mgr) return;
  ui_panel_mgr *m = ui->panel_mgr;
  for (size_t i = 0; i < m->ncompiled; i++) ccord_arena_free(&m->compiled[i].arena);
  for (size_t i = 0; i < m->ninputs; i++) {
    ccord_text_edit_free(&m->inputs[i].ed);
    ccord_free(m->inputs[i].last_seed);
  }
  if (m->select_filter_init) ccord_text_edit_free(&m->select_filter);
  for (size_t i = 0; i < PANEL_DECOR_CACHE_SLOTS; i++) {
    if (m->decor_cache[i].hash) {
      ccord_arena_free(&m->decor_cache[i].arena);
      ccord_free(m->decor_cache[i].json_copy);
    }
  }
  ccord_free(m->compiled);
  ccord_free(m->hits);
  ccord_free(m->boxes);
  ccord_free(m->inputs);
  ccord_free(m->scrolls);
  ccord_free(m->select_opts);
  ccord_free(m->synced_texs);
  ccord_free(m);
  ui->panel_mgr = nullptr;
}

compiled_panel *pp_sync_compiled(ui_panel_mgr *m, const ccord_panel_contribution *pc)
{
  compiled_panel *cp = nullptr;
  for (size_t i = 0; i < m->ncompiled; i++) {
    if (strcmp(m->compiled[i].owner, pc->owner) == 0 && strcmp(m->compiled[i].panel_id, pc->panel_id) == 0) {
      cp = &m->compiled[i];
      break;
    }
  }
  if (!cp) {
    if (m->ncompiled == m->capcompiled) {
      size_t ncap = m->capcompiled ? m->capcompiled * 2 : 4;
      compiled_panel *nc = ccord_realloc_array(m->compiled, ncap, sizeof *nc);
      if (!nc) return nullptr;
      m->compiled = nc;
      m->capcompiled = ncap;
    }
    cp = &m->compiled[m->ncompiled++];
    memset(cp, 0, sizeof *cp);
    snprintf(cp->owner, sizeof cp->owner, "%s", pc->owner);
    snprintf(cp->panel_id, sizeof cp->panel_id, "%s", pc->panel_id);
    ccord_arena_init(&cp->arena, 0);
  }
  if (cp->revision != pc->revision) {
    ccord_arena_reset(&cp->arena);
    cp->root = nullptr;
    cp->node_count = 0;
    cp->revision = pc->revision;
    static const ccord_json_limits k_lim = {PANEL_MAX_DEPTH + 4, 16384, 512u * 1024u};
    ccord_error err = {0};
    const ccord_json *j = ccord_json_parse(&cp->arena, pc->root_json, strlen(pc->root_json), &k_lim, &err);
    if (j) cp->root = pp_compile_node_into(&cp->arena, &cp->node_count, j, 0);
  }
  return cp;
}

void pp_prune_stale_compiled(ui_panel_mgr *m, const ccord_contrib *c)
{
  size_t w = 0;
  for (size_t i = 0; i < m->ncompiled; i++) {
    if (c && ccord_contrib_find_panel(c, m->compiled[i].owner, m->compiled[i].panel_id) != nullptr) {
      if (w != i) m->compiled[w] = m->compiled[i];
      w++;
    } else {
      ccord_arena_free(&m->compiled[i].arena);
    }
  }
  m->ncompiled = w;
}

panel_input_state *pp_get_input_state(ui_panel_mgr *m, uint64_t key, const char *seed, bool is_multiline)
{
  for (size_t i = 0; i < m->ninputs; i++) {
    if (m->inputs[i].key == key) {
      const char *cur_seed = m->inputs[i].last_seed ? m->inputs[i].last_seed : "";
      if (seed && strcmp(cur_seed, seed) != 0 && m->focused_input_key != key) {
        char *dup = ccord_strdup(seed);
        if (dup) {
          ccord_free(m->inputs[i].last_seed);
          m->inputs[i].last_seed = dup;
        }
        ccord_text_edit_clear(&m->inputs[i].ed);
        if (seed[0]) {
          ccord_error err = {0};
          (void)ccord_text_edit_insert(&m->inputs[i].ed, seed, strlen(seed), &err);
        }
      }
      return &m->inputs[i];
    }
  }
  if (m->ninputs == m->capinputs) {
    size_t ncap = m->capinputs ? m->capinputs * 2u : 16u;
    if (ncap > 2048u) return nullptr;
    panel_input_state *g = ccord_realloc_array(m->inputs, ncap, sizeof *g);
    if (!g) return nullptr;
    m->inputs = g;
    m->capinputs = ncap;
  }
  panel_input_state *st = &m->inputs[m->ninputs++];
  memset(st, 0, sizeof *st);
  st->key = key;
  st->is_multiline = is_multiline;
  ccord_text_edit_init(&st->ed, is_multiline ? 16384u : 4096u);
  if (seed && seed[0]) {
    st->last_seed = ccord_strdup(seed);
    ccord_error err = {0};
    (void)ccord_text_edit_insert(&st->ed, seed, strlen(seed), &err);
  }
  return st;
}

panel_scroll_state *pp_get_scroll_state(ui_panel_mgr *m, uint64_t key)
{
  for (size_t i = 0; i < m->nscrolls; i++) {
    if (m->scrolls[i].key == key) return &m->scrolls[i];
  }
  if (m->nscrolls == m->capscrolls) {
    size_t ncap = m->capscrolls ? m->capscrolls * 2u : 16u;
    if (ncap > 2048u) return nullptr;
    panel_scroll_state *g = ccord_realloc_array(m->scrolls, ncap, sizeof *g);
    if (!g) return nullptr;
    m->scrolls = g;
    m->capscrolls = ncap;
  }
  panel_scroll_state *st = &m->scrolls[m->nscrolls++];
  memset(st, 0, sizeof *st);
  st->key = key;
  return st;
}

// ---------------------------------------------------------------- layout integration (19 slots)

float ui_panels_slot_height(const ccord_ui *ui, ccord_panel_slot slot)
{
  if (!ui || !ui->d.contrib || ui->discovery_open || slot >= CCORD_PANEL__COUNT) return 0.0f;
  size_t n = ccord_contrib_panel_count(ui->d.contrib);
  float total = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const ccord_panel_contribution *p = ccord_contrib_panel_at(ui->d.contrib, i);
    if (p && p->visible && p->slot == slot) {
      total += pp_p_clampf(p->height, 12.0f, 360.0f);
    }
  }
  return total;
}

float ui_panels_slot_width(const ccord_ui *ui, ccord_panel_slot slot)
{
  if (!ui || !ui->d.contrib || ui->discovery_open || slot >= CCORD_PANEL__COUNT) return 0.0f;
  size_t n = ccord_contrib_panel_count(ui->d.contrib);
  float total = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const ccord_panel_contribution *p = ccord_contrib_panel_at(ui->d.contrib, i);
    if (p && p->visible && p->slot == slot) {
      float w = p->width > 0.0f ? p->width : 240.0f;
      total += pp_p_clampf(w, 48.0f, 480.0f);
    }
  }
  return total;
}

void ui_panels_apply_layout(ccord_ui *ui)
{
  if (!ui || !ui->d.contrib || ui->discovery_open) return;
  float top_h = ui_panels_slot_height(ui, CCORD_PANEL_TOP);
  float sub_h = ui_panels_slot_height(ui, CCORD_PANEL_SUBHEADER);
  float bot_h = ui_panels_slot_height(ui, CCORD_PANEL_BOTTOM);
  float comp_top_h = ui_panels_slot_height(ui, CCORD_PANEL_COMPOSER_TOP);
  float left_w = ui_panels_slot_width(ui, CCORD_PANEL_LEFT_SIDEBAR);
  float right_w = ui_panels_slot_width(ui, CCORD_PANEL_RIGHT_SIDEBAR);

  if (top_h > 0.0f && ui->layout.message_area.h > top_h + 60.0f) {
    ui->layout.chat_header.y += top_h;
    ui->layout.message_area.y += top_h;
    ui->layout.message_area.h -= top_h;
    if (ui->layout.member_list_visible && ui->layout.member_list.h > top_h) {
      ui->layout.member_list.y += top_h;
      ui->layout.member_list.h -= top_h;
    }
  }
  if (sub_h > 0.0f && ui->layout.message_area.h > sub_h + 60.0f) {
    ui->layout.message_area.y += sub_h;
    ui->layout.message_area.h -= sub_h;
    if (ui->layout.member_list_visible && ui->layout.member_list.h > sub_h) {
      ui->layout.member_list.y += sub_h;
      ui->layout.member_list.h -= sub_h;
    }
  }
  if (bot_h > 0.0f && ui->layout.message_area.h > bot_h + 60.0f) {
    ui->layout.composer.y -= bot_h;
    ui->layout.message_area.h -= bot_h;
    if (ui->layout.member_list_visible && ui->layout.member_list.h > bot_h) {
      ui->layout.member_list.h -= bot_h;
    }
  }
  if (comp_top_h > 0.0f && ui->layout.message_area.h > comp_top_h + 60.0f) {
    ui->layout.message_area.h -= comp_top_h;
  }
  if (left_w > 0.0f && ui->layout.message_area.w > left_w + 220.0f) {
    ui->layout.chat_header.x += left_w;
    ui->layout.chat_header.w -= left_w;
    ui->layout.message_area.x += left_w;
    ui->layout.message_area.w -= left_w;
    ui->layout.composer.x += left_w;
    ui->layout.composer.w -= left_w;
  }
  if (right_w > 0.0f && ui->layout.message_area.w > right_w + 220.0f) {
    ui->layout.message_area.w -= right_w;
    ui->layout.composer.w -= right_w;
    if (ui->layout.member_list_visible) {
      ui->layout.member_list.x -= right_w;
    }
  }
}

bool ui_panels_has_active_full_view(const ccord_ui *ui)
{
  if (!ui || !ui->panel_mgr || !ui->d.contrib || !ui->panel_mgr->full_view_owner[0]) return false;
  const ccord_panel_contribution *p = ccord_contrib_find_panel(
    ui->d.contrib, ui->panel_mgr->full_view_owner, ui->panel_mgr->full_view_id);
  return p && p->visible && p->slot == CCORD_PANEL_FULL_VIEW;
}

bool ui_panels_is_active_full_view(const ccord_ui *ui, const char *owner, const char *panel_id)
{
  if (!ui || !ui->panel_mgr || !owner || !panel_id) return false;
  return ui->panel_mgr->full_view_owner[0] != '\0' &&
         strcmp(ui->panel_mgr->full_view_owner, owner) == 0 &&
         strcmp(ui->panel_mgr->full_view_id, panel_id) == 0;
}

void ui_panels_set_active_full_view(ccord_ui *ui, const char *owner, const char *panel_id)
{
  if (!ui) return;
  ui_panel_mgr *m = pp_ensure_mgr(ui);
  if (!m) return;
  if (!owner || !owner[0] || !panel_id || !panel_id[0]) {
    m->full_view_owner[0] = '\0';
    m->full_view_id[0] = '\0';
    m->full_view_back_rect = (ccord_rect){0};
  } else {
    snprintf(m->full_view_owner, sizeof m->full_view_owner, "%s", owner);
    snprintf(m->full_view_id, sizeof m->full_view_id, "%s", panel_id);
  }
  ui->dirty = true;
}

