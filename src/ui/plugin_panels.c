// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_MAX_DEPTH 24
#define PANEL_MAX_NODES 512u
#define PANEL_MAX_HITS  256u

typedef enum pcol_kind : uint8_t { PCOL_NONE = 0, PCOL_TOKEN, PCOL_LITERAL } pcol_kind;

enum {
#define X(n) PTOK_##n,
  CCORD_THEME_COLORS(X)
#undef X
  PTOK__COUNT
};

typedef struct pcolor {
  pcol_kind kind;
  uint8_t token;
  ccord_color lit;
} pcolor;

typedef enum picon : uint8_t {
  PICON_NONE = 0,
  PICON_HASH,
  PICON_SPEAKER,
  PICON_GEAR,
  PICON_CHEVRON_UP,
  PICON_CHEVRON_DOWN,
  PICON_CHEVRON_LEFT,
  PICON_CHEVRON_RIGHT,
  PICON_CLOSE,
  PICON_MEMBERS,
  PICON_MIC,
  PICON_MIC_MUTED,
  PICON_HEADPHONES,
  PICON_HEADPHONES_DEAF,
  PICON_CAMERA,
  PICON_SCREEN,
  PICON_DISCONNECT,
  PICON_SEARCH,
  PICON_PIN,
  PICON_REPLY,
  PICON_EDIT,
  PICON_ATTACHMENT,
  PICON_FILE,
  PICON_FORUM,
  PICON_USER,
  PICON_INBOX,
  PICON_FOLDER,
  PICON_PLUS,
  PICON_COMPASS,
  PICON_AT,
} picon;

typedef enum palign : uint8_t { PALIGN_START = 0, PALIGN_CENTER, PALIGN_END, PALIGN_STRETCH } palign;
typedef enum pjustify : uint8_t { PJUST_START = 0, PJUST_CENTER, PJUST_END, PJUST_BETWEEN } pjustify;
typedef enum pborder_sides : uint8_t {
  PBORD_ALL = 0,
  PBORD_BOTTOM,
  PBORD_TOP,
  PBORD_LEFT,
  PBORD_RIGHT,
  PBORD_NO_BOTTOM,
} pborder_sides;

typedef enum pbtn_variant : uint8_t { PBTN_GHOST = 0, PBTN_SECONDARY, PBTN_PRIMARY, PBTN_DANGER } pbtn_variant;
typedef enum pbtn_sz : uint8_t { PBTN_XS = 0, PBTN_SM, PBTN_MD } pbtn_sz;

typedef enum pcmd_kind : uint8_t {
  PCMD_RECT = 0,
  PCMD_RRECT,
  PCMD_CIRCLE,
  PCMD_LINE,
  PCMD_TRIANGLE,
  PCMD_TEXT,
  PCMD_ICON,
} pcmd_kind;

typedef struct pdim {
  float val;
  bool is_pct;
} pdim;

typedef struct pcmd {
  pcmd_kind kind;
  pdim x, y, w, h;
  float x2, y2, x3, y3;
  float r_or_thick;
  float font_size;
  bool bold;
  bool ellipsis;
  picon icon;
  pcolor color;
  const char *text;
} pcmd;

typedef struct phit_spec {
  const char *id;
  const char *tooltip;
  ccord_cursor_kind cursor;
  pdim x, y, w, h;
} phit_spec;

typedef enum pnode_kind : uint8_t {
  PN_BOX = 0,
  PN_TEXT,
  PN_ICON,
  PN_BUTTON,
  PN_SPACER,
  PN_DIVIDER,
  PN_CANVAS,
} pnode_kind;

typedef struct pnode pnode;
struct pnode {
  pnode_kind kind;
  bool is_col;
  bool clip;
  bool show_on_hover;
  bool hide_on_hover;
  bool bold;
  bool ellipsis;
  bool selected;
  bool disabled;
  bool orient_explicit;
  bool is_vertical;
  palign align;
  pjustify justify;
  pborder_sides border_sides;
  pbtn_variant btn_variant;
  pbtn_sz btn_sz;
  picon icon;
  ccord_cursor_kind cursor;
  float w, h, min_w, max_w, flex;
  float gap;
  float pad_l, pad_r, pad_t, pad_b;
  float radius, border_w;
  float font_size;   // 0 = theme small, < 0 = semantic (-1 small, -2 base, -3 large)
  float icon_size;
  float inset, thickness;
  pcolor bg, hover_bg;
  pcolor border_color, hover_border_color;
  pcolor fg, hover_fg;
  const char *id;
  const char *text;
  const char *tooltip;
  pnode **children;
  size_t nchildren;
  pcmd *cmds;
  size_t ncmds;
  phit_spec *hits;
  size_t nhits;
};

typedef struct compiled_panel {
  char owner[65];
  char panel_id[40];
  uint64_t revision;
  ccord_arena arena;
  pnode *root;
  uint32_t node_count;
} compiled_panel;

typedef struct panel_hit {
  char owner[65];
  char panel_id[40];
  char node_id[65];
  char tooltip[128];
  ccord_cursor_kind cursor;
  ccord_rect rect;
} panel_hit;

typedef struct panel_box {
  ccord_rect rect;
} panel_box;

struct ui_panel_mgr {
  compiled_panel *compiled;
  size_t ncompiled, capcompiled;
  panel_hit hits[PANEL_MAX_HITS];
  size_t nhits;
  panel_box boxes[32];
  size_t nboxes;
};

static float p_clampf(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static float p_maxf(float a, float b)
{
  return a > b ? a : b;
}

static float p_minf(float a, float b)
{
  return a < b ? a : b;
}

// ---------------------------------------------------------------- theme token & hex color resolution

static const struct {
  const char *name;
  uint8_t token;
} k_color_tokens[] = {
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
    if (strcmp(s, k_color_tokens[i].name) == 0) {
      return (pcolor){.kind = PCOL_TOKEN, .token = k_color_tokens[i].token};
    }
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

static bool resolve_pcolor(const ccord_theme_colors *tc, pcolor pc, ccord_color *out)
{
  if (pc.kind == PCOL_NONE) return false;
  if (pc.kind == PCOL_LITERAL) {
    *out = pc.lit;
    return true;
  }
  switch (pc.token) {
#define X(n) case PTOK_##n: *out = tc->n; return true;
    CCORD_THEME_COLORS(X)
#undef X
    default: return false;
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

static void draw_picon(ccord_ui *ui, ccord_renderer *r, picon ic, float cx, float cy, float size, ccord_color col,
                       ccord_color hole_bg)
{
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
      ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = p_maxf(size, 10.0f), .color = col};
      ccord_vec2 m = ccord_text_measure(r, "@", 1, &st, 0.0f);
      ccord_text_draw(r, "@", 1, cx - m.x * 0.5f, cy - m.y * 0.5f, &st, 0.0f);
      (void)ui;
      break;
    }
  }
}

// ---------------------------------------------------------------- JSON tree compiler

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

static float eval_dim(pdim d, float span)
{
  return d.is_pct ? d.val * span : d.val;
}

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
    return p_clampf((float)ccord_json_number(j, 13.0), 8.0f, 48.0f);
  }
  size_t n = 0;
  const char *s = ccord_json_string(j, &n);
  if (!s) return -1.0f;
  if (strcmp(s, "large") == 0) return -3.0f;
  if (strcmp(s, "base") == 0) return -2.0f;
  return -1.0f;
}

static float resolve_font_size(const ccord_ui *ui, float fs)
{
  if (fs > 0.0f) return fs;
  if (fs <= -2.5f) return ui->theme->m.font_size_large;
  if (fs <= -1.5f) return ui->theme->m.font_size_base;
  return ui->theme->m.font_size_small;
}

static pnode *compile_node(compiled_panel *cp, const ccord_json *j, int depth)
{
  if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT || depth > PANEL_MAX_DEPTH || cp->node_count >= PANEL_MAX_NODES) {
    return nullptr;
  }
  size_t tl = 0;
  const char *tstr = ccord_json_string(ccord_json_get(j, "type"), &tl);
  if (!tstr) tstr = "box";

  pnode *nd = ccord_arena_alloc(&cp->arena, sizeof *nd, _Alignof(pnode));
  if (!nd) return nullptr;
  memset(nd, 0, sizeof *nd);
  cp->node_count++;

  if (strcmp(tstr, "row") == 0) { nd->kind = PN_BOX; nd->is_col = false; }
  else if (strcmp(tstr, "col") == 0) { nd->kind = PN_BOX; nd->is_col = true; }
  else if (strcmp(tstr, "box") == 0) {
    nd->kind = PN_BOX;
    size_t dl = 0;
    const char *dir = ccord_json_string(ccord_json_get(j, "direction"), &dl);
    nd->is_col = (dir && strcmp(dir, "col") == 0);
  }
  else if (strcmp(tstr, "text") == 0) nd->kind = PN_TEXT;
  else if (strcmp(tstr, "icon") == 0) nd->kind = PN_ICON;
  else if (strcmp(tstr, "button") == 0) nd->kind = PN_BUTTON;
  else if (strcmp(tstr, "spacer") == 0) nd->kind = PN_SPACER;
  else if (strcmp(tstr, "divider") == 0) nd->kind = PN_DIVIDER;
  else if (strcmp(tstr, "canvas") == 0) nd->kind = PN_CANVAS;
  else nd->kind = PN_BOX;

  nd->id = j_astr(&cp->arena, j, "id");
  nd->tooltip = j_astr(&cp->arena, j, "tooltip");
  nd->cursor = parse_cursor(j, nd->id != nullptr);
  nd->show_on_hover = j_bool(j, "show_on_hover", false);
  nd->hide_on_hover = j_bool(j, "hide_on_hover", false);

  nd->w = p_maxf(j_num(j, "w", 0.0f), 0.0f);
  nd->h = p_maxf(j_num(j, "h", 0.0f), 0.0f);
  nd->min_w = p_maxf(j_num(j, "min_w", 0.0f), 0.0f);
  nd->max_w = p_maxf(j_num(j, "max_w", 0.0f), 0.0f);
  pdim wdim = j_dim(j, "w", 0.0f);
  float def_flex = ((nd->kind == PN_SPACER && nd->w == 0.0f && nd->h == 0.0f) || (wdim.is_pct && wdim.val >= 0.99f))
                     ? 1.0f
                     : 0.0f;
  nd->flex = p_maxf(j_num(j, "flex", def_flex), 0.0f);

  if (nd->kind == PN_BOX) {
    nd->gap = p_maxf(j_num(j, "gap", 0.0f), 0.0f);
    const ccord_json *padj = ccord_json_get(j, "padding");
    if (padj && ccord_json_type_of(padj) == CCORD_JSON_ARRAY) {
      size_t pn = ccord_json_array_len(padj);
      if (pn == 2u) {
        float py = p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 0), 0.0), 0.0f);
        float px = p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 1), 0.0), 0.0f);
        nd->pad_t = nd->pad_b = py;
        nd->pad_l = nd->pad_r = px;
      } else if (pn >= 4u) {
        nd->pad_t = p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 0), 0.0), 0.0f);
        nd->pad_r = p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 1), 0.0), 0.0f);
        nd->pad_b = p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 2), 0.0), 0.0f);
        nd->pad_l = p_maxf((float)ccord_json_number(ccord_json_array_at(padj, 3), 0.0), 0.0f);
      }
    } else {
      float pad = p_maxf(j_num(j, "padding", 0.0f), 0.0f);
      float px = p_maxf(j_num(j, "pad_x", pad), 0.0f);
      float py = p_maxf(j_num(j, "pad_y", pad), 0.0f);
      nd->pad_l = p_maxf(j_num(j, "pad_left", px), 0.0f);
      nd->pad_r = p_maxf(j_num(j, "pad_right", px), 0.0f);
      nd->pad_t = p_maxf(j_num(j, "pad_top", py), 0.0f);
      nd->pad_b = p_maxf(j_num(j, "pad_bottom", py), 0.0f);
    }
    nd->radius = p_maxf(j_num(j, "radius", 0.0f), 0.0f);
    nd->clip = j_bool(j, "clip", false);

    nd->bg = parse_pcolor(ccord_json_get(j, "bg"));
    nd->hover_bg = parse_pcolor(ccord_json_get(j, "hover_bg"));
    nd->border_color = parse_pcolor(ccord_json_get(j, "border_color"));
    nd->hover_border_color = parse_pcolor(ccord_json_get(j, "hover_border_color"));
    float def_bw = (nd->border_color.kind != PCOL_NONE || nd->hover_border_color.kind != PCOL_NONE) ? 1.0f : 0.0f;
    nd->border_w = p_maxf(j_num(j, "border_w", j_num(j, "border_width", def_bw)), 0.0f);

    size_t bsl = 0;
    const char *bs = ccord_json_string(ccord_json_get(j, "border_sides"), &bsl);
    if (bs && strcmp(bs, "bottom") == 0) nd->border_sides = PBORD_BOTTOM;
    else if (bs && strcmp(bs, "top") == 0) nd->border_sides = PBORD_TOP;
    else if (bs && strcmp(bs, "left") == 0) nd->border_sides = PBORD_LEFT;
    else if (bs && strcmp(bs, "right") == 0) nd->border_sides = PBORD_RIGHT;
    else if (bs && strcmp(bs, "no_bottom") == 0) nd->border_sides = PBORD_NO_BOTTOM;
    else nd->border_sides = PBORD_ALL;

    size_t al = 0;
    const char *as = ccord_json_string(ccord_json_get(j, "align"), &al);
    if (as && strcmp(as, "start") == 0) nd->align = PALIGN_START;
    else if (as && strcmp(as, "end") == 0) nd->align = PALIGN_END;
    else if (as && strcmp(as, "stretch") == 0) nd->align = PALIGN_STRETCH;
    else if (as && strcmp(as, "center") == 0) nd->align = PALIGN_CENTER;
    else nd->align = nd->is_col ? PALIGN_STRETCH : PALIGN_CENTER;

    size_t jl = 0;
    const char *js = ccord_json_string(ccord_json_get(j, "justify"), &jl);
    if (js && strcmp(js, "center") == 0) nd->justify = PJUST_CENTER;
    else if (js && strcmp(js, "end") == 0) nd->justify = PJUST_END;
    else if (js && strcmp(js, "space_between") == 0) nd->justify = PJUST_BETWEEN;
    else nd->justify = PJUST_START;

    const ccord_json *ch = ccord_json_get(j, "children");
    if (ch && ccord_json_type_of(ch) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(ch);
      if (n > 0) {
        nd->children = ccord_arena_alloc(&cp->arena, n * sizeof(pnode *), _Alignof(pnode *));
        if (nd->children) {
          for (size_t i = 0; i < n; i++) {
            pnode *c = compile_node(cp, ccord_json_array_at(ch, i), depth + 1);
            if (c) nd->children[nd->nchildren++] = c;
          }
        }
      }
    }
  } else if (nd->kind == PN_TEXT) {
    nd->text = j_astr(&cp->arena, j, "text");
    if (!nd->text) nd->text = "";
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
    nd->hover_fg = parse_pcolor(ccord_json_get(j, "hover_color"));
    nd->font_size = parse_font_size(j);
    size_t fnl = 0;
    const char *fns = ccord_json_string(ccord_json_get(j, "font"), &fnl);
    nd->bold = j_bool(j, "bold", (fns && strcmp(fns, "bold") == 0));
    nd->ellipsis = j_bool(j, "ellipsis", true);
    size_t al = 0;
    const char *as = ccord_json_string(ccord_json_get(j, "align"), &al);
    if (as && strcmp(as, "center") == 0) nd->align = PALIGN_CENTER;
    else if (as && strcmp(as, "right") == 0) nd->align = PALIGN_END;
    else nd->align = PALIGN_START;
  } else if (nd->kind == PN_ICON) {
    const ccord_json *icj = ccord_json_get(j, "name");
    if (!icj) icj = ccord_json_get(j, "icon");
    nd->icon = parse_icon(icj);
    nd->icon_size = p_clampf(j_num(j, "size", 14.0f), 6.0f, 64.0f);
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
    nd->hover_fg = parse_pcolor(ccord_json_get(j, "hover_color"));
  } else if (nd->kind == PN_BUTTON) {
    nd->text = j_astr(&cp->arena, j, "label");
    nd->icon = parse_icon(ccord_json_get(j, "icon"));
    nd->icon_size = p_clampf(j_num(j, "icon_size", 12.0f), 6.0f, 32.0f);
    nd->selected = j_bool(j, "selected", j_bool(j, "active", false));
    nd->disabled = j_bool(j, "disabled", false);
    if (nd->disabled) nd->cursor = CCORD_CURSOR_DEFAULT;
    else nd->cursor = CCORD_CURSOR_POINTER;

    size_t vl = 0;
    const char *vs = ccord_json_string(ccord_json_get(j, "variant"), &vl);
    if (vs && strcmp(vs, "primary") == 0) nd->btn_variant = PBTN_PRIMARY;
    else if (vs && strcmp(vs, "danger") == 0) nd->btn_variant = PBTN_DANGER;
    else if (vs && strcmp(vs, "secondary") == 0) nd->btn_variant = PBTN_SECONDARY;
    else if (vs && strcmp(vs, "ghost") == 0) nd->btn_variant = PBTN_GHOST;
    else nd->btn_variant = (nd->text && nd->text[0]) ? PBTN_SECONDARY : PBTN_GHOST;

    size_t sl = 0;
    const char *ss = ccord_json_string(ccord_json_get(j, "size"), &sl);
    if (ss && strcmp(ss, "xs") == 0) nd->btn_sz = PBTN_XS;
    else if (ss && strcmp(ss, "md") == 0) nd->btn_sz = PBTN_MD;
    else nd->btn_sz = PBTN_SM;
  } else if (nd->kind == PN_DIVIDER) {
    size_t ol = 0;
    const char *os = ccord_json_string(ccord_json_get(j, "orientation"), &ol);
    if (os) {
      nd->orient_explicit = true;
      nd->is_vertical = (strcmp(os, "vertical") == 0);
    } else if (ccord_json_get(j, "vertical") != nullptr) {
      nd->orient_explicit = true;
      nd->is_vertical = j_bool(j, "vertical", false);
    }
    nd->inset = p_maxf(j_num(j, "inset", 6.0f), 0.0f);
    nd->thickness = p_clampf(j_num(j, "thickness", 1.0f), 1.0f, 8.0f);
    nd->fg = parse_pcolor(ccord_json_get(j, "color"));
  } else if (nd->kind == PN_CANVAS) {
    nd->clip = j_bool(j, "clip", true);
    const ccord_json *dr = ccord_json_get(j, "draw");
    if (dr && ccord_json_type_of(dr) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(dr);
      if (n > 256u) n = 256u;
      if (n > 0) {
        nd->cmds = ccord_arena_alloc(&cp->arena, n * sizeof(pcmd), _Alignof(pcmd));
        if (nd->cmds) {
          for (size_t i = 0; i < n; i++) {
            const ccord_json *cj = ccord_json_array_at(dr, i);
            size_t cl = 0;
            const char *cs = cj ? ccord_json_string(ccord_json_get(cj, "cmd"), &cl) : nullptr;
            if (!cs) continue;
            pcmd cmd = {0};
            cmd.color = parse_pcolor(ccord_json_get(cj, "color"));
            if (strcmp(cs, "rect") == 0) {
              cmd.kind = PCMD_RECT;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
            } else if (strcmp(cs, "rrect") == 0) {
              cmd.kind = PCMD_RRECT;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "w", 0.0f); cmd.h = j_dim(cj, "h", 0.0f);
              cmd.r_or_thick = p_maxf(j_num(cj, "r", j_num(cj, "radius", 4.0f)), 0.0f);
            } else if (strcmp(cs, "circle") == 0) {
              cmd.kind = PCMD_CIRCLE;
              cmd.x = j_dim(cj, "cx", j_num(cj, "x", 0.0f));
              cmd.y = j_dim(cj, "cy", j_num(cj, "y", 0.0f));
              cmd.r_or_thick = p_maxf(j_num(cj, "r", j_num(cj, "radius", 3.0f)), 0.0f);
            } else if (strcmp(cs, "line") == 0) {
              cmd.kind = PCMD_LINE;
              cmd.x = j_dim(cj, "x1", 0.0f); cmd.y = j_dim(cj, "y1", 0.0f);
              cmd.w = j_dim(cj, "x2", 0.0f); cmd.h = j_dim(cj, "y2", 0.0f);
              cmd.r_or_thick = p_clampf(j_num(cj, "thickness", j_num(cj, "width", 1.5f)), 0.5f, 16.0f);
            } else if (strcmp(cs, "triangle") == 0) {
              cmd.kind = PCMD_TRIANGLE;
              cmd.x = j_dim(cj, "x1", 0.0f); cmd.y = j_dim(cj, "y1", 0.0f);
              cmd.x2 = j_num(cj, "x2", 0.0f); cmd.y2 = j_num(cj, "y2", 0.0f);
              cmd.x3 = j_num(cj, "x3", 0.0f); cmd.y3 = j_num(cj, "y3", 0.0f);
            } else if (strcmp(cs, "text") == 0) {
              cmd.kind = PCMD_TEXT;
              cmd.x = j_dim(cj, "x", 0.0f); cmd.y = j_dim(cj, "y", 0.0f);
              cmd.w = j_dim(cj, "max_w", 0.0f);
              cmd.font_size = parse_font_size(cj);
              cmd.bold = j_bool(cj, "bold", false);
              cmd.ellipsis = j_bool(cj, "ellipsis", false);
              cmd.text = j_astr(&cp->arena, cj, "text");
              if (!cmd.text) cmd.text = "";
            } else if (strcmp(cs, "icon") == 0) {
              cmd.kind = PCMD_ICON;
              cmd.x = j_dim(cj, "cx", j_num(cj, "x", 0.0f));
              cmd.y = j_dim(cj, "cy", j_num(cj, "y", 0.0f));
              cmd.r_or_thick = p_clampf(j_num(cj, "size", 14.0f), 6.0f, 64.0f);
              const ccord_json *icj = ccord_json_get(cj, "name");
              if (!icj) icj = ccord_json_get(cj, "icon");
              cmd.icon = parse_icon(icj);
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
      if (n > 64u) n = 64u;
      if (n > 0) {
        nd->hits = ccord_arena_alloc(&cp->arena, n * sizeof(phit_spec), _Alignof(phit_spec));
        if (nd->hits) {
          for (size_t i = 0; i < n; i++) {
            const ccord_json *hj = ccord_json_array_at(ht, i);
            const char *hid = j_astr(&cp->arena, hj, "id");
            if (!hid || !hid[0]) continue;
            phit_spec hs = {
              .id = hid,
              .tooltip = j_astr(&cp->arena, hj, "tooltip"),
              .cursor = parse_cursor(hj, true),
              .x = j_dim(hj, "x", 0.0f),
              .y = j_dim(hj, "y", 0.0f),
              .w = j_dim(hj, "w", 0.0f),
              .h = j_dim(hj, "h", 0.0f),
            };
            nd->hits[nd->nhits++] = hs;
          }
        }
      }
    }
  }
  return nd;
}

static ui_panel_mgr *ensure_mgr(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (!ui->panel_mgr) {
    ui->panel_mgr = ccord_calloc(1, sizeof *ui->panel_mgr);
  }
  return ui->panel_mgr;
}

void ui_panels_destroy(ccord_ui *ui)
{
  if (!ui || !ui->panel_mgr) return;
  ui_panel_mgr *m = ui->panel_mgr;
  for (size_t i = 0; i < m->ncompiled; i++) {
    ccord_arena_free(&m->compiled[i].arena);
  }
  ccord_free(m->compiled);
  ccord_free(m);
  ui->panel_mgr = nullptr;
}

static compiled_panel *sync_compiled(ui_panel_mgr *m, const ccord_panel_contribution *pc)
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
    static const ccord_json_limits k_lim = {PANEL_MAX_DEPTH + 4, 8192, 256u * 1024u};
    ccord_error err = {0};
    const ccord_json *j = ccord_json_parse(&cp->arena, pc->root_json, strlen(pc->root_json), &k_lim, &err);
    if (j) cp->root = compile_node(cp, j, 0);
  }
  return cp;
}

static void prune_stale_compiled(ui_panel_mgr *m, const ccord_contrib *c)
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

// ---------------------------------------------------------------- layout integration

float ui_panels_slot_height(const ccord_ui *ui, ccord_panel_slot slot)
{
  if (!ui || !ui->d.contrib || ui->discovery_open || slot >= CCORD_PANEL__COUNT) return 0.0f;
  size_t n = ccord_contrib_panel_count(ui->d.contrib);
  float total = 0.0f;
  for (size_t i = 0; i < n; i++) {
    const ccord_panel_contribution *p = ccord_contrib_panel_at(ui->d.contrib, i);
    if (p && p->slot == slot) {
      total += p_clampf(p->height, 12.0f, 240.0f);
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
}

// ---------------------------------------------------------------- intrinsic measure & rendering

static float btn_default_h(pbtn_sz sz)
{
  switch (sz) {
    case PBTN_XS: return 18.0f;
    case PBTN_SM: return 24.0f;
    case PBTN_MD: return UIS_H_BTN_SM;
  }
  return 24.0f;
}

static bool child_visible(const pnode *c, bool parent_hovered)
{
  if (c->show_on_hover && !parent_hovered) return false;
  if (c->hide_on_hover && parent_hovered) return false;
  return true;
}

static ccord_vec2 measure_node(ccord_ui *ui, ccord_renderer *r, const pnode *nd, bool parent_is_col, bool parent_hovered)
{
  if (!nd) return (ccord_vec2){0.0f, 0.0f};
  float w = nd->w, h = nd->h;
  switch (nd->kind) {
    case PN_SPACER:
      break;
    case PN_DIVIDER: {
      bool vert = nd->orient_explicit ? nd->is_vertical : !parent_is_col;
      if (vert) {
        if (w <= 0.0f) w = nd->thickness;
      } else {
        if (h <= 0.0f) h = nd->thickness;
      }
      break;
    }
    case PN_ICON:
      if (w <= 0.0f) w = nd->icon_size;
      if (h <= 0.0f) h = nd->icon_size;
      break;
    case PN_TEXT: {
      float fs = resolve_font_size(ui, nd->font_size);
      ccord_text_style st = {.font = nd->bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, .size = fs, .color = ui->theme->c.text_normal};
      ccord_vec2 m = ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f);
      if (w <= 0.0f) w = m.x;
      if (h <= 0.0f) h = m.y;
      break;
    }
    case PN_BUTTON: {
      float bh = h > 0.0f ? h : btn_default_h(nd->btn_sz);
      h = bh;
      if (w <= 0.0f) {
        bool has_lbl = nd->text && nd->text[0];
        bool has_ic = nd->icon != PICON_NONE;
        if (!has_lbl && has_ic) {
          w = bh;
        } else {
          float fs = ui->theme->m.font_size_small - (nd->btn_sz == PBTN_XS ? 2.0f : 1.0f);
          ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = fs, .color = ui->theme->c.text_header};
          ccord_vec2 m = has_lbl ? ccord_text_measure(r, nd->text, strlen(nd->text), &st, 0.0f) : (ccord_vec2){0.0f, 0.0f};
          float pad_x = nd->btn_sz == PBTN_XS ? 6.0f : 10.0f;
          w = m.x + pad_x * 2.0f + (has_ic ? nd->icon_size + 5.0f : 0.0f);
        }
      }
      break;
    }
    case PN_CANVAS:
      break;
    case PN_BOX: {
      float sum_main = 0.0f, max_cross = 0.0f;
      size_t vis = 0;
      for (size_t i = 0; i < nd->nchildren; i++) {
        const pnode *c = nd->children[i];
        if (!child_visible(c, parent_hovered)) continue;
        ccord_vec2 cm = measure_node(ui, r, c, nd->is_col, parent_hovered);
        float c_main = nd->is_col ? cm.y : cm.x;
        float c_cross = nd->is_col ? cm.x : cm.y;
        sum_main += c_main;
        if (c_cross > max_cross) max_cross = c_cross;
        vis++;
      }
      if (vis > 1u) sum_main += nd->gap * (float)(vis - 1u);
      float auto_w = (nd->is_col ? max_cross : sum_main) + nd->pad_l + nd->pad_r;
      float auto_h = (nd->is_col ? sum_main : max_cross) + nd->pad_t + nd->pad_b;
      if (w <= 0.0f) w = auto_w;
      if (h <= 0.0f) h = auto_h;
      break;
    }
  }
  if (nd->min_w > 0.0f && w < nd->min_w) w = nd->min_w;
  if (nd->max_w > 0.0f && w > nd->max_w) w = nd->max_w;
  return (ccord_vec2){w, h};
}

static bool panels_can_hover(const ccord_ui *ui)
{
  return ui->mouse_in && !ui->menu.open && !ui->settings_open && !ui->go_live_open &&
         !ui->quick_switcher_open && !ui->shortcuts_open && !ui_server_settings_is_open(ui) &&
         !ui_channel_settings_is_open(ui);
}

static void record_hit(ui_panel_mgr *m, const ccord_panel_contribution *pc, const char *node_id, const char *tooltip,
                       ccord_cursor_kind cursor, ccord_rect rc)
{
  if (!node_id || !node_id[0] || rc.w <= 0.0f || rc.h <= 0.0f || m->nhits >= PANEL_MAX_HITS) return;
  panel_hit *h = &m->hits[m->nhits++];
  snprintf(h->owner, sizeof h->owner, "%s", pc->owner);
  snprintf(h->panel_id, sizeof h->panel_id, "%s", pc->panel_id);
  snprintf(h->node_id, sizeof h->node_id, "%s", node_id);
  snprintf(h->tooltip, sizeof h->tooltip, "%s", tooltip ? tooltip : "");
  h->cursor = cursor;
  h->rect = rc;
}

static void draw_box_chrome(ccord_renderer *r, const ccord_theme_colors *tc, const pnode *nd, ccord_rect rc,
                            bool hovered, ccord_color *out_surface_bg)
{
  ccord_color fill = {0}, bord = {0};
  bool has_fill = (hovered && resolve_pcolor(tc, nd->hover_bg, &fill)) || resolve_pcolor(tc, nd->bg, &fill);
  bool has_bord = nd->border_w > 0.0f &&
                  ((hovered && resolve_pcolor(tc, nd->hover_border_color, &bord)) ||
                   resolve_pcolor(tc, nd->border_color, &bord));
  if (has_fill) *out_surface_bg = fill;

  float bw = nd->border_w;
  float rad = nd->radius;

  if (nd->border_sides == PBORD_NO_BOTTOM) {
    if (has_bord) {
      ccord_draw_rrect(r, (ccord_rect){rc.x - bw, rc.y - bw, rc.w + bw * 2.0f, rc.h + bw}, rad + bw, bord);
    }
    if (has_fill) {
      if (rad > 0.0f) {
        ccord_draw_rrect(r, rc, rad, fill);
        float sq = p_minf(rad, rc.h);
        ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - sq, rc.w, sq}, fill);
      } else {
        ccord_draw_rect(r, rc, fill);
      }
    }
    return;
  }

  if (nd->border_sides == PBORD_ALL && has_bord && rad > 0.0f) {
    ccord_draw_rrect(r, (ccord_rect){rc.x - bw, rc.y - bw, rc.w + bw * 2.0f, rc.h + bw * 2.0f}, rad + bw, bord);
  }
  if (has_fill) {
    if (rad > 0.0f) ccord_draw_rrect(r, rc, rad, fill);
    else ccord_draw_rect(r, rc, fill);
  }
  if (has_bord) {
    switch (nd->border_sides) {
      case PBORD_ALL:
        if (rad <= 0.0f) {
          ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, bw}, bord);
          ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - bw, rc.w, bw}, bord);
          ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, bw, rc.h}, bord);
          ccord_draw_rect(r, (ccord_rect){rc.x + rc.w - bw, rc.y, bw, rc.h}, bord);
        }
        break;
      case PBORD_BOTTOM:
        ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - bw, rc.w, bw}, bord);
        break;
      case PBORD_TOP:
        ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, bw}, bord);
        break;
      case PBORD_LEFT:
        ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, bw, rc.h}, bord);
        break;
      case PBORD_RIGHT:
        ccord_draw_rect(r, (ccord_rect){rc.x + rc.w - bw, rc.y, bw, rc.h}, bord);
        break;
      case PBORD_NO_BOTTOM:
        break;
    }
  }
}

static void draw_node(ccord_ui *ui, ccord_renderer *r, ui_panel_mgr *m, const ccord_panel_contribution *pc,
                      const pnode *nd, ccord_rect rc, bool parent_is_col, bool ancestor_hovered, ccord_color surface_bg)
{
  if (!nd || rc.w <= 0.5f || rc.h <= 0.5f) return;
  const ccord_theme_colors *tc = &ui->theme->c;
  bool can_hov = panels_can_hover(ui);
  bool self_hovered = can_hov && ui_in_rect(ui->mx, ui->my, rc);
  bool eff_hovered = (nd->id != nullptr) ? self_hovered : ancestor_hovered;

  if (nd->id && !nd->disabled) {
    record_hit(m, pc, nd->id, nd->tooltip, nd->cursor, rc);
  }

  switch (nd->kind) {
    case PN_SPACER:
      return;

    case PN_DIVIDER: {
      ccord_color col = tc->divider;
      (void)resolve_pcolor(tc, nd->fg, &col);
      bool vert = nd->orient_explicit ? nd->is_vertical : !parent_is_col;
      if (vert) {
        float y0 = rc.y + nd->inset;
        float dh = p_maxf(rc.h - nd->inset * 2.0f, 0.0f);
        if (dh > 0.0f) ccord_draw_rect(r, (ccord_rect){rc.x + (rc.w - nd->thickness) * 0.5f, y0, nd->thickness, dh}, col);
      } else {
        float x0 = rc.x + nd->inset;
        float dw = p_maxf(rc.w - nd->inset * 2.0f, 0.0f);
        if (dw > 0.0f) ccord_draw_rect(r, (ccord_rect){x0, rc.y + (rc.h - nd->thickness) * 0.5f, dw, nd->thickness}, col);
      }
      return;
    }

    case PN_ICON: {
      ccord_color col = tc->text_muted;
      if (!(eff_hovered && resolve_pcolor(tc, nd->hover_fg, &col))) {
        (void)resolve_pcolor(tc, nd->fg, &col);
      }
      draw_picon(ui, r, nd->icon, rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f, nd->icon_size, col, surface_bg);
      return;
    }

    case PN_TEXT: {
      ccord_color col = tc->text_normal;
      if (!(eff_hovered && resolve_pcolor(tc, nd->hover_fg, &col))) {
        (void)resolve_pcolor(tc, nd->fg, &col);
      }
      float fs = resolve_font_size(ui, nd->font_size);
      ccord_text_style st = {.font = nd->bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, .size = fs, .color = col};
      size_t len = strlen(nd->text);
      ccord_vec2 tm = ccord_text_measure(r, nd->text, len, &st, 0.0f);
      float ty = rc.y + (rc.h - tm.y) * 0.5f;
      if (nd->ellipsis && tm.x > rc.w) {
        (void)ccord_text_draw_ellipsis(r, nd->text, len, rc.x, ty, rc.w, &st);
      } else {
        float tx = rc.x;
        if (nd->align == PALIGN_CENTER) tx = rc.x + (rc.w - tm.x) * 0.5f;
        else if (nd->align == PALIGN_END) tx = rc.x + rc.w - tm.x;
        ccord_text_draw(r, nd->text, len, tx, ty, &st, 0.0f);
      }
      return;
    }

    case PN_BUTTON: {
      bool hov = self_hovered && !nd->disabled;
      ccord_color bg = {0}, fg = tc->text_normal;
      bool draw_bg = false;
      switch (nd->btn_variant) {
        case PBTN_PRIMARY:
          bg = hov ? tc->brand_hover : tc->brand;
          fg = tc->text_on_brand;
          draw_bg = true;
          break;
        case PBTN_DANGER:
          bg = tc->danger;
          fg = tc->text_on_brand;
          draw_bg = true;
          break;
        case PBTN_SECONDARY:
          bg = (hov || nd->selected) ? tc->bg_item_hover : tc->bg_secondary;
          fg = (hov || nd->selected) ? tc->text_header : tc->text_normal;
          draw_bg = true;
          break;
        case PBTN_GHOST:
          if (hov || nd->selected) {
            bg = nd->selected ? tc->bg_item_selected : tc->bg_item_hover;
            draw_bg = true;
          }
          fg = (hov || nd->selected) ? tc->text_header : tc->text_muted;
          break;
      }
      if (nd->disabled) ccord_push_alpha(r, 0.45f);
      if (draw_bg) ccord_draw_rrect(r, rc, 4.0f, bg);
      bool has_lbl = nd->text && nd->text[0];
      bool has_ic = nd->icon != PICON_NONE;
      float cy = rc.y + rc.h * 0.5f;
      if (has_ic && !has_lbl) {
        draw_picon(ui, r, nd->icon, rc.x + rc.w * 0.5f, cy, nd->icon_size, fg, draw_bg ? bg : surface_bg);
      } else if (has_lbl) {
        float fs = ui->theme->m.font_size_small - (nd->btn_sz == PBTN_XS ? 2.0f : 1.0f);
        ccord_text_style st = {.font = CCORD_FONT_BOLD, .size = fs, .color = fg};
        size_t len = strlen(nd->text);
        ccord_vec2 tm = ccord_text_measure(r, nd->text, len, &st, 0.0f);
        float content_w = tm.x + (has_ic ? nd->icon_size + 5.0f : 0.0f);
        float start_x = rc.x + p_maxf((rc.w - content_w) * 0.5f, 4.0f);
        if (has_ic) {
          draw_picon(ui, r, nd->icon, start_x + nd->icon_size * 0.5f, cy, nd->icon_size, fg, draw_bg ? bg : surface_bg);
          start_x += nd->icon_size + 5.0f;
        }
        float avail_w = p_maxf(rc.x + rc.w - start_x - 4.0f, 0.0f);
        (void)ccord_text_draw_ellipsis(r, nd->text, len, start_x, cy - tm.y * 0.5f, avail_w, &st);
      }
      if (nd->disabled) ccord_pop_alpha(r);
      return;
    }

    case PN_CANVAS: {
      if (nd->clip) ccord_push_clip(r, rc);
      for (size_t i = 0; i < nd->ncmds; i++) {
        const pcmd *c = &nd->cmds[i];
        ccord_color col = tc->text_normal;
        (void)resolve_pcolor(tc, c->color, &col);
        float cx = rc.x + eval_dim(c->x, rc.w);
        float cy = rc.y + eval_dim(c->y, rc.h);
        switch (c->kind) {
          case PCMD_RECT:
            ccord_draw_rect(r, (ccord_rect){cx, cy, eval_dim(c->w, rc.w), eval_dim(c->h, rc.h)}, col);
            break;
          case PCMD_RRECT:
            ccord_draw_rrect(r, (ccord_rect){cx, cy, eval_dim(c->w, rc.w), eval_dim(c->h, rc.h)}, c->r_or_thick, col);
            break;
          case PCMD_CIRCLE:
            ccord_draw_circle(r, cx, cy, c->r_or_thick, col);
            break;
          case PCMD_LINE:
            ccord_draw_line(r, (ccord_vec2){cx, cy},
                            (ccord_vec2){rc.x + eval_dim(c->w, rc.w), rc.y + eval_dim(c->h, rc.h)}, c->r_or_thick, col);
            break;
          case PCMD_TRIANGLE:
            ccord_draw_triangle(r, (ccord_vec2){cx, cy}, (ccord_vec2){rc.x + c->x2, rc.y + c->y2},
                                (ccord_vec2){rc.x + c->x3, rc.y + c->y3}, col);
            break;
          case PCMD_TEXT: {
            float fs = resolve_font_size(ui, c->font_size);
            ccord_text_style st = {.font = c->bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, .size = fs, .color = col};
            float mw = eval_dim(c->w, rc.w);
            if (c->ellipsis && mw > 0.0f) {
              (void)ccord_text_draw_ellipsis(r, c->text, strlen(c->text), cx, cy, mw, &st);
            } else {
              ccord_text_draw(r, c->text, strlen(c->text), cx, cy, &st, 0.0f);
            }
            break;
          }
          case PCMD_ICON:
            draw_picon(ui, r, c->icon, cx, cy, c->r_or_thick, col, surface_bg);
            break;
        }
      }
      if (nd->clip) ccord_pop_clip(r);
      for (size_t i = 0; i < nd->nhits; i++) {
        const phit_spec *hs = &nd->hits[i];
        ccord_rect hr = {
          rc.x + eval_dim(hs->x, rc.w),
          rc.y + eval_dim(hs->y, rc.h),
          eval_dim(hs->w, rc.w),
          eval_dim(hs->h, rc.h),
        };
        record_hit(m, pc, hs->id, hs->tooltip, hs->cursor, hr);
      }
      return;
    }

    case PN_BOX: {
      ccord_color box_surface = surface_bg;
      draw_box_chrome(r, tc, nd, rc, eff_hovered, &box_surface);
      if (nd->nchildren == 0) return;

      if (nd->clip) ccord_push_clip(r, rc);
      ccord_rect inner = {
        rc.x + nd->pad_l,
        rc.y + nd->pad_t,
        p_maxf(rc.w - nd->pad_l - nd->pad_r, 0.0f),
        p_maxf(rc.h - nd->pad_t - nd->pad_b, 0.0f),
      };
      float avail_main = nd->is_col ? inner.h : inner.w;
      float avail_cross = nd->is_col ? inner.w : inner.h;

      // Collect visible children (up to 64 per box) and compute flex sizes.
      const pnode *vis[64];
      ccord_vec2 intrinsic[64];
      float main_sz[64];
      bool frozen[64];
      size_t nvis = 0;
      for (size_t i = 0; i < nd->nchildren && nvis < 64u; i++) {
        const pnode *c = nd->children[i];
        if (!child_visible(c, eff_hovered)) continue;
        vis[nvis] = c;
        intrinsic[nvis] = measure_node(ui, r, c, nd->is_col, eff_hovered);
        main_sz[nvis] = nd->is_col ? intrinsic[nvis].y : intrinsic[nvis].x;
        frozen[nvis] = (c->flex <= 0.0f);
        nvis++;
      }
      if (nvis == 0) {
        if (nd->clip) ccord_pop_clip(r);
        return;
      }

      float total_gaps = (nvis > 1u) ? nd->gap * (float)(nvis - 1u) : 0.0f;
      // 2-pass min/max clamping flex solver.
      for (int pass = 0; pass < 3; pass++) {
        float fixed_sum = total_gaps;
        float flex_sum = 0.0f;
        for (size_t i = 0; i < nvis; i++) {
          if (frozen[i]) fixed_sum += main_sz[i];
          else flex_sum += vis[i]->flex;
        }
        if (flex_sum <= 0.0f) break;
        float rem = p_maxf(avail_main - fixed_sum, 0.0f);
        bool clamped_any = false;
        for (size_t i = 0; i < nvis; i++) {
          if (frozen[i]) continue;
          float target = rem * (vis[i]->flex / flex_sum);
          float min_m = !nd->is_col ? vis[i]->min_w : 0.0f;
          float max_m = !nd->is_col ? vis[i]->max_w : 0.0f;
          if (min_m > 0.0f && target < min_m) {
            main_sz[i] = min_m;
            frozen[i] = true;
            clamped_any = true;
          } else if (max_m > 0.0f && target > max_m) {
            main_sz[i] = max_m;
            frozen[i] = true;
            clamped_any = true;
          } else {
            main_sz[i] = target;
          }
        }
        if (!clamped_any) break;
      }

      float used_main = total_gaps;
      for (size_t i = 0; i < nvis; i++) used_main += main_sz[i];
      float extra = p_maxf(avail_main - used_main, 0.0f);
      float cursor_main = 0.0f;
      float step_gap = nd->gap;
      if (nd->justify == PJUST_CENTER) cursor_main = extra * 0.5f;
      else if (nd->justify == PJUST_END) cursor_main = extra;
      else if (nd->justify == PJUST_BETWEEN && nvis > 1u) step_gap += extra / (float)(nvis - 1u);

      for (size_t i = 0; i < nvis; i++) {
        const pnode *c = vis[i];
        float m_len = main_sz[i];
        float c_intr = nd->is_col ? intrinsic[i].x : intrinsic[i].y;
        float c_len = (nd->align == PALIGN_STRETCH || (c->kind == PN_DIVIDER && c_intr <= 0.0f)) ? avail_cross
                    : (c_intr > 0.0f ? p_minf(c_intr, avail_cross) : avail_cross);
        float c_off = 0.0f;
        if (nd->align == PALIGN_CENTER) c_off = (avail_cross - c_len) * 0.5f;
        else if (nd->align == PALIGN_END) c_off = avail_cross - c_len;

        ccord_rect crc = nd->is_col
                           ? (ccord_rect){inner.x + c_off, inner.y + cursor_main, c_len, m_len}
                           : (ccord_rect){inner.x + cursor_main, inner.y + c_off, m_len, c_len};
        // Stop drawing overflow siblings once completely past the inner main axis when clipped.
        if (cursor_main < avail_main + 1.0f) {
          draw_node(ui, r, m, pc, c, crc, nd->is_col, eff_hovered, box_surface);
        }
        cursor_main += m_len + step_gap;
      }

      if (nd->clip) ccord_pop_clip(r);
      return;
    }
  }
}

static uint64_t hash_str(const char *s)
{
  uint64_t h = 1469598103934665603ULL;
  for (; s && *s; s++) {
    h ^= (unsigned char)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

void ui_panels_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r) return;
  ui_panel_mgr *m = ensure_mgr(ui);
  if (!m) return;
  m->nhits = 0;
  m->nboxes = 0;
  prune_stale_compiled(m, ui->d.contrib);
  if (!ui->d.contrib || ui->discovery_open) return;

  size_t np = ccord_contrib_panel_count(ui->d.contrib);
  if (np == 0) return;

  float col_x = ui->layout.chat_header.x;
  float col_w = ui->layout.chat_header.w;
  if (col_w <= 0.0f) return;

  float top_y = 0.0f;
  float sub_y = ui->layout.chat_header.y + ui->layout.chat_header.h;
  float bot_total = ui_panels_slot_height(ui, CCORD_PANEL_BOTTOM);
  float bot_y = ui->win_h - bot_total;

  for (size_t i = 0; i < np; i++) {
    const ccord_panel_contribution *pc = ccord_contrib_panel_at(ui->d.contrib, i);
    if (!pc) continue;
    float ph = p_clampf(pc->height, 12.0f, 240.0f);
    float py = 0.0f;
    if (pc->slot == CCORD_PANEL_TOP) {
      py = top_y;
      top_y += ph;
    } else if (pc->slot == CCORD_PANEL_SUBHEADER) {
      py = sub_y;
      sub_y += ph;
    } else if (pc->slot == CCORD_PANEL_BOTTOM) {
      py = bot_y;
      bot_y += ph;
    } else {
      continue;
    }

    ccord_rect prc = {col_x, py, col_w, ph};
    if (m->nboxes < sizeof m->boxes / sizeof m->boxes[0]) {
      m->boxes[m->nboxes++] = (panel_box){.rect = prc};
    }

    // Default panel backdrop so an unstyled root row blends cleanly into the client chrome.
    ccord_draw_rect(r, prc, ui->theme->c.bg_tertiary);
    ccord_push_clip(r, prc);
    compiled_panel *cp = sync_compiled(m, pc);
    if (cp && cp->root) {
      draw_node(ui, r, m, pc, cp->root, prc, false, false, ui->theme->c.bg_tertiary);
    }
    ccord_pop_clip(r);
  }

  // Draw tooltip for the topmost hovered interactive node, if any.
  if (panels_can_hover(ui)) {
    for (size_t k = m->nhits; k > 0; k--) {
      const panel_hit *h = &m->hits[k - 1u];
      if (ui_in_rect(ui->mx, ui->my, h->rect)) {
        if (h->tooltip[0] != '\0') {
          uis_tip_side side = (h->rect.y < 60.0f) ? UIS_TIP_BELOW : UIS_TIP_ABOVE;
          (void)uis_tooltip(ui, r, h->rect, h->tooltip, side, hash_str(h->owner) ^ hash_str(h->node_id));
        }
        break;
      }
    }
  }
}

static const panel_hit *find_top_hit(const ui_panel_mgr *m, float x, float y)
{
  if (!m) return nullptr;
  for (size_t k = m->nhits; k > 0; k--) {
    const panel_hit *h = &m->hits[k - 1u];
    if (ui_in_rect(x, y, h->rect)) return h;
  }
  return nullptr;
}

static bool point_in_any_panel(const ui_panel_mgr *m, float x, float y)
{
  if (!m) return false;
  for (size_t i = 0; i < m->nboxes; i++) {
    if (ui_in_rect(x, y, m->boxes[i].rect)) return true;
  }
  return false;
}

bool ui_panels_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->panel_mgr || !ui->d.contrib || ui->discovery_open) return false;
  ui_panel_mgr *m = ui->panel_mgr;
  if (m->nboxes == 0) return false;

  if (ev->type == CCORD_IN_WHEEL) {
    if (!point_in_any_panel(m, ev->x, ev->y)) return false;
    const panel_hit *h = find_top_hit(m, ev->x, ev->y);
    if (h) {
      ccord_panel_event pev = {
        .kind = CCORD_PANEL_EV_WHEEL,
        .node_id = h->node_id,
        .button = 0,
        .mods = ev->mods,
        .x = ev->x - h->rect.x,
        .y = ev->y - h->rect.y,
        .wheel = ev->wheel,
      };
      ccord_error err = {0};
      (void)ccord_contrib_dispatch_panel_event(ui->d.contrib, h->owner, h->panel_id, &pev, &err);
    }
    return true;
  }

  if (ev->type == CCORD_IN_MOUSE_DOWN) {
    if (!point_in_any_panel(m, ev->x, ev->y)) return false;
    const panel_hit *h = find_top_hit(m, ev->x, ev->y);
    if (h && (ev->button >= 1 && ev->button <= 3)) {
      ccord_panel_event_kind kind = CCORD_PANEL_EV_CLICK;
      if (ev->button == 1 && ev->clicks >= 2) kind = CCORD_PANEL_EV_DOUBLE_CLICK;
      else if (ev->button == 2) kind = CCORD_PANEL_EV_MIDDLE_CLICK;
      else if (ev->button == 3) kind = CCORD_PANEL_EV_RIGHT_CLICK;
      ccord_panel_event pev = {
        .kind = kind,
        .node_id = h->node_id,
        .button = ev->button,
        .mods = ev->mods,
        .x = ev->x - h->rect.x,
        .y = ev->y - h->rect.y,
        .wheel = 0.0f,
      };
      ccord_error err = {0};
      (void)ccord_contrib_dispatch_panel_event(ui->d.contrib, h->owner, h->panel_id, &pev, &err);
      ui->dirty = true;
    }
    return true;
  }

  return false;
}

bool ui_panels_cursor(const ccord_ui *ui, float x, float y, ccord_cursor_kind *out_cursor)
{
  if (!ui || !ui->panel_mgr || ui->discovery_open) return false;
  if (!point_in_any_panel(ui->panel_mgr, x, y)) return false;
  const panel_hit *h = find_top_hit(ui->panel_mgr, x, y);
  if (out_cursor) *out_cursor = h ? h->cursor : CCORD_CURSOR_DEFAULT;
  return true;
}

bool ui_panels_find_hit_rect(const ccord_ui *ui, const char *owner, const char *panel_id, const char *node_id,
                             ccord_rect *out)
{
  if (!ui || !ui->panel_mgr || !node_id) return false;
  const ui_panel_mgr *m = ui->panel_mgr;
  for (size_t k = m->nhits; k > 0; k--) {
    const panel_hit *h = &m->hits[k - 1u];
    if (owner && strcmp(h->owner, owner) != 0) continue;
    if (panel_id && strcmp(h->panel_id, panel_id) != 0) continue;
    if (strcmp(h->node_id, node_id) == 0) {
      if (out) *out = h->rect;
      return true;
    }
  }
  return false;
}
