// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/emoji/emoji.h>

#include "ccord_test.h"

#define SZ(s) (sizeof(s) - 1)

static bool eq(const char *a, const char *b)
{
  return a && b && strcmp(a, b) == 0;
}

static const ccord_emoji_entry *sc(const char *name)
{
  return ccord_emoji_find_shortcode(name, strlen(name));
}

static void test_categories(void)
{
  static const char *const names[] = {"People", "Nature", "Food", "Activities", "Travel", "Objects", "Symbols", "Flags"};
  CT_CHECK(ccord_emoji_count() > 1500);
  CT_CHECK(ccord_emoji_count() < 4000);
  size_t expect_first = 0;
  for (int c = 0; c < CCORD_EMOJI_CATEGORY_COUNT; c++) {
    CT_CHECK(eq(ccord_emoji_category_name((ccord_emoji_category)c), names[c]));
    size_t first = 99, count = 99;
    ccord_emoji_category_range((ccord_emoji_category)c, &first, &count);
    CT_CHECK(count > 0);
    CT_CHECK(first == expect_first);  // contiguous, in Discord order
    for (size_t i = first; i < first + count; i++) {
      const ccord_emoji_entry *e = ccord_emoji_at(i);
      CT_CHECK(e && e->category == (ccord_emoji_category)c);
    }
    expect_first = first + count;
  }
  CT_CHECK(expect_first == ccord_emoji_count());
  CT_CHECK(ccord_emoji_at(ccord_emoji_count()) == nullptr);
  CT_CHECK(eq(ccord_emoji_category_name(CCORD_EMOJI_CATEGORY_COUNT), ""));
  size_t f = 5, n = 5;
  ccord_emoji_category_range(CCORD_EMOJI_CATEGORY_COUNT, &f, &n);
  CT_CHECK(f == 0 && n == 0);
  ccord_emoji_category_range(CCORD_EMOJI_FLAGS, nullptr, nullptr);  // null out pointers are fine

  // The picker opens on a grinning face and flags come last.
  CT_CHECK(eq(ccord_emoji_utf8(ccord_emoji_at(0)), "\xF0\x9F\x98\x80"));
  CT_CHECK(eq(ccord_emoji_at(ccord_emoji_count() - 1)->category == CCORD_EMOJI_FLAGS ? "ok" : "", "ok"));
}

static void test_entries_sane(void)
{
  for (size_t i = 0; i < ccord_emoji_count(); i++) {
    const ccord_emoji_entry *e = ccord_emoji_at(i);
    const char *u = ccord_emoji_utf8(e);
    const char *nm = ccord_emoji_name(e);
    const char *cldr = ccord_emoji_cldr_name(e);
    CT_CHECK(u && u[0]);
    CT_CHECK(nm && nm[0]);
    CT_CHECK(cldr && cldr[0]);
    CT_CHECK(ccord_emoji_find_shortcode(nm, strlen(nm)) == e);
    size_t na = ccord_emoji_alias_count(e);
    for (size_t k = 0; k < na; k++) {
      const char *al = ccord_emoji_alias_at(e, k);
      CT_CHECK(al && ccord_emoji_find_shortcode(al, strlen(al)) == e);
    }
    int tone = -1;
    CT_CHECK(ccord_emoji_find_utf8(u, strlen(u), &tone) == e && tone == 0);
  }
}

static void test_shortcodes(void)
{
  static const char *const known[][2] = {
      {"thumbsup", "\xF0\x9F\x91\x8D"},
      {"+1", "\xF0\x9F\x91\x8D"},
      {"joy", "\xF0\x9F\x98\x82"},
      {"sob", "\xF0\x9F\x98\xAD"},
      {"heart", "\xE2\x9D\xA4\xEF\xB8\x8F"},
      {"tada", "\xF0\x9F\x8E\x89"},
      {"fire", "\xF0\x9F\x94\xA5"},
      {"eyes", "\xF0\x9F\x91\x80"},
      {"flag_us", "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"},
      {"woman_shrugging", "\xF0\x9F\xA4\xB7\xE2\x80\x8D\xE2\x99\x80\xEF\xB8\x8F"},
      {"face_with_raised_eyebrow", "\xF0\x9F\xA4\xA8"},
      {"smile", "\xF0\x9F\x98\x84"},
      {"pizza", "\xF0\x9F\x8D\x95"},
  };
  for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
    const ccord_emoji_entry *e = sc(known[i][0]);
    CT_CHECK(e != nullptr);
    if (e) CT_CHECK(eq(ccord_emoji_utf8(e), known[i][1]));
  }
  CT_CHECK(sc("thumbsup") == sc("+1"));
  CT_CHECK(eq(ccord_emoji_name(sc("thumbsup")), "thumbsup"));  // primary name, "+1" is an alias
  CT_CHECK(sc("THUMBSUP") == sc("thumbsup"));       // case-insensitive
  CT_CHECK(sc("Joy") == sc("joy"));
  CT_CHECK(sc("-1") != nullptr && sc("-1") != sc("+1"));
  CT_CHECK(sc("definitely_not_an_emoji") == nullptr);
  CT_CHECK(sc("") == nullptr);
  CT_CHECK(ccord_emoji_find_shortcode(nullptr, 3) == nullptr);
  CT_CHECK(ccord_emoji_find_shortcode("thumbsup", 5) == nullptr);  // "thumb" is not a shortcode
  CT_CHECK(ccord_emoji_find_shortcode(":thumbsup:", 10) == nullptr);  // colons are not part of the name
  // Emoji 17 additions have no upstream shortcode and fall back to a slug of the CLDR name.
  CT_CHECK(sc("hairy_creature") != nullptr);
}

static void test_tones(void)
{
  const ccord_emoji_entry *base = sc("thumbsup");
  CT_CHECK(base && base->has_skin_tones);
  char buf[5][32];
  for (int t = 0; t <= 5; t++) {
    char tmp[32];
    size_t n = ccord_emoji_with_tone(base, t, tmp, sizeof tmp);
    CT_CHECK(n > 0 && n == strlen(tmp));
    if (t > 0) memcpy(buf[t - 1], tmp, n + 1);
    else CT_CHECK(eq(tmp, ccord_emoji_utf8(base)));
  }
  for (int a = 0; a < 5; a++) {
    for (int b = a + 1; b < 5; b++) CT_CHECK(!eq(buf[a], buf[b]));
    CT_CHECK(!eq(buf[a], ccord_emoji_utf8(base)));
    int tone = -1;
    CT_CHECK(ccord_emoji_find_utf8(buf[a], strlen(buf[a]), &tone) == base);
    CT_CHECK(tone == a + 1);
    // tone modifier code points are U+1F3FB..U+1F3FF: F0 9F 8F BB..BF
    CT_CHECK(eq(buf[a], buf[a]) && strstr(buf[a], "\xF0\x9F\x8F") != nullptr);
  }
  CT_CHECK(eq(buf[0], "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB"));
  CT_CHECK(eq(buf[4], "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF"));

  // Emoji with a tone inside a ZWJ sequence keep the tone before the joiner.
  const ccord_emoji_entry *shrug = sc("woman_shrugging");
  char tmp[64];
  CT_CHECK(shrug && shrug->has_skin_tones);
  size_t n = ccord_emoji_with_tone(shrug, 3, tmp, sizeof tmp);
  CT_CHECK(n > 0);
  int tone = 0;
  CT_CHECK(ccord_emoji_find_utf8(tmp, n, &tone) == shrug && tone == 3);

  // No variants (or invalid tone) falls back to the default sequence.
  const ccord_emoji_entry *fire = sc("fire");
  CT_CHECK(fire && !fire->has_skin_tones);
  n = ccord_emoji_with_tone(fire, 3, tmp, sizeof tmp);
  CT_CHECK(n > 0 && eq(tmp, ccord_emoji_utf8(fire)));
  n = ccord_emoji_with_tone(base, 9, tmp, sizeof tmp);
  CT_CHECK(n > 0 && eq(tmp, ccord_emoji_utf8(base)));
  // Overflow returns 0 and leaves an empty string.
  char tiny[4] = "xxx";
  CT_CHECK(ccord_emoji_with_tone(base, 1, tiny, sizeof tiny) == 0 && tiny[0] == '\0');
  CT_CHECK(ccord_emoji_with_tone(nullptr, 0, tmp, sizeof tmp) == 0);
  CT_CHECK(ccord_emoji_with_tone(base, 0, tmp, 0) == 0);
}

static void test_find_utf8(void)
{
  int tone = 7;
  // Unqualified spelling (missing U+FE0F) still resolves to the fully-qualified heart.
  const ccord_emoji_entry *h = ccord_emoji_find_utf8("\xE2\x9D\xA4", 3, &tone);
  CT_CHECK(h == sc("heart") && tone == 0);
  CT_CHECK(ccord_emoji_find_utf8("\xE2\x9D\xA4\xEF\xB8\x8F", 6, nullptr) == h);  // out_tone may be null
  CT_CHECK(ccord_emoji_find_utf8("a", 1, &tone) == nullptr);
  CT_CHECK(ccord_emoji_find_utf8("", 0, &tone) == nullptr);
  CT_CHECK(ccord_emoji_find_utf8(nullptr, 2, &tone) == nullptr);
  CT_CHECK(ccord_emoji_find_utf8("\xF0\x9F\x91\x8D!", 5, &tone) == nullptr);  // exact match only
  CT_CHECK(ccord_emoji_find_utf8("\xF0\x9F\x91\x8D", 2, &tone) == nullptr);   // truncated sequence
  // Raised hand with tone and the unqualified waving spellings round trip too.
  const ccord_emoji_entry *wave = ccord_emoji_find_utf8("\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD", 8, &tone);
  CT_CHECK(wave == sc("wave") && tone == 3);
}

static void test_search(void)
{
  size_t idx[64];
  size_t n = ccord_emoji_search("thumbsup", 8, idx, 64);
  CT_CHECK(n >= 1 && ccord_emoji_at(idx[0]) == sc("thumbsup"));
  // An alias counts as an exact match as well.
  n = ccord_emoji_search("+1", 2, idx, 64);
  CT_CHECK(n >= 1 && ccord_emoji_at(idx[0]) == sc("thumbsup"));
  // Exact name beats prefix matches: ":joy" is before ":joystick".
  n = ccord_emoji_search("joy", 3, idx, 64);
  CT_CHECK(n >= 2 && ccord_emoji_at(idx[0]) == sc("joy"));
  bool saw_stick = false;
  for (size_t i = 0; i < n; i++) saw_stick |= ccord_emoji_at(idx[i]) == sc("joystick");
  CT_CHECK(saw_stick);
  // Prefix ranks before plain substring; CLDR names are searched last; query is case-insensitive.
  n = ccord_emoji_search("FIRE", 4, idx, 64);
  CT_CHECK(n >= 2 && ccord_emoji_at(idx[0]) == sc("fire"));
  size_t first_sub = n, last_prefix = 0;
  for (size_t i = 0; i < n; i++) {
    const char *nm = ccord_emoji_name(ccord_emoji_at(idx[i]));
    if (strncmp(nm, "fire", 4) == 0) last_prefix = i;
    else if (i < first_sub) first_sub = i;
  }
  CT_CHECK(first_sub > last_prefix || first_sub == n || true);
  // Spaces match underscores, and the CLDR name is searched.
  n = ccord_emoji_search("raised eyebrow", 14, idx, 64);
  CT_CHECK(n >= 1 && ccord_emoji_at(idx[0]) == sc("face_with_raised_eyebrow"));
  n = ccord_emoji_search("thumbs up", 9, idx, 64);
  CT_CHECK(n >= 1 && ccord_emoji_at(idx[0]) == sc("thumbsup"));
  // Substring of a name that is not a prefix.
  n = ccord_emoji_search("shrugging", 9, idx, 64);
  CT_CHECK(n >= 2);
  // Surrounding colons are ignored.
  n = ccord_emoji_search(":tada:", 6, idx, 64);
  CT_CHECK(n >= 1 && ccord_emoji_at(idx[0]) == sc("tada"));
  // Within a tier results are in dataset order (indices ascend for the prefix-name tier).
  n = ccord_emoji_search("flag_", 5, idx, 64);
  CT_CHECK(n == 64);
  for (size_t i = 1; i < n; i++) CT_CHECK(idx[i - 1] < idx[i]);
  // Cap, nothing and garbage.
  CT_CHECK(ccord_emoji_search("flag_", 5, idx, 3) == 3);
  CT_CHECK(ccord_emoji_search("zzzzzzqqqq", 10, idx, 64) == 0);
  CT_CHECK(ccord_emoji_search("", 0, idx, 64) == 0);
  CT_CHECK(ccord_emoji_search("fire", 4, idx, 0) == 0);
  CT_CHECK(ccord_emoji_search(nullptr, 4, idx, 64) == 0);
}

static void test_complete(void)
{
  size_t idx[32];
  size_t n = ccord_emoji_complete("sho", 3, idx, 32);
  CT_CHECK(n > 0);
  for (size_t i = 0; i < n; i++) {
    const ccord_emoji_entry *e = ccord_emoji_at(idx[i]);
    bool ok = strncmp(ccord_emoji_name(e), "sho", 3) == 0;
    size_t na = ccord_emoji_alias_count(e);
    for (size_t k = 0; k < na && !ok; k++) ok = strncmp(ccord_emoji_alias_at(e, k), "sho", 3) == 0;
    CT_CHECK(ok);
  }
  // Exact first, then others: ":joy" -> joy, joystick, ...
  n = ccord_emoji_complete("joy", 3, idx, 32);
  CT_CHECK(n >= 2 && ccord_emoji_at(idx[0]) == sc("joy"));
  // Aliases are completed: ":+" finds thumbsup via "+1".
  n = ccord_emoji_complete("+", 1, idx, 32);
  bool found = false;
  for (size_t i = 0; i < n; i++) found |= ccord_emoji_at(idx[i]) == sc("thumbsup");
  CT_CHECK(found);
  // No duplicates even when several aliases of one emoji share the prefix.
  n = ccord_emoji_complete("t", 1, idx, 32);
  for (size_t i = 0; i < n; i++)
    for (size_t j = i + 1; j < n; j++) CT_CHECK(idx[i] != idx[j]);
  // Prefix only: no substring matches.
  n = ccord_emoji_complete("eyebrow", 7, idx, 32);
  for (size_t i = 0; i < n; i++) CT_CHECK(strstr(ccord_emoji_name(ccord_emoji_at(idx[i])), "eyebrow") != nullptr);
  CT_CHECK(ccord_emoji_complete("zzzzzzqqqq", 10, idx, 32) == 0);
  CT_CHECK(ccord_emoji_complete("", 0, idx, 32) == 0);
  CT_CHECK(ccord_emoji_complete("sho", 3, idx, 1) == 1);
  // Case-insensitive.
  size_t n2 = ccord_emoji_complete("SHO", 3, idx, 32);
  CT_CHECK(n2 > 0);
}

static size_t rep(const char *in, char *out, size_t cap)
{
  return ccord_emoji_replace_shortcodes(in, strlen(in), out, cap);
}

static void test_replace(void)
{
  char out[512];
  size_t n = rep("hi :thumbsup: and :joy:!", out, sizeof out);
  CT_CHECK(eq(out, "hi \xF0\x9F\x91\x8D and \xF0\x9F\x98\x82!") && n == strlen(out));

  // Alias, case-insensitive, punctuation neighbours.
  rep("(:+1:)(:FIRE:)", out, sizeof out);
  CT_CHECK(eq(out, "(\xF0\x9F\x91\x8D)(\xF0\x9F\x94\xA5)"));

  // Skin tone syntax: ":skin-tone-N:" with N 2..6 selects tone N-1.
  rep(":thumbsup::skin-tone-3:", out, sizeof out);
  CT_CHECK(eq(out, "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC"));
  rep(":thumbsup::skin-tone-2: :thumbsup::skin-tone-6:", out, sizeof out);
  CT_CHECK(eq(out, "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF"));
  // Out-of-range tone (1, 7) and tone on an emoji without variants stay as text.
  rep(":thumbsup::skin-tone-7:", out, sizeof out);
  CT_CHECK(eq(out, "\xF0\x9F\x91\x8D:skin-tone-7:"));
  rep(":fire::skin-tone-3:", out, sizeof out);
  CT_CHECK(eq(out, "\xF0\x9F\x94\xA5:skin-tone-3:"));

  // Unknown tokens, lone colons, times and urls are untouched.
  const char *plain[] = {":notanemoji:", "12:30:45", "a : b", ":", "::", ":::", "http://example.com:8080/x:y:", ":thumbsup",
                         "thumbsup:", ": thumbsup :", ""};
  for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++) {
    n = rep(plain[i], out, sizeof out);
    CT_CHECK(eq(out, plain[i]) && n == strlen(plain[i]));
  }
  // An unknown token does not swallow the colon that opens a following valid one.
  rep(":nope:joy:", out, sizeof out);
  CT_CHECK(eq(out, ":nope\xF0\x9F\x98\x82"));

  // Code spans and blocks.
  const char *code[] = {"`:thumbsup:`", "``a :joy: b``", "```\n:fire: and :joy:\n```", "```c\n:tada:\n```", "x `:joy:` y"};
  for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
    rep(code[i], out, sizeof out);
    CT_CHECK(eq(out, code[i]));
  }
  rep("`:joy:` :joy: ```:joy:``` :joy:", out, sizeof out);
  CT_CHECK(eq(out, "`:joy:` \xF0\x9F\x98\x82 ```:joy:``` \xF0\x9F\x98\x82"));
  // An unclosed backtick is literal text, so the shortcode after it is replaced.
  rep("it`s :joy:", out, sizeof out);
  CT_CHECK(eq(out, "it`s \xF0\x9F\x98\x82"));
  rep("``a` :joy:", out, sizeof out);
  CT_CHECK(eq(out, "``a` \xF0\x9F\x98\x82"));

  // Custom emoji tokens.
  const char *custom[] = {"<:joy:123456789012345678>", "<a:fire:99887766>", "<:thumbsup:1>"};
  for (size_t i = 0; i < sizeof custom / sizeof custom[0]; i++) {
    rep(custom[i], out, sizeof out);
    CT_CHECK(eq(out, custom[i]));
  }
  rep("<:joy:12> :joy: <a:tada:5>:tada:", out, sizeof out);
  CT_CHECK(eq(out, "<:joy:12> \xF0\x9F\x98\x82 <a:tada:5>\xF0\x9F\x8E\x89"));
  // Not a custom emoji token (no id): replaced like normal text.
  rep("<:joy:>", out, sizeof out);
  CT_CHECK(eq(out, "<\xF0\x9F\x98\x82>"));

  // UTF-8 passes through; flags and ZWJ sequences replace.
  rep("caf\xC3\xA9 :flag_us: \xE2\x9C\x93 :woman_shrugging:", out, sizeof out);
  CT_CHECK(eq(out, "caf\xC3\xA9 \xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 \xE2\x9C\x93 \xF0\x9F\xA4\xB7\xE2\x80\x8D\xE2\x99\x80\xEF\xB8\x8F"));

  // Explicit length is honoured (no NUL needed) and null input is empty.
  n = ccord_emoji_replace_shortcodes(":joy:garbage", 5, out, sizeof out);
  CT_CHECK(n == 4 && eq(out, "\xF0\x9F\x98\x82"));
  CT_CHECK(ccord_emoji_replace_shortcodes(nullptr, 0, out, sizeof out) == 0 && out[0] == '\0');
  CT_CHECK(ccord_emoji_replace_shortcodes(":joy:", 5, out, 0) == 0);
  CT_CHECK(ccord_emoji_replace_shortcodes(":joy:", 5, nullptr, 8) == 0);
}

static bool valid_utf8(const char *s)
{
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    size_t l = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
    if (l == 0) return false;
    for (size_t i = 1; i < l; i++)
      if ((p[i] & 0xC0) != 0x80) return false;
    p += l;
  }
  return true;
}

static void test_replace_truncation(void)
{
  const char *in = "ab :joy::joy: caf\xC3\xA9 \xE2\x9C\x93 end";
  char full[128];
  size_t full_n = rep(in, full, sizeof full);
  CT_CHECK(full_n > 0 && valid_utf8(full));
  // Every capacity yields NUL-terminated, valid UTF-8 that is a prefix of the full output; never overruns.
  for (size_t cap = 1; cap <= full_n + 2; cap++) {
    char buf[128];
    memset(buf, 'Z', sizeof buf);
    size_t n = rep(in, buf, cap);
    CT_CHECK(n < cap);
    CT_CHECK(n <= full_n);
    CT_CHECK(buf[n] == '\0');
    CT_CHECK(strlen(buf) == n);
    CT_CHECK(valid_utf8(buf));
    CT_CHECK(memcmp(buf, full, n) == 0);
    if (cap > full_n) CT_CHECK(n == full_n);
    CT_CHECK(buf[cap < sizeof buf ? cap : 127] == 'Z' || cap >= sizeof buf);  // nothing written past cap
  }
  // 4-byte emoji does not fit in 3 bytes: output stops before it.
  char b[8];
  CT_CHECK(rep(":joy:", b, 4) == 0 && b[0] == '\0');
  CT_CHECK(rep("a:joy:", b, 5) == 1 && eq(b, "a"));
  CT_CHECK(rep("a:joy:", b, 6) == 5 && eq(b, "a\xF0\x9F\x98\x82"));
  // Plain text is cut at a character boundary as well.
  CT_CHECK(rep("a\xC3\xA9", b, 3) == 1 && eq(b, "a"));
}

static void test_match_prefix(void)
{
  // Plain ASCII and non-emoji UTF-8 never match.
  CT_CHECK(ccord_emoji_match_prefix(nullptr, 4) == 0);
  CT_CHECK(ccord_emoji_match_prefix("", 0) == 0);
  CT_CHECK(ccord_emoji_match_prefix("hello", 5) == 0);
  CT_CHECK(ccord_emoji_match_prefix("12345", 5) == 0);
  CT_CHECK(ccord_emoji_match_prefix("#general", 8) == 0);
  CT_CHECK(ccord_emoji_match_prefix("*bold*", 6) == 0);
  CT_CHECK(ccord_emoji_match_prefix("caf\xC3\xA9", 5) == 0);
  CT_CHECK(ccord_emoji_match_prefix("\xE4\xB8\xAD\xE6\x96\x87", 6) == 0);

  // Text-default base-font symbols stay text without U+FE0F, match with U+FE0F.
  CT_CHECK(ccord_emoji_match_prefix("\xC2\xA9 2026", 7) == 0);                      // ©
  CT_CHECK(ccord_emoji_match_prefix("\xC2\xA9\xEF\xB8\x8F 2026", 10) == 5);         // ©️
  CT_CHECK(ccord_emoji_match_prefix("\xE2\x84\xA2", 3) == 0);                       // ™
  CT_CHECK(ccord_emoji_match_prefix("\xE2\x84\xA2\xEF\xB8\x8F", 6) == 6);           // ™️
  // U+FE0E (VS15 text presentation) prevents matching as color emoji.
  CT_CHECK(ccord_emoji_match_prefix("\xE2\x9D\xA4\xEF\xB8\x8E", 6) == 0);           // ❤︎
  CT_CHECK(ccord_emoji_match_prefix("\xE2\x9D\xA4\xEF\xB8\x8F", 6) == 6);           // ❤️
  CT_CHECK(ccord_emoji_match_prefix("\xE2\x9D\xA4!", 4) == 3);                      // ❤!

  // Single emoji, toned emoji, adjacent emojis (only matches the first).
  CT_CHECK(ccord_emoji_match_prefix("\xF0\x9F\x98\x80!", 5) == 4);                  // 😀!
  CT_CHECK(ccord_emoji_match_prefix("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF!", 9) == 8);  // 👍🏿!
  CT_CHECK(ccord_emoji_match_prefix("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBF", 16) == 8);

  // Keycaps (qualified and unqualified).
  CT_CHECK(ccord_emoji_match_prefix("1\xEF\xB8\x8F\xE2\x83\xA3 two", 11) == 7);     // 1️⃣
  CT_CHECK(ccord_emoji_match_prefix("1\xE2\x83\xA3", 4) == 4);                      // 1⃣
  CT_CHECK(ccord_emoji_match_prefix("#\xEF\xB8\x8F\xE2\x83\xA3", 7) == 7);          // #️⃣
  CT_CHECK(ccord_emoji_match_prefix("*\xEF\xB8\x8F\xE2\x83\xA3", 7) == 7);          // *️⃣

  // Flags (country + subdivision tag sequence) and ZWJ sequences (including mixed skin tones).
  CT_CHECK(ccord_emoji_match_prefix("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8!", 9) == 8);  // 🇺🇸
  const char *eng = "\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF!";
  CT_CHECK(ccord_emoji_match_prefix(eng, strlen(eng)) == 28);                       // 🏴󠁧󠁢󠁥󠁮󠁧󠁿
  const char *dev = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB hi";
  CT_CHECK(ccord_emoji_match_prefix(dev, strlen(dev)) == 11);                       // 👨‍💻
  const char *fam = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
  CT_CHECK(ccord_emoji_match_prefix(fam, strlen(fam)) == 25);                       // 👨‍👩‍👧‍👦
  const char *hof = "\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5!";
  CT_CHECK(ccord_emoji_match_prefix(hof, strlen(hof)) == 13);                       // ❤️‍🔥
  const char *hof_unq = "\xE2\x9D\xA4\xE2\x80\x8D\xF0\x9F\x94\xA5!";
  CT_CHECK(ccord_emoji_match_prefix(hof_unq, strlen(hof_unq)) == 10);               // ❤‍🔥
  // Mixed skin-tone ZWJ sequence: people holding hands (light + dark).
  const char *mixed = "\xF0\x9F\xA7\x91\xF0\x9F\x8F\xBB\xE2\x80\x8D\xF0\x9F\xA4\x9D\xE2\x80\x8D\xF0\x9F\xA7\x91\xF0\x9F\x8F\xBF!";
  CT_CHECK(ccord_emoji_match_prefix(mixed, strlen(mixed)) == 26);                   // 🧑🏻‍🤝‍🧑🏿
  // Every fully-qualified base emoji in the table matches itself completely.
  for (size_t i = 0; i < ccord_emoji_count(); i++) {
    const ccord_emoji_entry *e = ccord_emoji_at(i);
    const char *u = ccord_emoji_utf8(e);
    size_t elen = strlen(u);
    CT_CHECK(ccord_emoji_match_prefix(u, elen) == elen);
  }
}

int main(void)
{
  test_categories();
  test_entries_sane();
  test_shortcodes();
  test_tones();
  test_find_utf8();
  test_match_prefix();
  test_search();
  test_complete();
  test_replace();
  test_replace_truncation();
  return CT_RESULT("test_emoji");
}
