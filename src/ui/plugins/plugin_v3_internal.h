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
#define V3_MAX_FORM_FIELDS   256u
#define V3_MAX_FIELD_OPTS    256u
#define V3_MAX_VIS_IN          8u

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
  V3F_BUTTON,
  V3F_PATH,
} v3_field_kind;

typedef struct v3_opt {
  char label[64];
  char value[64];
} v3_opt;

typedef struct v3_vis_cond {
  bool active;
  char key[65];
  bool has_equals;
  char equals_val[96];
  bool has_not_equals;
  char not_equals_val[96];
  bool has_truthy;
  bool truthy_want;
  char in_vals[V3_MAX_VIS_IN][64];
  size_t nin_vals;
} v3_vis_cond;

typedef struct v3_field {
  v3_field_kind kind;
  char key[65];
  char label[96];
  char desc[160];
  char placeholder[96];
  char button_label[64];
  uint8_t btn_variant;   // 0=secondary, 1=primary, 2=danger
  bool path_is_dir;      // for V3F_PATH: false="file", true="dir"
  v3_vis_cond vis;
  bool bool_val;
  double num_val;
  double num_min;
  double num_max;
  double num_step;
  v3_opt *opts;
  size_t nopts;
  size_t sel_idx;
  ccord_text_edit edit;
  bool edit_inited;
  ccord_rect hit_rect;
  ccord_rect browse_rect;
  ccord_rect clear_rect;
} v3_field;

typedef struct v3_dialog_state {
  uint64_t active_req_id;
  ccord_dialog_kind kind;
  v3_field *fields;
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
  v3_field *fields;
  size_t nfields;
  int focused_idx;
  int drag_slider_idx;
  float scroll_y;
  float content_h;
  ccord_rect box_rect;
  ccord_rect body_rect;
  ccord_rect done_rect;
} v3_settings_state;

typedef struct v3_hitl_state {
  uint64_t active_req_id;
  ccord_text_edit pw_edit;
  bool pw_inited;
  bool pw_focused;
  ccord_text_edit mfa_edit;
  bool mfa_inited;
  bool mfa_focused;
  bool pending;
  bool mfa_required;
  uint32_t mfa_methods;
  int mfa_method_idx;
  char error[200];
  ccord_rect box_rect;
  ccord_rect pw_rect;
  ccord_rect mfa_rect;
  ccord_rect mfa_chip_rects[4];
  ccord_rect approve_rect;
  ccord_rect deny_rect;
  uint64_t cb_gen;
} v3_hitl_state;

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
  v3_hitl_state hitl;

  // Pending async OS file/folder picker target for V3F_PATH fields
  int pending_path_field;
  bool pending_path_in_settings;
};


// Cross-TU internal declarations
void pv3_v3_clear_fields(v3_field **fields, size_t *nfields);
ui_plugin_v3 * pv3_v3_ensure(ccord_ui *ui);
bool pv3_v3_parse_hex_color(const char *s, ccord_color *out);
