// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/error.h>

#define CCORD_TEXT_UNDO_CAP 16u

typedef struct ccord_text_undo_entry {
  char *data;
  size_t len, caret, anchor;
} ccord_text_undo_entry;

// Composer text model. UTF-8, caret and selection always on codepoint boundaries. No rendering.
typedef struct ccord_text_edit {
  char *data;
  size_t len, cap, max_bytes;
  size_t caret, anchor;   // anchor==caret -> no selection
  ccord_text_undo_entry *undo;
  size_t undo_len, undo_cap;
  ccord_text_undo_entry *redo;
  size_t redo_len, redo_cap;
  size_t last_undo_caret;
  uint8_t last_undo_op;
} ccord_text_edit;

void ccord_text_edit_init(ccord_text_edit *t, size_t max_bytes);
void ccord_text_edit_free(ccord_text_edit *t);
void ccord_text_edit_clear(ccord_text_edit *t);
const char *ccord_text_edit_cstr(const ccord_text_edit *t);
// Inserts valid UTF-8 at the caret (replacing selection). Past max_bytes -> CCORD_ERR_RANGE and nothing inserted.
[[nodiscard]] ccord_status ccord_text_edit_insert(ccord_text_edit *t, const char *utf8, size_t len, ccord_error *err);
// Smart paste: if a single-line selection is active and `utf8` is an http(s) URL, wraps as [selection](url).
[[nodiscard]] ccord_status ccord_text_edit_paste(ccord_text_edit *t, const char *utf8, size_t len, ccord_error *err);
// Wraps the current selection with `prefix` and `suffix` (e.g. "**" for bold), or inserts both and places caret inside.
[[nodiscard]] ccord_status ccord_text_edit_wrap_selection(ccord_text_edit *t, const char *prefix, const char *suffix,
                                                          ccord_error *err);
void ccord_text_edit_backspace(ccord_text_edit *t);
void ccord_text_edit_delete(ccord_text_edit *t);
void ccord_text_edit_backspace_word(ccord_text_edit *t);
void ccord_text_edit_delete_word(ccord_text_edit *t);
// dir: -1 left, +1 right, -2 buffer home, +2 buffer end, -3 word left, +3 word right, -4 line home, +4 line end
void ccord_text_edit_move(ccord_text_edit *t, int dir, bool extend);
void ccord_text_edit_select_all(ccord_text_edit *t);
void ccord_text_edit_select_word_at(ccord_text_edit *t, size_t byte_pos);
void ccord_text_edit_select_line_at(ccord_text_edit *t, size_t byte_pos);
void ccord_text_edit_set_caret(ccord_text_edit *t, size_t byte_pos, bool extend);
bool ccord_text_edit_undo(ccord_text_edit *t);
bool ccord_text_edit_redo(ccord_text_edit *t);
[[nodiscard]] char *ccord_text_edit_copy_selection(const ccord_text_edit *t);   // ccord_free(); nullptr if empty
