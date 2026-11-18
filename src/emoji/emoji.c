// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/emoji/emoji.h>

#include <string.h>

#include "emoji_data.h"

enum {
  MAX_NAME = 64,          // longest shortcode / query we are willing to look at
  MAX_ENTRIES = 4096,     // upper bound for the stack scratch arrays (checked against the generated count)
  TIER_NONE = 0xFF,
};

static const char *const k_category_names[CCORD_EMOJI_CATEGORY_COUNT] = {
    "People", "Nature", "Food", "Activities", "Travel", "Objects", "Symbols", "Flags"};

size_t ccord_emoji_count(void)
{
  return ccord_emoji_data_count;
}

const ccord_emoji_entry *ccord_emoji_at(size_t i)
{
  return i < ccord_emoji_data_count ? &ccord_emoji_data[i] : nullptr;
}

const char *ccord_emoji_category_name(ccord_emoji_category c)
{
  return (unsigned)c < CCORD_EMOJI_CATEGORY_COUNT ? k_category_names[c] : "";
}

void ccord_emoji_category_range(ccord_emoji_category c, size_t *first, size_t *count)
{
  size_t f = 0, n = 0;
  if ((unsigned)c < CCORD_EMOJI_CATEGORY_COUNT) {
    f = ccord_emoji_cat_start[c];
    n = (size_t)ccord_emoji_cat_start[c + 1] - f;
  }
  if (first) *first = f;
  if (count) *count = n;
}

// ---- tones -----------------------------------------------------------------------------------------------------

static const ccord_emoji_tone_set *find_tone_set(size_t idx)
{
  size_t lo = 0, hi = ccord_emoji_tone_set_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ccord_emoji_tone_sets[mid].entry < idx) lo = mid + 1;
    else hi = mid;
  }
  if (lo < ccord_emoji_tone_set_count && ccord_emoji_tone_sets[lo].entry == idx) return &ccord_emoji_tone_sets[lo];
  return nullptr;
}

size_t ccord_emoji_with_tone(const ccord_emoji_entry *e, int tone, char *out, size_t cap)
{
  if (!e || !out || cap == 0) return 0;
  const char *src = e->utf8;
  if (tone >= 1 && tone <= 5 && e->has_skin_tones && e >= ccord_emoji_data &&
      e < ccord_emoji_data + ccord_emoji_data_count) {
    const ccord_emoji_tone_set *ts = find_tone_set((size_t)(e - ccord_emoji_data));
    if (ts) src = ts->utf8[tone - 1];
  }
  size_t n = strlen(src);
  if (n + 1 > cap) {
    out[0] = '\0';
    return 0;
  }
  memcpy(out, src, n + 1);
  return n;
}

// ---- exact lookups ---------------------------------------------------------------------------------------------

static int cmp_bytes(const char *a, size_t alen, const char *b, size_t blen)
{
  size_t n = alen < blen ? alen : blen;
  int c = n ? memcmp(a, b, n) : 0;
  if (c) return c;
  return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static char lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

// Lower-cases name into buf (' ' becomes '_'); false when empty or too long.
static bool normalise(const char *s, size_t len, char *buf, size_t *outlen)
{
  if (!s || len == 0 || len > MAX_NAME) return false;
  for (size_t i = 0; i < len; i++) buf[i] = s[i] == ' ' ? '_' : lower(s[i]);
  *outlen = len;
  return true;
}

// First index in the shortcode index whose key is >= (s, len).
static size_t sc_lower_bound(const char *s, size_t len)
{
  size_t lo = 0, hi = ccord_emoji_sc_index_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const ccord_emoji_sc_key *k = &ccord_emoji_sc_index[mid];
    if (cmp_bytes(k->key, k->len, s, len) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

const ccord_emoji_entry *ccord_emoji_find_shortcode(const char *name, size_t len)
{
  char buf[MAX_NAME];
  size_t n;
  if (!normalise(name, len, buf, &n)) return nullptr;
  size_t i = sc_lower_bound(buf, n);
  if (i < ccord_emoji_sc_index_count) {
    const ccord_emoji_sc_key *k = &ccord_emoji_sc_index[i];
    if (cmp_bytes(k->key, k->len, buf, n) == 0) return &ccord_emoji_data[k->entry];
  }
  return nullptr;
}

const ccord_emoji_entry *ccord_emoji_find_utf8(const char *s, size_t len, int *out_tone)
{
  if (!s || len == 0) return nullptr;
  size_t lo = 0, hi = ccord_emoji_utf8_index_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const ccord_emoji_utf8_key *k = &ccord_emoji_utf8_index[mid];
    int c = cmp_bytes(k->utf8, k->len, s, len);
    if (c == 0) {
      if (out_tone) *out_tone = k->tone;
      return &ccord_emoji_data[k->entry];
    }
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

// ---- search / completion ---------------------------------------------------------------------------------------

// Case-insensitive helpers; the needle is already normalised, ' ' and '_' are equivalent in the haystack.
static char hay_char(char c)
{
  return c == ' ' ? '_' : lower(c);
}

static bool ci_prefix(const char *hay, size_t hlen, const char *nd, size_t nlen)
{
  if (nlen > hlen) return false;
  for (size_t i = 0; i < nlen; i++)
    if (hay_char(hay[i]) != nd[i]) return false;
  return true;
}

static bool ci_substr(const char *hay, size_t hlen, const char *nd, size_t nlen)
{
  if (nlen > hlen) return false;
  for (size_t s = 0; s + nlen <= hlen; s++)
    if (ci_prefix(hay + s, hlen - s, nd, nlen)) return true;
  return false;
}

// Tiers: 0 exact name/alias, 1 prefix of name, 2 prefix of alias, 3 substring of name/alias, 4 substring of CLDR name.
static unsigned entry_tier(const ccord_emoji_entry *e, const char *q, size_t ql)
{
  size_t nlen = strlen(e->name);
  unsigned best = TIER_NONE;
  if (nlen == ql && ci_prefix(e->name, nlen, q, ql)) return 0;
  if (ci_prefix(e->name, nlen, q, ql)) best = 1;
  else if (ci_substr(e->name, nlen, q, ql)) best = 3;
  if (e->aliases) {
    for (const char *const *a = e->aliases; *a; a++) {
      size_t alen = strlen(*a);
      if (alen == ql && ci_prefix(*a, alen, q, ql)) return 0;
      if (best > 2 && ci_prefix(*a, alen, q, ql)) best = 2;
      else if (best > 3 && ci_substr(*a, alen, q, ql)) best = 3;
    }
  }
  if (best == TIER_NONE && ci_substr(e->cldr_name, strlen(e->cldr_name), q, ql)) best = 4;
  return best;
}

static bool query_prepare(const char *q, size_t ql, char *buf, size_t *outlen)
{
  if (!q) return false;
  while (ql && q[0] == ':') {
    q++;
    ql--;
  }
  while (ql && q[ql - 1] == ':') ql--;
  return normalise(q, ql, buf, outlen);
}

// Emits indices in (tier, dataset order) order from the per-entry tier array.
static size_t emit_ranked(const uint8_t *tier, unsigned ntiers, size_t *out_idx, size_t cap)
{
  size_t n = 0;
  for (unsigned t = 0; t < ntiers && n < cap; t++)
    for (size_t i = 0; i < ccord_emoji_data_count && n < cap; i++)
      if (tier[i] == t) out_idx[n++] = i;
  return n;
}

size_t ccord_emoji_search(const char *query, size_t qlen, size_t *out_idx, size_t cap)
{
  char q[MAX_NAME];
  size_t ql;
  if (!out_idx || cap == 0 || ccord_emoji_data_count > MAX_ENTRIES || !query_prepare(query, qlen, q, &ql)) return 0;
  uint8_t tier[MAX_ENTRIES];
  for (size_t i = 0; i < ccord_emoji_data_count; i++) tier[i] = (uint8_t)entry_tier(&ccord_emoji_data[i], q, ql);
  return emit_ranked(tier, 5, out_idx, cap);
}

size_t ccord_emoji_complete(const char *prefix, size_t plen, size_t *out_idx, size_t cap)
{
  char q[MAX_NAME];
  size_t ql;
  if (!out_idx || cap == 0 || ccord_emoji_data_count > MAX_ENTRIES || !query_prepare(prefix, plen, q, &ql)) return 0;
  uint8_t tier[MAX_ENTRIES];
  memset(tier, TIER_NONE, ccord_emoji_data_count);
  for (size_t i = sc_lower_bound(q, ql); i < ccord_emoji_sc_index_count; i++) {
    const ccord_emoji_sc_key *k = &ccord_emoji_sc_index[i];
    if (k->len < ql || memcmp(k->key, q, ql) != 0) break;
    const ccord_emoji_entry *e = &ccord_emoji_data[k->entry];
    uint8_t t = k->len == ql ? 0 : (strcmp(e->name, k->key) == 0 ? 1 : 2);
    if (t < tier[k->entry]) tier[k->entry] = t;
  }
  return emit_ranked(tier, 3, out_idx, cap);
}

// ---- shortcode replacement -------------------------------------------------------------------------------------

typedef struct writer {
  char *out;
  size_t cap;
  size_t n;
  bool full;
} writer;

// Appends bytes atomically: either all of them fit (keeping room for the NUL) or none, and writing stops for good.
static void w_atomic(writer *w, const char *s, size_t n)
{
  if (w->full) return;
  if (w->n + n + 1 > w->cap) {
    w->full = true;
    return;
  }
  memcpy(w->out + w->n, s, n);
  w->n += n;
}

static size_t utf8_seq_len(const char *s, size_t avail)
{
  unsigned char c = (unsigned char)s[0];
  size_t want = c < 0x80 ? 1 : (c >= 0xF0 && c < 0xF8) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2) ? 2 : 1;
  if (c >= 0xF8) want = 1;
  if (want > avail) return 1;
  for (size_t i = 1; i < want; i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) return 1;
  return want;
}

// Copies input text one whole UTF-8 character at a time so truncation never splits a sequence.
static void w_copy(writer *w, const char *s, size_t n)
{
  size_t i = 0;
  while (i < n && !w->full) {
    size_t l = utf8_seq_len(s + i, n - i);
    w_atomic(w, s + i, l);
    i += l;
  }
}

static bool is_name_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '+' ||
         c == '-';
}

static bool is_digit(char c)
{
  return c >= '0' && c <= '9';
}

// Length of a "<:name:id>" / "<a:name:id>" custom emoji token starting at in[i] == '<', or 0.
static size_t custom_emoji_len(const char *in, size_t len, size_t i)
{
  size_t j = i + 1;
  if (j < len && in[j] == 'a') j++;
  if (j >= len || in[j] != ':') return 0;
  j++;
  size_t name_start = j;
  while (j < len && ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z') || is_digit(in[j]) || in[j] == '_'))
    j++;
  if (j - name_start < 2 || j >= len || in[j] != ':') return 0;
  j++;
  size_t id_start = j;
  while (j < len && is_digit(in[j])) j++;
  if (j == id_start || j >= len || in[j] != '>') return 0;
  return j + 1 - i;
}

// End (exclusive) of a code span opened by a run of n backticks at in[i..i+n), or 0 when it is never closed.
static size_t code_span_end(const char *in, size_t len, size_t i, size_t n)
{
  size_t k = i + n;
  while (k < len) {
    if (in[k] != '`') {
      k++;
      continue;
    }
    size_t run = 0;
    while (k + run < len && in[k + run] == '`') run++;
    if (run == n) return k + run;
    k += run;
  }
  return 0;
}

size_t ccord_emoji_replace_shortcodes(const char *in, size_t len, char *out, size_t cap)
{
  if (!out || cap == 0) return 0;
  writer w = {out, cap, 0, false};
  if (!in) len = 0;
  size_t i = 0;
  while (i < len && !w.full) {
    char c = in[i];
    if (c == '`') {
      size_t run = 0;
      while (i + run < len && in[i + run] == '`') run++;
      size_t end = code_span_end(in, len, i, run);
      if (end == 0) end = i + run;
      w_copy(&w, in + i, end - i);
      i = end;
    } else if (c == '<') {
      size_t l = custom_emoji_len(in, len, i);
      if (l == 0) l = 1;
      w_copy(&w, in + i, l);
      i += l;
    } else if (c == ':') {
      size_t j = i + 1;
      while (j < len && j - i - 1 <= MAX_NAME && is_name_char(in[j])) j++;
      const ccord_emoji_entry *e = nullptr;
      if (j < len && in[j] == ':' && j > i + 1 && j - i - 1 <= MAX_NAME) e = ccord_emoji_find_shortcode(in + i + 1, j - i - 1);
      if (!e) {
        w_atomic(&w, ":", 1);
        i++;
        continue;
      }
      size_t end = j + 1;
      int tone = 0;
      static const char k_skin[] = ":skin-tone-";
      if (e->has_skin_tones && end + sizeof k_skin - 1 + 2 <= len && memcmp(in + end, k_skin, sizeof k_skin - 1) == 0) {
        char d = in[end + sizeof k_skin - 1];
        if (d >= '2' && d <= '6' && in[end + sizeof k_skin] == ':') {
          tone = d - '1';
          end += sizeof k_skin + 1;
        }
      }
      char buf[64];
      size_t n = ccord_emoji_with_tone(e, tone, buf, sizeof buf);
      if (n == 0) n = ccord_emoji_with_tone(e, 0, buf, sizeof buf);
      w_atomic(&w, buf, n);
      i = end;
    } else {
      size_t l = utf8_seq_len(in + i, len - i);
      w_atomic(&w, in + i, l);
      i += l;
    }
  }
  out[w.n] = '\0';
  return w.n;
}
