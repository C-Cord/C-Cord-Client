// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/text_edit.h>

#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>

static size_t min_sz(size_t a, size_t b)
{
  return a < b ? a : b;
}

static size_t max_sz(size_t a, size_t b)
{
  return a > b ? a : b;
}

static void free_stack(ccord_text_undo_entry *stk, size_t *len)
{
  for (size_t i = 0; i < *len; i++) {
    ccord_free(stk[i].data);
    stk[i] = (ccord_text_undo_entry){0};
  }
  *len = 0;
}

static void push_entry(ccord_text_undo_entry *stk, size_t *len, const ccord_text_edit *t)
{
  const char *src = t->data ? t->data : "";
  // Deduplicate identical consecutive snapshots
  if (*len > 0) {
    const ccord_text_undo_entry *top = &stk[*len - 1];
    if (top->len == t->len && memcmp(top->data ? top->data : "", src, t->len) == 0) {
      return;
    }
  }
  char *copy = ccord_strndup(src, t->len);
  if (!copy) return;   // best-effort under OOM
  if (*len == CCORD_TEXT_UNDO_CAP) {
    ccord_free(stk[0].data);
    memmove(&stk[0], &stk[1], (CCORD_TEXT_UNDO_CAP - 1u) * sizeof stk[0]);
    (*len)--;
  }
  stk[*len] = (ccord_text_undo_entry){
    .data = copy,
    .len = t->len,
    .caret = t->caret,
    .anchor = t->anchor,
  };
  (*len)++;
}

static void record_undo(ccord_text_edit *t)
{
  push_entry(t->undo, &t->undo_len, t);
  free_stack(t->redo, &t->redo_len);
}

void ccord_text_edit_init(ccord_text_edit *t, size_t max_bytes)
{
  *t = (ccord_text_edit){.max_bytes = max_bytes};
}

void ccord_text_edit_free(ccord_text_edit *t)
{
  if (!t) return;
  free_stack(t->undo, &t->undo_len);
  free_stack(t->redo, &t->redo_len);
  ccord_free(t->data);
  size_t max = t->max_bytes;
  ccord_text_edit_init(t, max);
}

void ccord_text_edit_clear(ccord_text_edit *t)
{
  if (!t) return;
  free_stack(t->undo, &t->undo_len);
  free_stack(t->redo, &t->redo_len);
  t->len = 0;
  t->caret = 0;
  t->anchor = 0;
  if (t->data) t->data[0] = '\0';
}

const char *ccord_text_edit_cstr(const ccord_text_edit *t)
{
  return (t && t->data) ? t->data : "";
}

static bool has_selection(const ccord_text_edit *t)
{
  return t->caret != t->anchor;
}

static bool is_word_byte(unsigned char c)
{
  if (c >= 0x80u) return true;   // non-ASCII UTF-8 bytes treated as word characters
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static size_t prev_word_pos(const ccord_text_edit *t, size_t pos)
{
  if (!t->data || pos == 0) return 0;
  size_t p = pos;
  while (p > 0) {
    size_t prev = ccord_utf8_prev(t->data, p);
    if (is_word_byte((unsigned char)t->data[prev])) break;
    p = prev;
  }
  while (p > 0) {
    size_t prev = ccord_utf8_prev(t->data, p);
    if (!is_word_byte((unsigned char)t->data[prev])) break;
    p = prev;
  }
  return p;
}

static size_t next_word_pos(const ccord_text_edit *t, size_t pos)
{
  if (!t->data || pos >= t->len) return t->len;
  size_t p = pos;
  while (p < t->len && !is_word_byte((unsigned char)t->data[p])) {
    p = ccord_utf8_next(t->data, t->len, p);
  }
  while (p < t->len && is_word_byte((unsigned char)t->data[p])) {
    p = ccord_utf8_next(t->data, t->len, p);
  }
  return p;
}

static size_t line_start_pos(const ccord_text_edit *t, size_t pos)
{
  if (!t->data || pos == 0) return 0;
  size_t p = pos > t->len ? t->len : pos;
  while (p > 0 && t->data[p - 1] != '\n') p--;
  return p;
}

static size_t line_end_pos(const ccord_text_edit *t, size_t pos)
{
  if (!t->data || pos >= t->len) return t ? t->len : 0;
  size_t p = pos;
  while (p < t->len && t->data[p] != '\n') p++;
  return p;
}

// Removes [from, to) and puts the caret at `from`.
static void erase_range(ccord_text_edit *t, size_t from, size_t to)
{
  if (to > t->len) to = t->len;
  if (from >= to) return;
  memmove(t->data + from, t->data + to, t->len - to);
  t->len -= to - from;
  t->data[t->len] = '\0';
  t->caret = from;
  t->anchor = from;
}

static void erase_selection(ccord_text_edit *t)
{
  erase_range(t, min_sz(t->caret, t->anchor), max_sz(t->caret, t->anchor));
}

ccord_status ccord_text_edit_insert(ccord_text_edit *t, const char *utf8, size_t len, ccord_error *err)
{
  if (len == 0) return CCORD_OK;
  if (!utf8 || !ccord_utf8_valid(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "text edit: invalid UTF-8");
  size_t sel = has_selection(t) ? max_sz(t->caret, t->anchor) - min_sz(t->caret, t->anchor) : 0;
  size_t after = t->len - sel;
  if (len > t->max_bytes || after > t->max_bytes - len) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "text edit: limit of %zu bytes reached", t->max_bytes);
  }
  size_t need = after + len + 1;
  if (need > t->cap) {
    size_t ncap = t->cap ? t->cap : 64;
    while (ncap < need) ncap *= 2;
    char *n = ccord_realloc(t->data, ncap);
    if (!n) return ccord_error_set(err, CCORD_ERR_NOMEM, "text edit: out of memory");
    t->data = n;
    t->cap = ncap;
  }
  record_undo(t);
  erase_selection(t);
  memmove(t->data + t->caret + len, t->data + t->caret, t->len - t->caret);
  memcpy(t->data + t->caret, utf8, len);
  t->len += len;
  t->data[t->len] = '\0';
  t->caret += len;
  t->anchor = t->caret;
  return CCORD_OK;
}

static bool looks_like_url(const char *s, size_t len)
{
  if (!s || len < 10) return false;
  bool https = len > 8 && strncmp(s, "https://", 8) == 0;
  bool http = len > 7 && strncmp(s, "http://", 7) == 0;
  if (!https && !http) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch <= 0x20u || ch == 0x7fu || ch == ')' || ch == '>') return false;
  }
  return true;
}

ccord_status ccord_text_edit_paste(ccord_text_edit *t, const char *utf8, size_t len, ccord_error *err)
{
  if (!t) return ccord_error_set(err, CCORD_ERR_INVALID, "text edit: null editor");
  if (has_selection(t) && looks_like_url(utf8, len)) {
    size_t from = min_sz(t->caret, t->anchor);
    size_t to = max_sz(t->caret, t->anchor);
    size_t slen = to - from;
    bool single_line = slen > 0 && slen <= 512 && memchr(t->data + from, '\n', slen) == nullptr &&
                       memchr(t->data + from, ']', slen) == nullptr;
    if (single_line && !looks_like_url(t->data + from, slen)) {
      size_t wrapped_len = slen + len + 4;   // '[' + sel + "](" + url + ')'
      if (wrapped_len <= t->max_bytes && (t->len - slen) <= t->max_bytes - wrapped_len) {
        char *buf = ccord_malloc(wrapped_len + 1);
        if (buf) {
          buf[0] = '[';
          memcpy(buf + 1, t->data + from, slen);
          buf[1 + slen] = ']';
          buf[2 + slen] = '(';
          memcpy(buf + 3 + slen, utf8, len);
          buf[3 + slen + len] = ')';
          buf[wrapped_len] = '\0';
          ccord_status st = ccord_text_edit_insert(t, buf, wrapped_len, err);
          ccord_free(buf);
          return st;
        }
      }
    }
  }
  return ccord_text_edit_insert(t, utf8, len, err);
}

ccord_status ccord_text_edit_wrap_selection(ccord_text_edit *t, const char *prefix, const char *suffix,
                                            ccord_error *err)
{
  if (!t || !prefix || !suffix) return ccord_error_set(err, CCORD_ERR_INVALID, "text edit: null argument");
  size_t plen = strlen(prefix), slen = strlen(suffix);
  size_t from = min_sz(t->caret, t->anchor);
  size_t to = max_sz(t->caret, t->anchor);
  size_t sel_len = to - from;
  size_t add_len = plen + slen;
  if (add_len > t->max_bytes || t->len > t->max_bytes - add_len) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "text edit: limit of %zu bytes reached", t->max_bytes);
  }
  size_t need = t->len + add_len + 1;
  if (need > t->cap) {
    size_t ncap = t->cap ? t->cap : 64;
    while (ncap < need) ncap *= 2;
    char *n = ccord_realloc(t->data, ncap);
    if (!n) return ccord_error_set(err, CCORD_ERR_NOMEM, "text edit: out of memory");
    t->data = n;
    t->cap = ncap;
  }
  record_undo(t);
  // Move tail after `to` by plen + slen
  memmove(t->data + to + add_len, t->data + to, t->len - to);
  // Move selected slice `[from, to)` right by plen
  if (sel_len > 0) {
    memmove(t->data + from + plen, t->data + from, sel_len);
  }
  memcpy(t->data + from, prefix, plen);
  memcpy(t->data + from + plen + sel_len, suffix, slen);
  t->len += add_len;
  t->data[t->len] = '\0';
  t->anchor = from + plen;
  t->caret = from + plen + sel_len;
  return CCORD_OK;
}

void ccord_text_edit_backspace(ccord_text_edit *t)
{
  if (has_selection(t)) {
    record_undo(t);
    erase_selection(t);
  } else if (t->caret > 0) {
    record_undo(t);
    erase_range(t, ccord_utf8_prev(t->data, t->caret), t->caret);
  }
}

void ccord_text_edit_delete(ccord_text_edit *t)
{
  if (has_selection(t)) {
    record_undo(t);
    erase_selection(t);
  } else if (t->caret < t->len) {
    record_undo(t);
    erase_range(t, t->caret, ccord_utf8_next(t->data, t->len, t->caret));
  }
}

void ccord_text_edit_backspace_word(ccord_text_edit *t)
{
  if (!t) return;
  if (has_selection(t)) {
    record_undo(t);
    erase_selection(t);
  } else if (t->caret > 0) {
    record_undo(t);
    erase_range(t, prev_word_pos(t, t->caret), t->caret);
  }
}

void ccord_text_edit_delete_word(ccord_text_edit *t)
{
  if (!t) return;
  if (has_selection(t)) {
    record_undo(t);
    erase_selection(t);
  } else if (t->caret < t->len) {
    record_undo(t);
    erase_range(t, t->caret, next_word_pos(t, t->caret));
  }
}

void ccord_text_edit_move(ccord_text_edit *t, int dir, bool extend)
{
  size_t pos = t->caret;
  if (!extend && has_selection(t) && (dir == -1 || dir == 1 || dir == -3 || dir == 3)) {
    // Collapsing a selection with an arrow key jumps to the matching edge.
    pos = dir < 0 ? min_sz(t->caret, t->anchor) : max_sz(t->caret, t->anchor);
  } else if (dir == -1) {
    pos = t->caret > 0 ? ccord_utf8_prev(t->data, t->caret) : 0;
  } else if (dir == 1) {
    pos = t->caret < t->len ? ccord_utf8_next(t->data, t->len, t->caret) : t->len;
  } else if (dir == -2) {
    pos = 0;
  } else if (dir == 2) {
    pos = t->len;
  } else if (dir == -3) {
    pos = prev_word_pos(t, t->caret);
  } else if (dir == 3) {
    pos = next_word_pos(t, t->caret);
  } else if (dir == -4) {
    pos = line_start_pos(t, t->caret);
  } else if (dir == 4) {
    pos = line_end_pos(t, t->caret);
  }
  t->caret = pos;
  if (!extend) t->anchor = pos;
}

void ccord_text_edit_select_all(ccord_text_edit *t)
{
  t->anchor = 0;
  t->caret = t->len;
}

void ccord_text_edit_select_word_at(ccord_text_edit *t, size_t byte_pos)
{
  if (!t || !t->data || t->len == 0) return;
  ccord_text_edit_set_caret(t, byte_pos, false);
  size_t p = t->caret;
  if (p == t->len && p > 0) p = ccord_utf8_prev(t->data, p);
  bool w = is_word_byte((unsigned char)t->data[p]);
  size_t lo = p, hi = ccord_utf8_next(t->data, t->len, p);
  while (lo > 0) {
    size_t prev = ccord_utf8_prev(t->data, lo);
    if (is_word_byte((unsigned char)t->data[prev]) != w || t->data[prev] == '\n') break;
    lo = prev;
  }
  while (hi < t->len) {
    if (is_word_byte((unsigned char)t->data[hi]) != w || t->data[hi] == '\n') break;
    hi = ccord_utf8_next(t->data, t->len, hi);
  }
  t->anchor = lo;
  t->caret = hi;
}

void ccord_text_edit_select_line_at(ccord_text_edit *t, size_t byte_pos)
{
  if (!t || !t->data || t->len == 0) return;
  ccord_text_edit_set_caret(t, byte_pos, false);
  t->anchor = line_start_pos(t, t->caret);
  t->caret = line_end_pos(t, t->caret);
}

void ccord_text_edit_set_caret(ccord_text_edit *t, size_t byte_pos, bool extend)
{
  if (byte_pos > t->len) byte_pos = t->len;
  // Snap to a code point boundary (move back to the lead byte).
  while (byte_pos > 0 && byte_pos < t->len && ((uint8_t)t->data[byte_pos] & 0xC0u) == 0x80u) byte_pos--;
  t->caret = byte_pos;
  if (!extend) t->anchor = byte_pos;
}

static bool restore_entry(ccord_text_edit *t, ccord_text_undo_entry *src_stk, size_t *src_len,
                          ccord_text_undo_entry *dst_stk, size_t *dst_len)
{
  if (!t || *src_len == 0) return false;
  push_entry(dst_stk, dst_len, t);
  ccord_text_undo_entry e = src_stk[*src_len - 1];
  src_stk[*src_len - 1] = (ccord_text_undo_entry){0};
  (*src_len)--;
  size_t need = e.len + 1;
  if (need > t->cap) {
    size_t ncap = t->cap ? t->cap : 64;
    while (ncap < need) ncap *= 2;
    char *n = ccord_realloc(t->data, ncap);
    if (!n) {
      ccord_free(e.data);
      return false;
    }
    t->data = n;
    t->cap = ncap;
  }
  if (t->data) {
    if (e.len > 0 && e.data) memcpy(t->data, e.data, e.len);
    t->data[e.len] = '\0';
  }
  t->len = e.len;
  t->caret = e.caret <= e.len ? e.caret : e.len;
  t->anchor = e.anchor <= e.len ? e.anchor : e.len;
  ccord_free(e.data);
  return true;
}

bool ccord_text_edit_undo(ccord_text_edit *t)
{
  return restore_entry(t, t->undo, &t->undo_len, t->redo, &t->redo_len);
}

bool ccord_text_edit_redo(ccord_text_edit *t)
{
  return restore_entry(t, t->redo, &t->redo_len, t->undo, &t->undo_len);
}

char *ccord_text_edit_copy_selection(const ccord_text_edit *t)
{
  if (!has_selection(t)) return nullptr;
  size_t from = min_sz(t->caret, t->anchor), to = max_sz(t->caret, t->anchor);
  return ccord_strndup(t->data + from, to - from);
}
