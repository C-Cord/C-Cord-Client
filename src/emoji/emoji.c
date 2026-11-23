// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/emoji/emoji.h>

#include <string.h>

#include "emoji_data.h"

enum {
  MAX_NAME = 64,          // longest shortcode / query we are willing to look at
  FAST_CAP = 128,         // per-tier stack bucket capacity for single-pass search/complete
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

const char *ccord_emoji_utf8(const ccord_emoji_entry *e)
{
  return e ? ccord_emoji_strtab + e->utf8_off : "";
}

const char *ccord_emoji_name(const ccord_emoji_entry *e)
{
  return e ? ccord_emoji_strtab + e->name_off : "";
}

const char *ccord_emoji_cldr_name(const ccord_emoji_entry *e)
{
  return e ? ccord_emoji_strtab + e->cldr_name_off : "";
}

size_t ccord_emoji_alias_count(const ccord_emoji_entry *e)
{
  return e ? e->alias_count : 0;
}

const char *ccord_emoji_alias_at(const ccord_emoji_entry *e, size_t idx)
{
  if (!e || idx >= e->alias_count) return nullptr;
  size_t pos = (size_t)e->alias_start + idx;
  return pos < ccord_emoji_alias_offs_count ? ccord_emoji_strtab + ccord_emoji_alias_offs[pos] : nullptr;
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
  const char *src = ccord_emoji_utf8(e);
  if (tone >= 1 && tone <= 5 && e->has_skin_tones && e >= ccord_emoji_data &&
      e < ccord_emoji_data + ccord_emoji_data_count) {
    const ccord_emoji_tone_set *ts = find_tone_set((size_t)(e - ccord_emoji_data));
    if (ts) src = ccord_emoji_strtab + ts->utf8_off[tone - 1];
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
    if (cmp_bytes(ccord_emoji_strtab + k->key_off, k->len, s, len) < 0) lo = mid + 1;
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
    if (cmp_bytes(ccord_emoji_strtab + k->key_off, k->len, buf, n) == 0) return &ccord_emoji_data[k->entry];
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
    int c = cmp_bytes(ccord_emoji_strtab + k->utf8_off, k->len, s, len);
    if (c == 0) {
      if (out_tone) *out_tone = k->tone;
      return &ccord_emoji_data[k->entry];
    }
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

static size_t utf8_seq_len(const char *s, size_t avail);

static bool utf8_has_prefix_entry(const char *s, size_t len)
{
  size_t lo = 0, hi = ccord_emoji_utf8_index_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const ccord_emoji_utf8_key *k = &ccord_emoji_utf8_index[mid];
    if (cmp_bytes(ccord_emoji_strtab + k->utf8_off, k->len, s, len) < 0) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= ccord_emoji_utf8_index_count) return false;
  const ccord_emoji_utf8_key *k = &ccord_emoji_utf8_index[lo];
  return k->len >= len && memcmp(ccord_emoji_strtab + k->utf8_off, s, len) == 0;
}

static bool is_skin_tone_utf8(const char *s, size_t l)
{
  return l == 4 && (unsigned char)s[0] == 0xF0 && (unsigned char)s[1] == 0x9F &&
         (unsigned char)s[2] == 0x8F && (unsigned char)s[3] >= 0xBB && (unsigned char)s[3] <= 0xBF;
}

static bool is_zwj_utf8(const char *s, size_t l)
{
  return l == 3 && (unsigned char)s[0] == 0xE2 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x8D;
}

// Text-default symbols covered by the UI base font (©, ®, ‼, ⁉, ™, ℹ): without U+FE0F they stay text.
static bool is_bare_text_symbol(const char *s, size_t len)
{
  if (len == 2 && (unsigned char)s[0] == 0xC2) {
    unsigned char b1 = (unsigned char)s[1];
    return b1 == 0xA9 || b1 == 0xAE;   // © (U+00A9), ® (U+00AE)
  }
  if (len == 3 && (unsigned char)s[0] == 0xE2) {
    unsigned char b1 = (unsigned char)s[1], b2 = (unsigned char)s[2];
    return (b1 == 0x80 && b2 == 0xBC) || (b1 == 0x81 && b2 == 0x89) ||   // ‼ (U+203C), ⁉ (U+2049)
           (b1 == 0x84 && (b2 == 0xA2 || b2 == 0xB9));                   // ™ (U+2122), ℹ (U+2139)
  }
  return false;
}

size_t ccord_emoji_match_prefix(const char *s, size_t len)
{
  if (!s || len == 0) return 0;
  unsigned char b0 = (unsigned char)s[0];
  if (b0 < 0x80) {
    // Only ASCII keycap starters ('0'..'9', '#', '*') followed by optional U+FE0F and U+20E3.
    if (!((b0 >= '0' && b0 <= '9') || b0 == '#' || b0 == '*')) return 0;
    if (len >= 7 && (unsigned char)s[1] == 0xEF && (unsigned char)s[2] == 0xB8 && (unsigned char)s[3] == 0x8F &&
        (unsigned char)s[4] == 0xE2 && (unsigned char)s[5] == 0x83 && (unsigned char)s[6] == 0xA3) {
      return 7;
    }
    if (len >= 4 && (unsigned char)s[1] == 0xE2 && (unsigned char)s[2] == 0x83 && (unsigned char)s[3] == 0xA3) {
      return 4;
    }
    return 0;
  }
  if (b0 != 0xC2 && b0 != 0xE2 && b0 != 0xE3 && b0 != 0xF0) return 0;

  char norm[64];
  size_t nlen = 0;
  size_t pos = 0;
  size_t best = 0;
  size_t prev_start = 0, prev_len = 0;
  bool has_tone = false, has_zwj = false, norm_ok = true;

  while (pos < len && pos < 64) {
    size_t cl = utf8_seq_len(s + pos, len - pos);
    if (pos + cl > 64) break;
    bool tone = is_skin_tone_utf8(s + pos, cl);
    if (tone) {
      // Only allow a skin tone in the tone-stripped ZWJ path immediately after a toneable base emoji.
      const ccord_emoji_entry *pe = prev_len > 0 ? ccord_emoji_find_utf8(s + prev_start, prev_len, nullptr) : nullptr;
      if (!pe || !pe->has_skin_tones) norm_ok = false;
      has_tone = true;
    } else {
      if (is_zwj_utf8(s + pos, cl)) has_zwj = true;
      if (norm_ok && nlen + cl <= sizeof norm) {
        memcpy(norm + nlen, s + pos, cl);
        nlen += cl;
      } else {
        norm_ok = false;
      }
    }
    prev_start = pos;
    prev_len = cl;
    pos += cl;

    if (ccord_emoji_find_utf8(s, pos, nullptr) != nullptr) {
      best = pos;
    } else if (norm_ok && has_tone && has_zwj && nlen > 0 && ccord_emoji_find_utf8(norm, nlen, nullptr) != nullptr) {
      best = pos;
    }

    bool more = utf8_has_prefix_entry(s, pos);
    if (!more && norm_ok && has_tone && nlen > 0) more = utf8_has_prefix_entry(norm, nlen);
    if (!more) break;
  }

  if (best == 0) return 0;
  // U+FE0E (VS15) explicitly requests text presentation rather than color emoji.
  if (best + 3 <= len && (unsigned char)s[best] == 0xEF && (unsigned char)s[best + 1] == 0xB8 &&
      (unsigned char)s[best + 2] == 0x8E) {
    return 0;
  }
  if (is_bare_text_symbol(s, best)) return 0;
  return best;
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
static unsigned entry_tier(const ccord_emoji_entry *e, const char *q, size_t ql, unsigned max_tier)
{
  const char *name = ccord_emoji_name(e);
  size_t nlen = strlen(name);
  unsigned best = TIER_NONE;
  if (nlen == ql && ci_prefix(name, nlen, q, ql)) return 0;
  if (max_tier >= 1 && ci_prefix(name, nlen, q, ql)) best = 1;
  else if (max_tier >= 3 && ci_substr(name, nlen, q, ql)) best = 3;
  size_t na = ccord_emoji_alias_count(e);
  for (size_t ai = 0; ai < na; ai++) {
    const char *a = ccord_emoji_alias_at(e, ai);
    size_t alen = strlen(a);
    if (alen == ql && ci_prefix(a, alen, q, ql)) return 0;
    if (max_tier >= 2 && best > 2 && ci_prefix(a, alen, q, ql)) best = 2;
    else if (max_tier >= 3 && best > 3 && ci_substr(a, alen, q, ql)) best = 3;
  }
  const char *cldr = ccord_emoji_cldr_name(e);
  if (best == TIER_NONE && max_tier >= 4 && ci_substr(cldr, strlen(cldr), q, ql)) best = 4;
  return best <= max_tier ? best : TIER_NONE;
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

static bool str_has_prefix(const char *s, const char *q, size_t ql)
{
  for (size_t i = 0; i < ql; i++) {
    if (s[i] != q[i]) return false;
  }
  return true;
}

// Inserts `val` into sorted ascending `arr[0..*n)` capped at `cap` (deduplicating).
static void insert_sorted_u16(uint16_t *arr, size_t *n, size_t cap, uint16_t val)
{
  if (cap == 0) return;
  size_t cnt = *n;
  if (cnt > cap) cnt = cap;
  if (cnt == cap && val >= arr[cnt - 1u]) {
    *n = cnt;
    return;
  }
  size_t lo = 0, hi = cnt;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (arr[mid] < val) lo = mid + 1u;
    else hi = mid;
  }
  if (lo < cnt && arr[lo] == val) {
    *n = cnt;
    return;
  }
  size_t new_cnt = cnt < cap ? cnt + 1u : cap;
  for (size_t j = new_cnt - 1u; j > lo; j--) arr[j] = arr[j - 1u];
  arr[lo] = val;
  *n = new_cnt;
}

static void insert_sorted_sz(size_t *arr, size_t *n, size_t cap, size_t val)
{
  if (cap == 0) return;
  size_t cnt = *n;
  if (cnt == cap && val >= arr[cnt - 1u]) return;
  size_t lo = 0, hi = cnt;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (arr[mid] < val) lo = mid + 1u;
    else hi = mid;
  }
  if (lo < cnt && arr[lo] == val) return;
  size_t new_cnt = cnt < cap ? cnt + 1u : cap;
  for (size_t j = new_cnt - 1u; j > lo; j--) arr[j] = arr[j - 1u];
  arr[lo] = val;
  *n = new_cnt;
}

size_t ccord_emoji_search(const char *query, size_t qlen, size_t *out_idx, size_t cap)
{
  char q[MAX_NAME];
  size_t ql;
  if (!out_idx || cap == 0 || !query_prepare(query, qlen, q, &ql)) return 0;
  size_t want = cap < ccord_emoji_data_count ? cap : ccord_emoji_data_count;
  if (want <= FAST_CAP) {
    uint16_t buckets[5][FAST_CAP];
    size_t counts[5] = {0};
    unsigned max_tier = 4;
    bool done = false;
    for (size_t i = 0; i < ccord_emoji_data_count && !done; i++) {
      unsigned t = entry_tier(&ccord_emoji_data[i], q, ql, max_tier);
      if (t > max_tier) continue;
      buckets[t][counts[t]++] = (uint16_t)i;
      size_t acc = 0;
      for (unsigned nt = 0; nt <= max_tier; nt++) {
        acc += counts[nt];
        if (acc >= want) {
          if (nt == 0) done = true;
          else max_tier = nt - 1u;
          break;
        }
      }
    }
    size_t n = 0;
    for (unsigned t = 0; t < 5 && n < want; t++)
      for (size_t k = 0; k < counts[t] && n < want; k++) out_idx[n++] = buckets[t][k];
    return n;
  }
  size_t n = 0;
  for (unsigned target = 0; target < 5 && n < want; target++) {
    for (size_t i = 0; i < ccord_emoji_data_count && n < want; i++) {
      if (entry_tier(&ccord_emoji_data[i], q, ql, target) == target) out_idx[n++] = i;
    }
  }
  return n;
}

size_t ccord_emoji_complete(const char *prefix, size_t plen, size_t *out_idx, size_t cap)
{
  char q[MAX_NAME];
  size_t ql;
  if (!out_idx || cap == 0 || !query_prepare(prefix, plen, q, &ql)) return 0;
  size_t want = cap < ccord_emoji_data_count ? cap : ccord_emoji_data_count;
  size_t start = sc_lower_bound(q, ql);

  bool has_exact = false;
  uint16_t exact_entry = 0;
  size_t i0 = start;
  if (i0 < ccord_emoji_sc_index_count) {
    const ccord_emoji_sc_key *k = &ccord_emoji_sc_index[i0];
    if (k->len == ql && memcmp(ccord_emoji_strtab + k->key_off, q, ql) == 0) {
      has_exact = true;
      exact_entry = k->entry;
      i0++;
    }
  }
  size_t cap1 = want - (has_exact ? 1u : 0u);
  if (cap1 == 0) {
    if (has_exact) {
      out_idx[0] = exact_entry;
      return 1;
    }
    return 0;
  }

  if (want <= FAST_CAP) {
    uint16_t t1[FAST_CAP];
    uint16_t t2[FAST_CAP];
    size_t n1 = 0, n2 = 0;
    for (size_t i = i0; i < ccord_emoji_sc_index_count; i++) {
      const ccord_emoji_sc_key *k = &ccord_emoji_sc_index[i];
      const char *key = ccord_emoji_strtab + k->key_off;
      if (k->len < ql || memcmp(key, q, ql) != 0) break;
      if (has_exact && k->entry == exact_entry) continue;
      const ccord_emoji_entry *e = &ccord_emoji_data[k->entry];
      const char *ename = ccord_emoji_name(e);
      if (ename == key || strcmp(ename, key) == 0) {
        insert_sorted_u16(t1, &n1, cap1, k->entry);
      } else if (n1 < cap1 && !str_has_prefix(ename, q, ql)) {
        insert_sorted_u16(t2, &n2, cap1 - n1, k->entry);
      }
    }
    size_t n = 0;
    if (has_exact) out_idx[n++] = exact_entry;
    for (size_t k = 0; k < n1 && n < want; k++) out_idx[n++] = t1[k];
    for (size_t k = 0; k < n2 && n < want; k++) out_idx[n++] = t2[k];
    return n;
  }

  size_t n = 0;
  if (has_exact) out_idx[n++] = exact_entry;
  size_t n1 = 0;
  size_t end = i0;
  for (size_t i = i0; i < ccord_emoji_sc_index_count; i++) {
    const ccord_emoji_sc_key *k = &ccord_emoji_sc_index[i];
    const char *key = ccord_emoji_strtab + k->key_off;
    if (k->len < ql || memcmp(key, q, ql) != 0) break;
    end = i + 1u;
    if (has_exact && k->entry == exact_entry) continue;
    const ccord_emoji_entry *e = &ccord_emoji_data[k->entry];
    const char *ename = ccord_emoji_name(e);
    if (ename == key || strcmp(ename, key) == 0) {
      insert_sorted_sz(out_idx + n, &n1, cap1, k->entry);
    }
  }
  n += n1;
  if (n < want) {
    size_t cap2 = want - n;
    size_t n2 = 0;
    for (size_t i = i0; i < end; i++) {
      const ccord_emoji_sc_key *k = &ccord_emoji_sc_index[i];
      const char *key = ccord_emoji_strtab + k->key_off;
      if (has_exact && k->entry == exact_entry) continue;
      const ccord_emoji_entry *e = &ccord_emoji_data[k->entry];
      const char *ename = ccord_emoji_name(e);
      if (ename != key && strcmp(ename, key) != 0 && !str_has_prefix(ename, q, ql)) {
        insert_sorted_sz(out_idx + n, &n2, cap2, k->entry);
      }
    }
    n += n2;
  }
  return n;
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
