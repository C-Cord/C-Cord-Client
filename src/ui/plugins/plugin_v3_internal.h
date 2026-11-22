// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "ui_internal.h"
#include "ui_style.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#define V3_MAX_DECOR_HITS    512u
#define V3_MAX_ACTBTN_HITS    64u
#define V3_MAX_LOCAL_HITS     64u
#define V3_MAX_RAIL_HITS      16u
#define V3_MAX_FORM_FIELDS    32u
#define V3_MAX_FIELD_OPTS     24u

typedef struct v3_decor_hit {
  char owner[65];
  char decor_id[65];
  char node_id[65];
  ccord_decor_target target;
  ccord_id target_id;
  ccord_rect rect;
} v3_decor_hit;

typedef struct v3_actbtn_hit {
  char owner[65];
  char btn_id[65];
  char tooltip[128];
  ccord_action_btn_slot slot;
  ccord_menu_context ctx;
  ccord_rect rect;
} v3_actbtn_hit;

typedef struct v3_local_hit {
  uint64_t local_id;
  ccord_rect dismiss_rect;
} v3_local_hit;

typedef struct v3_rail_hit {
  char owner[65];
  char panel_id[40];
  ccord_rect rect;
} v3_rail_hit;

typedef enum v3_field_kind : uint8_t {
  V3F_SECTION = 0,
  V3F_TEXT,
  V3F_SECRET,
  V3F_TOGGLE,
  V3F_SLIDER,
  V3F_SELECT,
  V3F_COLOR,
} v3_field_kind;

typedef struct v3_opt {
  char label[64];
  char value[64];
} v3_opt;

typedef struct v3_field {
  v3_field_kind kind;
  char key[65];
  char label[96];
  char desc[160];
  char placeholder[96];
  bool bool_val;
  double num_val;
  double num_min;
  double num_max;
  double num_step;
  v3_opt opts[V3_MAX_FIELD_OPTS];
  size_t nopts;
  size_t sel_idx;
  ccord_text_edit edit;
  bool edit_inited;
  ccord_rect hit_rect;
} v3_field;

typedef struct v3_dialog_state {
  uint64_t active_req_id;
  ccord_dialog_kind kind;
  v3_field fields[V3_MAX_FORM_FIELDS];
  size_t nfields;
  int focused_idx;
  int drag_slider_idx;
  float scroll_y;
  float content_h;
  ccord_rect box_rect;
  ccord_rect body_rect;
  ccord_rect confirm_rect;
  ccord_rect cancel_rect;
} v3_dialog_state;

typedef struct v3_settings_state {
  bool open;
  char owner[65];
  char plugin_name[96];
  uint64_t loaded_rev;
  v3_field fields[V3_MAX_FORM_FIELDS];
  size_t nfields;
  int focused_idx;
  int drag_slider_idx;
  float scroll_y;
  float content_h;
  ccord_rect box_rect;
  ccord_rect body_rect;
  ccord_rect done_rect;
} v3_settings_state;

struct ui_plugin_v3 {
  v3_decor_hit decor_hits[V3_MAX_DECOR_HITS];
  size_t ndecor_hits;

  v3_actbtn_hit actbtn_hits[V3_MAX_ACTBTN_HITS];
  size_t nactbtn_hits;

  v3_local_hit local_hits[V3_MAX_LOCAL_HITS];
  size_t nlocal_hits;

  v3_rail_hit rail_hits[V3_MAX_RAIL_HITS];
  size_t nrail_hits;

  v3_dialog_state dlg;
  v3_settings_state set;
};


// Cross-TU internal declarations
void pv3_v3_clear_fields(v3_field *fields, size_t *nfields);
ui_plugin_v3 * pv3_v3_ensure(ccord_ui *ui);
bool pv3_v3_parse_hex_color(const char *s, ccord_color *out);
