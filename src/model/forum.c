// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>
#include <ccord/core/utf8.h>
#include <ccord/model/forum.h>

#define DISCORD_EPOCH_MS 1420070400000ll
#define SNOWFLAKE_MIN_MS 31536000000ull   // ids from the first year after the epoch are treated as synthetic

int64_t ccord_snowflake_unix_ms(ccord_id id)
{
  uint64_t ms = (uint64_t)id >> 22;
  if (ms < SNOWFLAKE_MIN_MS) return 0;
  return (int64_t)ms + DISCORD_EPOCH_MS;
}

size_t ccord_forum_strip_markdown(const char *src, size_t len, char *dst, size_t cap)
{
  if (!dst || cap == 0) return 0;
  dst[0] = '\0';
  if (!src || len == 0 || cap == 1) return 0;

  size_t out = 0;
  size_t i = 0;
  bool line_start = true;
  bool need_space = false;

  while (i < len && out + 1 < cap) {
    char c = src[i];
    if (c == '\r') {
      i++;
      continue;
    }
    if (c == '\n') {
      line_start = true;
      if (out > 0) need_space = true;
      i++;
      continue;
    }
    if (c == ' ' || c == '\t') {
      if (out > 0) need_space = true;
      i++;
      continue;
    }
    if (line_start) {
      // Skip blockquotes "> " and headers "# " at the start of a line.
      if (c == '>' && i + 1 < len && (src[i + 1] == ' ' || src[i + 1] == '\t')) {
        i += 2;
        continue;
      }
      if (c == '#') {
        size_t h = i;
        while (h < len && src[h] == '#' && h - i < 6) h++;
        if (h < len && (src[h] == ' ' || src[h] == '\t')) {
          i = h + 1;
          continue;
        }
      }
      if ((c == '-' || c == '*') && i + 1 < len && (src[i + 1] == ' ' || src[i + 1] == '\t')) {
        i += 2;
        line_start = false;
        continue;
      }
      line_start = false;
    }

    // Strip code fence ```lang\n
    if (i + 2 < len && src[i] == '`' && src[i + 1] == '`' && src[i + 2] == '`') {
      i += 3;
      while (i < len && src[i] != '\n' && src[i] != '`' && src[i] != ' ') i++;
      if (out > 0) need_space = true;
      continue;
    }
    // Strip inline markdown tokens: `, *, **, ***, __, ~~, ||
    if (c == '`' || c == '*') {
      i++;
      continue;
    }
    if (i + 1 < len && ((c == '_' && src[i + 1] == '_') ||
                        (c == '~' && src[i + 1] == '~') ||
                        (c == '|' && src[i + 1] == '|'))) {
      i += 2;
      continue;
    }
    // Convert [label](url) -> label
    if (c == '[') {
      size_t cb = i + 1;
      while (cb < len && src[cb] != ']' && src[cb] != '\n') cb++;
      if (cb > i + 1 && cb + 1 < len && src[cb] == ']' && src[cb + 1] == '(') {
        size_t cp = cb + 2;
        while (cp < len && src[cp] != ')' && src[cp] != '\n') cp++;
        if (cp < len && src[cp] == ')') {
          if (need_space && out + 1 < cap) {
            dst[out++] = ' ';
            need_space = false;
          }
          size_t llen = cb - (i + 1);
          size_t pos = 0;
          while (pos < llen) {
            size_t next = ccord_utf8_next(src + i + 1, llen, pos);
            size_t clen = next - pos;
            if (out + clen >= cap) break;
            memcpy(dst + out, src + i + 1 + pos, clen);
            out += clen;
            pos = next;
          }
          i = cp + 1;
          continue;
        }
      }
    }

    if (need_space) {
      if (out + 1 >= cap) break;
      dst[out++] = ' ';
      need_space = false;
    }

    size_t next = ccord_utf8_next(src, len, i);
    size_t clen = next - i;
    if (clen == 0) clen = 1;
    if (out + clen >= cap) break;
    memcpy(dst + out, src + i, clen);
    out += clen;
    i = next;
  }

  dst[out] = '\0';
  return out;
}
