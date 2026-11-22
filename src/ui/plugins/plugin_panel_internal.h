// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_MAX_DEPTH  24
#define PANEL_MAX_NODES  2048u
#define PANEL_MAX_HITS   1024u
#define PANEL_MAX_CMDS   1024u
#define PANEL_MAX_BOXES  64u
#define PANEL_MAX_INPUTS 32u
#define PANEL_MAX_SCROLL 32u
#define PANEL_MAX_OPTS   32u
#define PANEL_MAX_POLY   64u

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
  PICON_HASH, PICON_SPEAKER, PICON_GEAR,
  PICON_CHEVRON_UP, PICON_CHEVRON_DOWN, PICON_CHEVRON_LEFT, PICON_CHEVRON_RIGHT,
  PICON_CLOSE, PICON_MEMBERS, PICON_MIC, PICON_MIC_MUTED,
  PICON_HEADPHONES, PICON_HEADPHONES_DEAF, PICON_CAMERA, PICON_SCREEN,
  PICON_DISCONNECT, PICON_SEARCH, PICON_PIN, PICON_REPLY, PICON_EDIT,
  PICON_ATTACHMENT, PICON_FILE, PICON_FORUM, PICON_USER, PICON_INBOX,
  PICON_FOLDER, PICON_PLUS, PICON_COMPASS, PICON_AT,
} picon;

typedef enum palign : uint8_t { PALIGN_START = 0, PALIGN_CENTER, PALIGN_END, PALIGN_STRETCH } palign;
typedef enum pjustify : uint8_t { PJUST_START = 0, PJUST_CENTER, PJUST_END, PJUST_BETWEEN } pjustify;
typedef enum pborder_sides : uint8_t {
  PBORD_ALL = 0, PBORD_BOTTOM, PBORD_TOP, PBORD_LEFT, PBORD_RIGHT, PBORD_NO_BOTTOM,
} pborder_sides;

typedef enum pbtn_variant : uint8_t { PBTN_GHOST = 0, PBTN_SECONDARY, PBTN_PRIMARY, PBTN_DANGER } pbtn_variant;
typedef enum pbtn_sz : uint8_t { PBTN_XS = 0, PBTN_SM, PBTN_MD } pbtn_sz;

typedef enum pcmd_kind : uint8_t {
  PCMD_RECT = 0, PCMD_RRECT, PCMD_CIRCLE, PCMD_LINE, PCMD_TRIANGLE, PCMD_TEXT, PCMD_ICON,
  // v2 canvas commands
  PCMD_IMAGE, PCMD_RRECT_STROKE, PCMD_CIRCLE_STROKE, PCMD_ARC, PCMD_POLYGON, PCMD_GRADIENT,
  PCMD_CLIP_PUSH, PCMD_CLIP_POP, PCMD_ALPHA_PUSH, PCMD_ALPHA_POP, PCMD_OFFSET_PUSH, PCMD_OFFSET_POP,
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
  float start_deg, end_deg, stroke_w;
  float font_size;
  bool bold, ellipsis, filled, vertical, cover;
  picon icon;
  pcolor color, color2;
  const char *text;      // text, texture key, or custom icon name
  ccord_vec2 *pts;
  size_t npts;
} pcmd;

typedef struct phit_spec {
  const char *id;
  const char *tooltip;
  ccord_cursor_kind cursor;
  pdim x, y, w, h;
  bool draggable, track_move;
} phit_spec;

typedef struct pkv_opt {
  const char *label;
  const char *value;
} pkv_opt;

typedef enum pnode_kind : uint8_t {
  PN_BOX = 0, PN_TEXT, PN_ICON, PN_BUTTON, PN_SPACER, PN_DIVIDER, PN_CANVAS,
  // v2 Layer-1 widgets
  PN_INPUT, PN_TOGGLE, PN_CHECKBOX, PN_RADIO, PN_SLIDER, PN_SELECT,
  PN_TABS, PN_CHIP, PN_PROGRESS, PN_SCROLL, PN_IMAGE, PN_AVATAR, PN_MARKDOWN,
} pnode_kind;

typedef struct pnode pnode;
struct pnode {
  pnode_kind kind;
  bool is_col, clip, wrap, pos_abs;
  bool show_on_hover, hide_on_hover;
  bool bold, ellipsis, selected, disabled, checked, masked, cover;
  bool orient_explicit, is_vertical;
  bool draggable, track_move;
  palign align;
  pjustify justify;
  pborder_sides border_sides;
  pbtn_variant btn_variant;
  pbtn_sz btn_sz;
  picon icon;
  const char *icon_name;
  ccord_cursor_kind cursor;
  float w, h, min_w, max_w, min_h, max_h, flex;
  float left_x, top_y;
  float opacity;
  float gap;
  float pad_l, pad_r, pad_t, pad_b;
  float radius, border_w;
  float font_size, icon_size, inset, thickness;
  float num_val, num_min, num_max, num_step;
  ccord_id avatar_uid;
  pcolor bg, hover_bg, border_color, hover_border_color, fg, hover_fg;
  const char *id, *text, *placeholder, *value_str, *tooltip, *texture_key;
  pnode **children;
  size_t nchildren;
  pcmd *cmds;
  size_t ncmds;
  phit_spec *hits;
  size_t nhits;
  pkv_opt *opts;
  size_t nopts;
};

typedef struct compiled_panel {
  char owner[65];
  char panel_id[40];
  uint64_t revision;
  ccord_arena arena;
  pnode *root;
  uint32_t node_count;
} compiled_panel;

typedef enum phit_widget_kind : uint8_t {
  PHIT_GENERIC = 0, PHIT_INPUT, PHIT_TOGGLE, PHIT_CHECKBOX, PHIT_RADIO, PHIT_SLIDER, PHIT_SELECT, PHIT_TAB, PHIT_SCROLL,
} phit_widget_kind;

typedef struct panel_hit {
  char owner[65];
  char panel_id[40];
  char node_id[65];
  char tooltip[128];
  char value_str[128];
  ccord_cursor_kind cursor;
  ccord_rect rect;
  phit_widget_kind wkind;
  bool checked;
  bool draggable, track_move;
  float num_min, num_max, num_step;
  size_t opt_idx, opt_count;
} panel_hit;

typedef struct panel_box {
  char owner[65];
  char panel_id[40];
  ccord_panel_slot slot;
  ccord_rect rect;
  bool is_overlay;
} panel_box;

typedef struct panel_input_state {
  uint64_t key;
  ccord_text_edit ed;
  char last_seed[512];
} panel_input_state;

typedef struct panel_scroll_state {
  uint64_t key;
  float scroll_y;
  float max_scroll;
} panel_scroll_state;

typedef struct decor_cache_entry {
  uint64_t hash;
  ccord_arena arena;
  pnode *root;
} decor_cache_entry;

struct ui_panel_mgr {
  compiled_panel *compiled;
  size_t ncompiled, capcompiled;
  panel_hit hits[PANEL_MAX_HITS];
  size_t nhits;
  panel_box boxes[PANEL_MAX_BOXES];
  size_t nboxes;
  // Retained interactive widget state
  panel_input_state inputs[PANEL_MAX_INPUTS];
  size_t ninputs;
  uint64_t focused_input_key;
  char focused_owner[65];
  char focused_panel[40];
  char focused_node[65];
  panel_scroll_state scrolls[PANEL_MAX_SCROLL];
  size_t nscrolls;
  // Hover & drag tracking
  char hover_owner[65];
  char hover_panel[40];
  char hover_node[65];
  bool dragging;
  panel_hit drag_hit;
  // Texture sync revision
  uint64_t synced_tex_rev;
  // Active full-view plugin tab
  char full_view_owner[65];
  char full_view_id[40];
  ccord_rect full_view_back_rect;
  // Compiled cache for inline decorator widget trees
  decor_cache_entry decor_cache[32];
  size_t decor_cursor;
};


// Cross-TU internal declarations
pnode * pp_compile_node_into(ccord_arena *arena, uint32_t *node_count, const ccord_json *j, int depth);
void pp_draw_picon(ccord_ui *ui, ccord_renderer *r, picon ic, const char *raw_name, float cx, float cy, float size, ccord_color col, ccord_color hole_bg);
ui_panel_mgr * pp_ensure_mgr(ccord_ui *ui);
float pp_eval_dim(pdim d, float span);
panel_input_state * pp_get_input_state(ui_panel_mgr *m, uint64_t key, const char *seed);
panel_scroll_state * pp_get_scroll_state(ui_panel_mgr *m, uint64_t key);
uint64_t pp_hash_str(const char *s);
float pp_p_clampf(float v, float lo, float hi);
float pp_p_maxf(float a, float b);
float pp_p_minf(float a, float b);
void pp_prune_stale_compiled(ui_panel_mgr *m, const ccord_contrib *c);
float pp_resolve_font_size(const ccord_ui *ui, float fs);
bool pp_resolve_pcolor(const ccord_theme_colors *tc, pcolor pc, ccord_color *out);
compiled_panel * pp_sync_compiled(ui_panel_mgr *m, const ccord_panel_contribution *pc);
uint64_t pp_widget_key(const char *owner, const char *panel_id, const char *node_id);
