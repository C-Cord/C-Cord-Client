// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/ini.h>

#include <stdbool.h>
#include <stdint.h>
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

typedef struct seen_slot {
  uint64_t hash;
  char *key;
} seen_slot;

typedef struct seen_set {
  seen_slot *slots;
  size_t count;
  size_t cap;
} seen_set;

static uint64_t fnv1a64(const char *s, size_t n)
{
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static void seen_free(seen_set *sl)
{
  for (size_t i = 0; i < sl->cap; i++) ccord_free(sl->slots[i].key);
  ccord_free(sl->slots);
  *sl = (seen_set){0};
}

// Records "section.key"; CCORD_ERR_PARSE if that pair was already seen in this file.
static ccord_status seen_add(seen_set *sl, const char *section, size_t sn, const char *key, size_t kn, ccord_error *err, int line)
{
  if (sn > INI_NAME_MAX || kn > INI_NAME_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "line %d: name too long", line);
  }
  size_t flen = sn + 1u + kn;
  char buf[INI_NAME_MAX * 2u + 2u];
  memcpy(buf, section, sn);
  buf[sn] = '\x1f';
  memcpy(buf + sn + 1u, key, kn);
  buf[flen] = '\0';

  uint64_t h = fnv1a64(buf, flen);
  size_t at = 0;
  if (sl->cap > 0) {
    size_t mask = sl->cap - 1u;
    at = (size_t)h & mask;
    while (sl->slots[at].key != nullptr) {
      if (sl->slots[at].hash == h && strcmp(sl->slots[at].key, buf) == 0) {
        return ccord_error_set(err, CCORD_ERR_PARSE, "line %d: duplicate key '%s' in [%s]", line, key, section);
      }
      at = (at + 1u) & mask;
    }
  }
  if (sl->count >= INI_ENTRIES_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "line %d: too many entries", line);
  }
  if (sl->cap == 0 || (sl->count + 1u) * 2u > sl->cap) {
    size_t ncap = sl->cap ? sl->cap * 2u : 32u;
    seen_slot *ns = ccord_calloc(ncap, sizeof *ns);
    if (!ns) return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
    size_t nmask = ncap - 1u;
    for (size_t i = 0; i < sl->cap; i++) {
      if (!sl->slots[i].key) continue;
      size_t slot_pos = (size_t)sl->slots[i].hash & nmask;
      while (ns[slot_pos].key != nullptr) slot_pos = (slot_pos + 1u) & nmask;
      ns[slot_pos] = sl->slots[i];
    }
    ccord_free(sl->slots);
    sl->slots = ns;
    sl->cap = ncap;
    at = (size_t)h & nmask;
    while (sl->slots[at].key != nullptr) at = (at + 1u) & nmask;
  }
  char *full = ccord_malloc(flen + 1u);
  if (!full) return ccord_error_set(err, CCORD_ERR_NOMEM, "out of memory");
  memcpy(full, buf, flen + 1u);
  sl->slots[at].hash = h;
  sl->slots[at].key = full;
  sl->count++;
  return CCORD_OK;
}

ccord_status ccord_ini_parse(const char *text, size_t len, ccord_ini_visit visit, void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!text && len) return ccord_error_set(err, CCORD_ERR_INVALID, "ini: null input");
  if (!visit) return ccord_error_set(err, CCORD_ERR_INVALID, "ini: null visitor");

  char section[INI_NAME_MAX + 1] = "";
  size_t section_len = 0;
  bool have_section = false;
  seen_set seen = {0};
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
      size_t sn = (size_t)(name_end - name);
      if (!name_ok(name, sn) || sn > INI_NAME_MAX) {
        st = ccord_error_set(err, CCORD_ERR_PARSE, "line %d: bad section name", line_no);
        break;
      }
      memcpy(section, name, sn);
      section[sn] = '\0';
      section_len = sn;
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

    st = seen_add(&seen, section, section_len, key, kn, err, line_no);
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
