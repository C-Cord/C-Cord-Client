// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Discord's report menu (GET /reporting/menu/{type}) as a node tree, and the body a submit sends
// (POST /reporting/{type}). Pure: no I/O, no session, no UI.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/model/model.h>

#define CCORD_REPORT_MAX_PATH 32

typedef enum ccord_report_type : uint8_t { CCORD_REPORT_MESSAGE, CCORD_REPORT_USER, CCORD_REPORT_TYPE_COUNT } ccord_report_type;
const char *ccord_report_type_name(ccord_report_type t);                 // "message" / "user"; nullptr out of range

typedef enum ccord_report_button : uint8_t { CCORD_REPORT_BTN_NONE, CCORD_REPORT_BTN_NEXT, CCORD_REPORT_BTN_SUBMIT,
                                             CCORD_REPORT_BTN_DONE, CCORD_REPORT_BTN_CANCEL } ccord_report_button;
typedef enum ccord_report_element_kind : uint8_t {
  CCORD_REPORT_EL_UNKNOWN, CCORD_REPORT_EL_CHECKBOX, CCORD_REPORT_EL_DROPDOWN, CCORD_REPORT_EL_FREE_TEXT, CCORD_REPORT_EL_TEXT,
  CCORD_REPORT_EL_EXTERNAL_LINK, CCORD_REPORT_EL_BREADCRUMBS, CCORD_REPORT_EL_SUCCESS, CCORD_REPORT_EL_FAIL,
  CCORD_REPORT_EL_BLOCK_USERS, CCORD_REPORT_EL_MESSAGE_PREVIEW, CCORD_REPORT_EL_USER_PREVIEW } ccord_report_element_kind;

typedef struct ccord_report_choice { const char *value, *label, *description; } ccord_report_choice;   // checkbox row / dropdown option
typedef struct ccord_report_element {
  const char *name; ccord_report_element_kind kind; bool should_submit;
  const char *title, *subtitle, *placeholder, *body;           // free_text / dropdown / text
  const char *url, *link_text;                                  // external_link
  const ccord_report_choice *choices; size_t choice_count;      // checkbox / dropdown
  uint32_t rows, character_limit;                               // free_text (character_limit 0 -> 800)
} ccord_report_element;
typedef struct ccord_report_child { const char *name; uint32_t target; } ccord_report_child;
typedef struct ccord_report_node {
  uint32_t id; const char *key, *header, *subheader, *info, *report_type;
  const ccord_report_child *children; size_t child_count;
  const ccord_report_element *elements; size_t element_count;
  ccord_report_button button; uint32_t button_target; bool has_button_target;
  bool multi_select_required, auto_submit;
} ccord_report_node;
typedef struct ccord_report_menu {
  ccord_arena arena;                                    // owns every string and array below
  char name[24], version[16], variant[64], language[16];
  uint32_t root, success, fail;
  ccord_report_node *nodes; size_t node_count;
} ccord_report_menu;

// Parses the menu response. On failure *out is left empty (nothing to free). CCORD_ERR_PARSE for malformed JSON
// or a missing root node. Unknown element types become CCORD_REPORT_EL_UNKNOWN, never an error.
[[nodiscard]] ccord_status ccord_report_menu_parse(ccord_report_menu *out, const char *json, size_t len, ccord_error *err);
const ccord_report_node *ccord_report_menu_node(const ccord_report_menu *m, uint32_t id);   // nullptr when the id is not in nodes
void ccord_report_menu_free(ccord_report_menu *m);

// What a submit sends. `elements` are already ordered and de-duplicated by the caller.
typedef struct ccord_report_value { const char *name; const char *const *values; size_t value_count; } ccord_report_value;
typedef struct ccord_report_request {
  ccord_report_type type;
  const char *version, *variant, *language;             // echoed from the menu (language nullptr -> "en")
  const uint32_t *breadcrumbs; size_t breadcrumb_count; // 1..CCORD_REPORT_MAX_PATH (32)
  const ccord_report_value *elements; size_t element_count;
  ccord_id channel_id, message_id;                      // type message
  ccord_id reported_user_id;                            // type user
} ccord_report_request;
// Writes the POST body JSON into *out (which the caller inits and frees).
[[nodiscard]] ccord_status ccord_report_build_body(const ccord_report_request *req, ccord_buf *out, ccord_error *err);
