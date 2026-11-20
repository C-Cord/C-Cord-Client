// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/utf8.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Returns the number of bytes of the sequence starting at s[0] (1..4), or 0 if malformed.
static size_t seq_len(const unsigned char *s, size_t len, char32_t *cp)
{
  if (!s || len == 0 || !cp) return 0;
  unsigned char c = s[0];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  size_t n;
  char32_t v;
  char32_t min;
  if (c >= 0xC2 && c <= 0xDF) {
    n = 2; v = c & 0x1Fu; min = 0x80;
  } else if (c >= 0xE0 && c <= 0xEF) {
    n = 3; v = c & 0x0Fu; min = 0x800;
  } else if (c >= 0xF0 && c <= 0xF4) {
    n = 4; v = c & 0x07u; min = 0x10000;
  } else {
    return 0;
  }
  if (len < n) {
    return 0;
  }
  for (size_t i = 1; i < n; i++) {
    if ((s[i] & 0xC0u) != 0x80u) {
      return 0;
    }
    v = (v << 6) | (s[i] & 0x3Fu);
  }
  if (v < min || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) {
    return 0;
  }
  *cp = v;
  return n;
}

bool ccord_utf8_valid(const char *s, size_t len)
{
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < len) {
    while (len - i >= 8u) {
      uint64_t w;
      memcpy(&w, p + i, sizeof w);
      if ((w & 0x8080808080808080ULL) != 0) {
        break;
      }
      i += 8u;
    }
    while (i < len && p[i] < 0x80u) {
      i++;
    }
    if (i >= len) {
      break;
    }
    char32_t cp;
    size_t n = seq_len(p + i, len - i, &cp);
    if (n == 0) {
      return false;
    }
    i += n;
  }
  return true;
}

bool ccord_utf8_valid_text(const char *s, size_t len)
{
  const unsigned char *p = (const unsigned char *)s;
  const uint64_t hi = 0x8080808080808080ULL;
  size_t i = 0;
  while (i < len) {
    while (len - i >= 8u) {
      uint64_t w;
      memcpy(&w, p + i, sizeof w);
      // Fast-path 8 printable ASCII bytes (0x20..0x7E): high bit 0 guarantees zero inter-byte carries.
      uint64_t bad = (w & hi) |
                     ((w + 0x0101010101010101ULL) & hi) |
                     (~(w + 0x6060606060606060ULL) & hi);
      if (bad != 0) {
        break;
      }
      i += 8u;
    }
    while (i < len && p[i] >= 0x20u && p[i] <= 0x7Eu) {
      i++;
    }
    if (i >= len) {
      break;
    }
    unsigned char c = p[i];
    if (c == '\n' || c == '\t') {
      i++;
      continue;
    }
    if (c < 0x80u) {
      return false; // C0 control (other than \n/\t) or DEL (0x7F)
    }
    char32_t cp;
    size_t n = seq_len(p + i, len - i, &cp);
    if (n == 0) {
      return false;
    }
    if (cp >= 0x80u && cp <= 0x9Fu) {
      return false; // C1 controls
    }
    i += n;
  }
  return true;
}

int ccord_utf8_decode(const char *s, size_t len, char32_t *cp)
{
  size_t n = seq_len((const unsigned char *)s, len, cp);
  return n == 0 ? -1 : (int)n;
}

size_t ccord_utf8_encode(char32_t cp, char out[static 4])
{
  if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
    return 0;
  }
  if (cp < 0x80u) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800u) {
    out[0] = (char)(0xC0u | (cp >> 6));
    out[1] = (char)(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp < 0x10000u) {
    out[0] = (char)(0xE0u | (cp >> 12));
    out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
  }
  out[0] = (char)(0xF0u | (cp >> 18));
  out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
  out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
  out[3] = (char)(0x80u | (cp & 0x3Fu));
  return 4;
}

size_t ccord_utf8_next(const char *s, size_t len, size_t pos)
{
  if (pos >= len) {
    return len;
  }
  size_t i = pos + 1;
  while (i < len && (((unsigned char)s[i]) & 0xC0u) == 0x80u) {
    i++;
  }
  return i;
}

size_t ccord_utf8_prev(const char *s, size_t pos)
{
  if (pos == 0) {
    return 0;
  }
  size_t i = pos - 1;
  while (i > 0 && (((unsigned char)s[i]) & 0xC0u) == 0x80u) {
    i--;
  }
  return i;
}
