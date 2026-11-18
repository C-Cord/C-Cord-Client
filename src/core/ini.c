// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/ini.h>

#include <stdbool.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>

#define INI_LINE_MAX 512u
#define INI_ENTRIES_MAX 16384u
#define INI_NAME_MAX 63u

static bool name_char_ok(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

static bool name_ok(const char *s, size_t n)
{
  if (n == 0) return false;
  for (size_t i = 0; i < n; i++) {
    if (!name_char_ok(s[i])) return false;
  }
  return true;
}

static const char *trim_left(const char *s, const char *end)
{
  while (s < end && (*s == ' ' || *s == '\t')) s++;
  return s;
}

static const char *trim_right(const char *s, const char *end)
{
  while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
  return end;
}

typedef struct seen_list {
  char **items;
  size_t count;
} seen_list;

static void seen_free(seen_list *sl)
{
  for (size_t i = 0; i < sl->count; i++) ccord_free(sl->items[i]);
  ccord_free(sl->items);
  sl->items = nullptr;
  sl->count = 0;
}

// Records "section.key"; CCORD_ERR_PARSE if that pair was already seen in this file.
static ccord_status seen_add(seen_list *sl, const char *section, size_t sn, const char *key, size_t kn, ccord_error *err, int line)
{
  char *full = ccord_malloc(sn + 1 + kn + 1);
  if (!full) return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
  memcpy(full, section, sn);
  full[sn] = '.';
  memcpy(full + sn + 1, key, kn);
  full[sn + 1 + kn] = '\0';
  for (size_t i = 0; i < sl->count; i++) {
    if (strcmp(sl->items[i], full) == 0) {
      ccord_free(full);
      return ccord_error_set(err, CCORD_ERR_PARSE, "line %d: duplicate key '%s' in [%s]", line, key, section);
    }
  }
  if (sl->count >= INI_ENTRIES_MAX) {
    ccord_free(full);
    return ccord_error_set(err, CCORD_ERR_RANGE, "line %d: too many entries", line);
  }
  char **grown = ccord_realloc_array(sl->items, sl->count + 1, sizeof *grown);
  if (!grown) {
    ccord_free(full);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
  }
  sl->items = grown;
  sl->items[sl->count++] = full;
  return CCORD_OK;
}

ccord_status ccord_ini_parse(const char *text, size_t len, ccord_ini_visit visit, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!text && len) return ccord_error_set(err, CCORD_ERR_INVALID, "ini: null input");
  if (!visit) return ccord_error_set(err, CCORD_ERR_INVALID, "ini: null visitor");

  char section[INI_NAME_MAX + 1] = "";
  bool have_section = false;
  seen_list seen = {nullptr, 0};
  ccord_status st = CCORD_OK;
  size_t pos = 0;
  int line_no = 0;

  while (pos < len) {
    const char *nl = memchr(text + pos, '\n', len - pos);
    size_t line_end = nl ? (size_t)(nl - text) : len;
    size_t next = nl ? line_end + 1 : len;
    line_no++;

    size_t raw_len = line_end - pos;
    if (raw_len > 0 && text[pos + raw_len - 1] == '\r') raw_len--;
    if (raw_len > INI_LINE_MAX) {
      st = ccord_error_set(err, CCORD_ERR_RANGE, "line %d: longer than %u bytes", line_no, INI_LINE_MAX);
      break;
    }
    const char *line = text + pos;
    if (!ccord_utf8_valid_text(line, raw_len)) {
      st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: invalid text", line_no);
      break;
    }
    pos = next;

    if (raw_len > 0 && line[0] == '#') continue; // comment only at line start
    const char *s = trim_left(line, line + raw_len);
    const char *e = trim_right(s, line + raw_len);
    if (s == e) continue;

    if (*s == '[') {
      if (e - s < 3 || e[-1] != ']') {
        st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: malformed section", line_no);
        break;
      }
      const char *name = s + 1, *name_end = e - 1;
      if (!name_ok(name, (size_t)(name_end - name)) || (size_t)(name_end - name) > INI_NAME_MAX) {
        st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: bad section name", line_no);
        break;
      }
      memcpy(section, name, (size_t)(name_end - name));
      section[name_end - name] = '\0';
      have_section = true;
      continue;
    }

    const char *eq = memchr(s, '=', (size_t)(e - s));
    if (!eq) {
      st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: expected key = value", line_no);
      break;
    }
    const char *kb = s, *ke = trim_right(s, eq);
    const char *vb = trim_left(eq + 1, e), *ve = e;
    if (!name_ok(kb, (size_t)(ke - kb))) {
      st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: bad key name", line_no);
      break;
    }
    if (!have_section) {
      st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: key outside a section", line_no);
      break;
    }
    char key[INI_NAME_MAX + 1];
    size_t kn = (size_t)(ke - kb);
    if (kn > INI_NAME_MAX) {
      st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: key too long", line_no);
      break;
    }
    memcpy(key, kb, kn);
    key[kn] = '\0';
    char value[INI_LINE_MAX + 1];
    size_t vn = (size_t)(ve - vb);
    memcpy(value, vb, vn);
    value[vn] = '\0';

    st = seen_add(&seen, section, strlen(section), key, kn, err, line_no);
    if (st != CCORD_OK) break;

    ccord_error inner;
    ccord_error_clear(&inner);
    if (!visit(section, key, value, line_no, ud, &inner)) {
      if (inner.code != CCORD_OK) {
        st = ccord_error_set(err, inner.code, "%s", inner.message);
      } else {
        st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: rejected value for '%s'", line_no, key);
      }
      break;
    }
  }
  seen_free(&seen);
  return st;
}
