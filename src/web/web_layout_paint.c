// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "web_internal.h"

#include <ccord/core/os.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- CSS color, unit, calc(), and custom-property helpers ----

static int hex_dig(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool ascii_ieq(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool ascii_ieq_n(const char *a, size_t alen, const char *b)
{
  if (!a || !b) return false;
  size_t blen = strlen(b);
  if (alen != blen) return false;
  for (size_t i = 0; i < alen; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

static uint8_t clamp_u8(double x)
{
  return x < 0.0 ? 0u : (x > 255.0 ? 255u : (uint8_t)(x + 0.5));
}

static double hue_to_rgb(double p, double q, double t)
{
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

bool web_css_parse_color(const char *val, ccord_color *out)
{
  if (!val || !out) return false;
  while (*val && isspace((unsigned char)*val)) val++;
  if (!*val) return false;

  if (val[0] == '#') {
    const char *h = val + 1;
    size_t n = 0;
    while (hex_dig(h[n]) >= 0) n++;
    if (n == 3 || n == 4) {
      uint8_t r = (uint8_t)(hex_dig(h[0]) * 17);
      uint8_t g = (uint8_t)(hex_dig(h[1]) * 17);
      uint8_t b = (uint8_t)(hex_dig(h[2]) * 17);
      uint8_t a = (uint8_t)(n == 4 ? (hex_dig(h[3]) * 17) : 255);
      *out = (ccord_color){r, g, b, a};
      return true;
    }
    if (n == 6 || n == 8) {
      uint8_t r = (uint8_t)((hex_dig(h[0]) << 4) | hex_dig(h[1]));
      uint8_t g = (uint8_t)((hex_dig(h[2]) << 4) | hex_dig(h[3]));
      uint8_t b = (uint8_t)((hex_dig(h[4]) << 4) | hex_dig(h[5]));
      uint8_t a = (uint8_t)(n == 8 ? ((hex_dig(h[6]) << 4) | hex_dig(h[7])) : 255);
      *out = (ccord_color){r, g, b, a};
      return true;
    }
    return false;
  }

  if (ccord_strncasecmp(val, "rgb(", 4) == 0 || ccord_strncasecmp(val, "rgba(", 5) == 0) {
    const char *p = strchr(val, '(');
    if (!p) return false;
    p++;
    double nums[4] = {0, 0, 0, 1.0};
    int k = 0;
    while (*p && *p != ')' && k < 4) {
      while (*p && (*p == ' ' || *p == ',' || *p == '/')) p++;
      if (*p == ')') break;
      char *end = nullptr;
      nums[k++] = strtod(p, &end);
      if (end == p) break;
      p = end;
      if (*p == '%') { if (k <= 3) nums[k - 1] *= 2.55; else nums[k - 1] /= 100.0; p++; }
    }
    if (k >= 3) {
      uint8_t a = k >= 4 ? clamp_u8(nums[3] <= 1.0 ? nums[3] * 255.0 : nums[3]) : 255;
      *out = (ccord_color){clamp_u8(nums[0]), clamp_u8(nums[1]), clamp_u8(nums[2]), a};
      return true;
    }
    return false;
  }

  if (ccord_strncasecmp(val, "hsl(", 4) == 0 || ccord_strncasecmp(val, "hsla(", 5) == 0) {
    const char *p = strchr(val, '(');
    if (!p) return false;
    p++;
    double nums[4] = {0, 0, 0, 1.0};
    int k = 0;
    while (*p && *p != ')' && k < 4) {
      while (*p && (*p == ' ' || *p == ',' || *p == '/')) p++;
      if (*p == ')') break;
      char *end = nullptr;
      nums[k++] = strtod(p, &end);
      if (end == p) break;
      p = end;
      if (ccord_strncasecmp(p, "deg", 3) == 0) p += 3;
      else if (*p == '%') { if (k == 4) nums[3] /= 100.0; p++; }
    }
    if (k >= 3) {
      double h = fmod(fmod(nums[0], 360.0) + 360.0, 360.0) / 360.0;
      double s = fmax(0.0, fmin(100.0, nums[1])) / 100.0;
      double l = fmax(0.0, fmin(100.0, nums[2])) / 100.0;
      double r = l, g = l, b = l;
      if (s > 1e-6) {
        double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
        double pv = 2.0 * l - q;
        r = hue_to_rgb(pv, q, h + 1.0 / 3.0);
        g = hue_to_rgb(pv, q, h);
        b = hue_to_rgb(pv, q, h - 1.0 / 3.0);
      }
      uint8_t a = k >= 4 ? clamp_u8(nums[3] <= 1.0 ? nums[3] * 255.0 : nums[3]) : 255;
      *out = (ccord_color){clamp_u8(r * 255.0), clamp_u8(g * 255.0), clamp_u8(b * 255.0), a};
      return true;
    }
    return false;
  }

  // Match the first identifier token (so shorthand values like "white no-repeat" work).
  size_t tlen = 0;
  while (val[tlen] && isalpha((unsigned char)val[tlen])) tlen++;
  if (tlen == 0) return false;
  static const struct { const char *name; ccord_color c; } k_named[] = {
    {"transparent", {0, 0, 0, 0}},
    {"white",       {255, 255, 255, 255}},
    {"black",       {0, 0, 0, 255}},
    {"red",         {235, 64, 52, 255}},
    {"green",       {35, 165, 90, 255}},
    {"blue",        {88, 101, 242, 255}},
    {"gray",        {128, 128, 128, 255}},
    {"grey",        {128, 128, 128, 255}},
    {"lightgray",   {211, 211, 211, 255}},
    {"lightgrey",   {211, 211, 211, 255}},
    {"darkgray",    {169, 169, 169, 255}},
    {"darkgrey",    {169, 169, 169, 255}},
    {"dimgray",     {105, 105, 105, 255}},
    {"dimgrey",     {105, 105, 105, 255}},
    {"silver",      {192, 192, 192, 255}},
    {"whitesmoke",  {245, 245, 245, 255}},
    {"yellow",      {250, 204, 21, 255}},
    {"orange",      {249, 115, 22, 255}},
    {"purple",      {128, 0, 128, 255}},
    {"pink",        {255, 192, 203, 255}},
    {"cyan",        {0, 255, 255, 255}},
    {"aqua",        {0, 255, 255, 255}},
    {"teal",        {0, 128, 128, 255}},
    {"navy",        {0, 0, 128, 255}},
    {"lime",        {0, 255, 0, 255}},
    {"gold",        {255, 215, 0, 255}},
    {"slategray",   {112, 128, 144, 255}},
    {"slategrey",   {112, 128, 144, 255}},
  };
  for (size_t i = 0; i < sizeof k_named / sizeof k_named[0]; i++) {
    if (ascii_ieq_n(val, tlen, k_named[i].name)) {
      *out = k_named[i].c;
      return true;
    }
  }
  return false;
}

static bool parse_length_ex(const char *val, float ref_px, float vw, float vh, float font_px,
                            float *out, bool *is_pct, float *pct_val)
{
  if (is_pct) *is_pct = false;
  if (pct_val) *pct_val = 0.0f;
  if (!val) return false;
  while (*val && isspace((unsigned char)*val)) val++;
  if (!*val || ascii_ieq(val, "auto") || ascii_ieq(val, "none") || ascii_ieq(val, "normal")) return false;

  // Simple calc(A + B) / calc(A - B) support
  if (ccord_strncasecmp(val, "calc(", 5) == 0) {
    const char *p = val + 5;
    float acc = 0.0f;
    float sign = 1.0f;
    bool any = false;
    while (*p && *p != ')') {
      while (*p && isspace((unsigned char)*p)) p++;
      if (!*p || *p == ')') break;
      if (any && (*p == '+' || *p == '-')) {
        sign = (*p == '-') ? -1.0f : 1.0f;
        p++;
        continue;
      }
      char *end = nullptr;
      double v = strtod(p, &end);
      if (end == p) break;
      float term = (float)v;
      if (*end == '%') { term = (float)(v * 0.01 * (double)ref_px); end++; }
      else if (ccord_strncasecmp(end, "vw", 2) == 0) { term = (float)(v * 0.01 * (double)vw); end += 2; }
      else if (ccord_strncasecmp(end, "vh", 2) == 0) { term = (float)(v * 0.01 * (double)vh); end += 2; }
      else if (ccord_strncasecmp(end, "rem", 3) == 0) { term = (float)(v * 14.0); end += 3; }
      else if (ccord_strncasecmp(end, "em", 2) == 0) { term = (float)(v * (double)font_px); end += 2; }
      else if (ccord_strncasecmp(end, "px", 2) == 0) { end += 2; }
      acc += sign * term;
      sign = 1.0f;
      any = true;
      p = end;
    }
    if (any) { *out = acc; return true; }
    return false;
  }

  char *end = nullptr;
  double v = strtod(val, &end);
  if (end == val) return false;
  if (*end == '%') {
    if (is_pct) *is_pct = true;
    if (pct_val) *pct_val = (float)v;
    *out = (float)(v * 0.01 * (double)ref_px);
    return true;
  }
  if (ccord_strncasecmp(end, "vw", 2) == 0) { *out = (float)(v * 0.01 * (double)vw); return true; }
  if (ccord_strncasecmp(end, "vh", 2) == 0) { *out = (float)(v * 0.01 * (double)vh); return true; }
  if (ccord_strncasecmp(end, "rem", 3) == 0) { *out = (float)(v * 14.0); return true; }
  if (ccord_strncasecmp(end, "em", 2) == 0) { *out = (float)(v * (double)(font_px > 0.0f ? font_px : 14.0f)); return true; }
  if (ccord_strncasecmp(end, "pt", 2) == 0) { *out = (float)(v * 1.3333); return true; }
  *out = (float)v;
  return true;
}

// ---- CSS stylesheet parser ----

void web_css_sheet_free(web_css_sheet *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->nrules; i++) {
    ccord_free(s->rules[i].selector);
    ccord_free(s->rules[i].media_query);
    for (size_t j = 0; j < s->rules[i].nprops; j++) {
      ccord_free(s->rules[i].props[j].name);
      ccord_free(s->rules[i].props[j].value);
    }
    ccord_free(s->rules[i].props);
  }
  ccord_free(s->rules);
  memset(s, 0, sizeof *s);
}

static char *trim_dup(const char *s, size_t n)
{
  while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
  while (n > 0 && isspace((unsigned char)s[n - 1u])) n--;
  char *out = ccord_malloc(n + 1u);
  if (!out) return nullptr;
  if (n > 0) memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static int selector_specificity(const char *sel)
{
  int sp = 0;
  bool in_word = false;
  for (const char *p = sel; *p; p++) {
    if (*p == '#') { sp += 100; in_word = true; }
    else if (*p == '.' || *p == '[') { sp += 10; in_word = true; }
    else if (*p == ':') { sp += 10; in_word = true; }
    else if (isalpha((unsigned char)*p) && !in_word) { sp += 1; in_word = true; }
    else if (isspace((unsigned char)*p) || *p == '>' || *p == '+' || *p == '~' || *p == ',') { in_word = false; }
  }
  return sp;
}

static void parse_decls(const char *s, size_t len, web_style_prop **out_props, size_t *out_n)
{
  size_t cap = 8, n = 0;
  web_style_prop *props = ccord_calloc(cap, sizeof *props);
  if (!props) return;
  size_t i = 0;
  while (i < len) {
    while (i < len && (isspace((unsigned char)s[i]) || s[i] == ';')) i++;
    if (i >= len) break;
    size_t ks = i;
    while (i < len && s[i] != ':' && s[i] != ';') i++;
    if (i >= len || s[i] != ':') { while (i < len && s[i] != ';') i++; continue; }
    size_t ke = i;
    i++; // skip ':'
    size_t vs = i;
    int paren = 0;
    char quote = 0;
    while (i < len) {
      char c = s[i];
      if (quote) {
        if (c == '\\' && i + 1u < len) { i += 2u; continue; }
        if (c == quote) quote = 0;
      } else {
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') paren++;
        else if (c == ')' && paren > 0) paren--;
        else if (c == ';' && paren == 0) break;
      }
      i++;
    }
    size_t ve = i;
    char *k = trim_dup(s + ks, ke - ks);
    char *v = trim_dup(s + vs, ve - vs);
    if (k && v && k[0] && v[0]) {
      char *imp = strstr(v, "!important");
      if (imp) {
        *imp = '\0';
        size_t vl = strlen(v);
        while (vl > 0 && isspace((unsigned char)v[vl - 1u])) v[--vl] = '\0';
      }
      // Preserve case for CSS custom properties (--varName)
      if (!(k[0] == '-' && k[1] == '-')) {
        for (char *q = k; *q; q++) *q = (char)tolower((unsigned char)*q);
      }
      if (n == cap) {
        cap *= 2u;
        web_style_prop *np = ccord_realloc(props, cap * sizeof *np);
        if (!np) { ccord_free(k); ccord_free(v); break; }
        props = np;
      }
      props[n].name = k;
      props[n].value = v;
      n++;
    } else {
      ccord_free(k);
      ccord_free(v);
    }
  }
  *out_props = props;
  *out_n = n;
}

void web_css_parse_inline(web_node *n, const char *style_attr)
{
  if (!n) return;
  for (size_t i = 0; i < n->ninline; i++) {
    ccord_free(n->inline_props[i].name);
    ccord_free(n->inline_props[i].value);
  }
  ccord_free(n->inline_props);
  n->inline_props = nullptr;
  n->ninline = n->inline_cap = 0;
  if (!style_attr || !style_attr[0]) return;
  parse_decls(style_attr, strlen(style_attr), &n->inline_props, &n->ninline);
  n->inline_cap = n->ninline;
}

static web_style_prop *clone_props(const web_style_prop *src, size_t n)
{
  web_style_prop *dst = ccord_calloc(n > 0 ? n : 1u, sizeof *dst);
  if (!dst) return nullptr;
  for (size_t i = 0; i < n; i++) {
    dst[i].name = ccord_strdup(src[i].name);
    dst[i].value = ccord_strdup(src[i].value);
  }
  return dst;
}

static void append_rule_one(web_css_sheet *s, char *sel, const char *mq,
                            web_style_prop *props, size_t nprops)
{
  if (!sel || !sel[0] || nprops == 0) {
    ccord_free(sel);
    for (size_t k = 0; k < nprops; k++) { ccord_free(props[k].name); ccord_free(props[k].value); }
    ccord_free(props);
    return;
  }
  if (s->nrules == s->cap) {
    size_t nc = s->cap ? s->cap * 2u : 32u;
    web_css_rule *nr = ccord_realloc(s->rules, nc * sizeof *nr);
    if (!nr) {
      ccord_free(sel);
      for (size_t k = 0; k < nprops; k++) { ccord_free(props[k].name); ccord_free(props[k].value); }
      ccord_free(props);
      return;
    }
    s->rules = nr;
    s->cap = nc;
  }
  s->rules[s->nrules].selector = sel;
  s->rules[s->nrules].media_query = (mq && mq[0]) ? ccord_strdup(mq) : nullptr;
  s->rules[s->nrules].props = props;
  s->rules[s->nrules].nprops = nprops;
  s->rules[s->nrules].specificity = selector_specificity(sel);
  s->nrules++;
}

static void parse_sheet_with_media(web_css_sheet *s, const char *css, size_t len, const char *media_query, int depth)
{
  if (!s || !css || len == 0 || depth > 6) return;
  // Strip /* ... */ comments while respecting quoted strings
  char *clean = ccord_malloc(len + 1u);
  if (!clean) return;
  size_t w = 0;
  char quote = 0;
  for (size_t i = 0; i < len; ) {
    if (quote) {
      if (css[i] == '\\' && i + 1u < len) {
        clean[w++] = css[i++];
        clean[w++] = css[i++];
        continue;
      }
      if (css[i] == quote) quote = 0;
      clean[w++] = css[i++];
    } else if (css[i] == '"' || css[i] == '\'') {
      quote = css[i];
      clean[w++] = css[i++];
    } else if (i + 1u < len && css[i] == '/' && css[i + 1u] == '*') {
      i += 2u;
      while (i + 1u < len && !(css[i] == '*' && css[i + 1u] == '/')) i++;
      if (i + 1u < len) i += 2u;
    } else {
      clean[w++] = css[i++];
    }
  }
  clean[w] = '\0';

  size_t i = 0;
  while (i < w) {
    while (i < w && isspace((unsigned char)clean[i])) i++;
    if (i >= w) break;
    if (clean[i] == '@') {
      size_t at_s = i;
      char q = 0;
      while (i < w) {
        if (q) { if (clean[i] == '\\' && i + 1u < w) { i += 2u; continue; } if (clean[i] == q) q = 0; }
        else if (clean[i] == '"' || clean[i] == '\'') q = clean[i];
        else if (clean[i] == '{' || clean[i] == ';') break;
        i++;
      }
      size_t at_e = i;
      if (i < w && clean[i] == ';') { i++; continue; }
      if (i < w && clean[i] == '{') {
        i++;
        size_t inner_s = i;
        int dep = 1;
        q = 0;
        while (i < w && dep > 0) {
          if (q) { if (clean[i] == '\\' && i + 1u < w) { i += 2u; continue; } if (clean[i] == q) q = 0; }
          else if (clean[i] == '"' || clean[i] == '\'') q = clean[i];
          else if (clean[i] == '{') dep++;
          else if (clean[i] == '}') dep--;
          if (dep > 0) i++;
        }
        size_t inner_e = i;
        if (i < w && clean[i] == '}') i++;

        if (at_e - at_s >= 6 && ccord_strncasecmp(clean + at_s, "@media", 6) == 0 && inner_e > inner_s) {
          char *mq = trim_dup(clean + at_s + 6, at_e - (at_s + 6));
          parse_sheet_with_media(s, clean + inner_s, inner_e - inner_s, mq ? mq : media_query, depth + 1);
          ccord_free(mq);
        } else if (at_e - at_s >= 9 && ccord_strncasecmp(clean + at_s, "@supports", 9) == 0 && inner_e > inner_s) {
          char *cond = trim_dup(clean + at_s + 9, at_e - (at_s + 9));
          bool negated = cond && ccord_strncasecmp(cond, "not", 3) == 0;
          ccord_free(cond);
          if (!negated) {
            parse_sheet_with_media(s, clean + inner_s, inner_e - inner_s, media_query, depth + 1);
          }
        }
      }
      continue;
    }

    size_t sel_s = i;
    char q = 0;
    while (i < w) {
      if (q) { if (clean[i] == '\\' && i + 1u < w) { i += 2u; continue; } if (clean[i] == q) q = 0; }
      else if (clean[i] == '"' || clean[i] == '\'') q = clean[i];
      else if (clean[i] == '{') break;
      i++;
    }
    if (i >= w) break;
    size_t sel_e = i;
    i++; // skip '{'
    size_t body_s = i;
    int dep = 1;
    q = 0;
    while (i < w && dep > 0) {
      if (q) { if (clean[i] == '\\' && i + 1u < w) { i += 2u; continue; } if (clean[i] == q) q = 0; }
      else if (clean[i] == '"' || clean[i] == '\'') q = clean[i];
      else if (clean[i] == '{') dep++;
      else if (clean[i] == '}') dep--;
      if (dep > 0) i++;
    }
    size_t body_e = i;
    if (i < w && clean[i] == '}') i++;

    web_style_prop *props = nullptr;
    size_t nprops = 0;
    parse_decls(clean + body_s, body_e - body_s, &props, &nprops);
    if (nprops == 0) { ccord_free(props); continue; }

    // Split top-level comma-separated selectors so each gets its own exact specificity.
    size_t cur = sel_s;
    bool first_used = false;
    while (cur < sel_e) {
      size_t st = cur;
      int br = 0, pa = 0;
      char sq = 0;
      while (cur < sel_e) {
        char c = clean[cur];
        if (sq) { if (c == '\\' && cur + 1u < sel_e) { cur += 2u; continue; } if (c == sq) sq = 0; }
        else if (c == '"' || c == '\'') sq = c;
        else if (c == '[') br++; else if (c == ']' && br > 0) br--;
        else if (c == '(') pa++; else if (c == ')' && pa > 0) pa--;
        else if (c == ',' && br == 0 && pa == 0) break;
        cur++;
      }
      char *one = trim_dup(clean + st, cur - st);
      if (cur < sel_e && clean[cur] == ',') cur++;
      if (!one || !one[0]) { ccord_free(one); continue; }
      if (!first_used) {
        append_rule_one(s, one, media_query, props, nprops);
        first_used = true;
      } else {
        web_style_prop *cp = clone_props(props, nprops);
        if (cp) append_rule_one(s, one, media_query, cp, nprops);
        else ccord_free(one);
      }
    }
    if (!first_used) {
      for (size_t k = 0; k < nprops; k++) { ccord_free(props[k].name); ccord_free(props[k].value); }
      ccord_free(props);
    }
  }
  ccord_free(clean);
}

void web_css_parse_sheet(web_css_sheet *s, const char *css, size_t len)
{
  parse_sheet_with_media(s, css, len, nullptr, 0);
}

// ---- Media query evaluator & CSS custom properties ----

static bool media_query_matches(const char *mq, float vw, float vh, bool dark_theme)
{
  if (!mq || !mq[0]) return true;
  // Lowercase scratch copy for easy matching
  char buf[256];
  size_t n = strlen(mq);
  if (n >= sizeof buf) n = sizeof buf - 1u;
  for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)mq[i]);
  buf[n] = '\0';

  if (strstr(buf, "print") && !strstr(buf, "screen") && !strstr(buf, "all")) return false;
  if (strstr(buf, "forced-colors") && strstr(buf, "active")) return false;
  if (strstr(buf, "prefers-contrast") && strstr(buf, "more")) return false;
  if (strstr(buf, "prefers-color-scheme")) {
    if (strstr(buf, "dark") && !dark_theme) return false;
    if (strstr(buf, "light") && dark_theme) return false;
  }
  const char *p = buf;
  while ((p = strstr(p, "max-width")) != nullptr) {
    const char *c = strchr(p, ':');
    if (c) {
      float lim = (float)strtod(c + 1, nullptr);
      if (lim >= 0.0f && vw > lim + 0.5f) return false;
    }
    p += 9;
  }
  p = buf;
  while ((p = strstr(p, "min-width")) != nullptr) {
    const char *c = strchr(p, ':');
    if (c) {
      float lim = (float)strtod(c + 1, nullptr);
      if (vw < lim - 0.5f) return false;
    }
    p += 9;
  }
  p = buf;
  while ((p = strstr(p, "max-height")) != nullptr) {
    const char *c = strchr(p, ':');
    if (c) {
      float lim = (float)strtod(c + 1, nullptr);
      if (lim >= 0.0f && vh > lim + 0.5f) return false;
    }
    p += 10;
  }
  p = buf;
  while ((p = strstr(p, "min-height")) != nullptr) {
    const char *c = strchr(p, ':');
    if (c) {
      float lim = (float)strtod(c + 1, nullptr);
      if (vh < lim - 0.5f) return false;
    }
    p += 10;
  }
  return true;
}

static void node_clear_vars(web_node *n)
{
  for (size_t i = 0; i < n->nvars; i++) {
    ccord_free(n->css_vars[i].name);
    ccord_free(n->css_vars[i].value);
  }
  n->nvars = 0;
}

static void node_set_var(web_node *n, const char *name, const char *val)
{
  for (size_t i = 0; i < n->nvars; i++) {
    if (strcmp(n->css_vars[i].name, name) == 0) {
      char *nv = ccord_strdup(val);
      if (nv) { ccord_free(n->css_vars[i].value); n->css_vars[i].value = nv; }
      return;
    }
  }
  if (n->nvars == n->var_cap) {
    size_t nc = n->var_cap ? n->var_cap * 2u : 8u;
    web_style_prop *np = ccord_realloc(n->css_vars, nc * sizeof *np);
    if (!np) return;
    n->css_vars = np;
    n->var_cap = nc;
  }
  n->css_vars[n->nvars].name = ccord_strdup(name);
  n->css_vars[n->nvars].value = ccord_strdup(val);
  n->nvars++;
}

static const char *node_lookup_var(const web_node *n, const char *name)
{
  for (const web_node *cur = n; cur; cur = cur->parent) {
    for (size_t i = 0; i < cur->nvars; i++) {
      if (strcmp(cur->css_vars[i].name, name) == 0) return cur->css_vars[i].value;
    }
  }
  return nullptr;
}

static void resolve_css_vars(const web_node *n, const char *in, char *out, size_t cap, int depth)
{
  if (!in || !out || cap == 0) return;
  if (depth > 6 || !strstr(in, "var(")) {
    snprintf(out, cap, "%s", in);
    return;
  }
  size_t w = 0;
  const char *p = in;
  while (*p && w + 1u < cap) {
    if (ccord_strncasecmp(p, "var(", 4) == 0) {
      p += 4;
      const char *start = p;
      int dep = 1;
      while (*p && dep > 0) {
        if (*p == '(') dep++;
        else if (*p == ')') dep--;
        if (dep > 0) p++;
      }
      const char *end = p;
      if (*p == ')') p++;

      // Split into --name and optional ,fallback
      const char *comma = nullptr;
      int p2 = 0;
      for (const char *q = start; q < end; q++) {
        if (*q == '(') p2++;
        else if (*q == ')' && p2 > 0) p2--;
        else if (*q == ',' && p2 == 0) { comma = q; break; }
      }
      const char *ne = comma ? comma : end;
      while (start < ne && isspace((unsigned char)*start)) start++;
      while (ne > start && isspace((unsigned char)ne[-1])) ne--;
      char vname[96];
      size_t nl = (size_t)(ne - start);
      if (nl >= sizeof vname) nl = sizeof vname - 1u;
      memcpy(vname, start, nl);
      vname[nl] = '\0';

      const char *found = node_lookup_var(n, vname);
      char sub[512];
      sub[0] = '\0';
      if (found) {
        resolve_css_vars(n, found, sub, sizeof sub, depth + 1);
      } else if (comma) {
        const char *fb = comma + 1;
        while (fb < end && isspace((unsigned char)*fb)) fb++;
        char fb_raw[512];
        size_t fl = (size_t)(end - fb);
        if (fl >= sizeof fb_raw) fl = sizeof fb_raw - 1u;
        memcpy(fb_raw, fb, fl);
        fb_raw[fl] = '\0';
        resolve_css_vars(n, fb_raw, sub, sizeof sub, depth + 1);
      }
      size_t sl = strlen(sub);
      if (w + sl >= cap) sl = cap - 1u - w;
      memcpy(out + w, sub, sl);
      w += sl;
    } else {
      out[w++] = *p++;
    }
  }
  out[w] = '\0';
}

// ---- Style application & cascade ----

static void parse_4box(const char *val, float dst[4], float ref_px, float vw, float vh, float font_px,
                       bool *auto_l, bool *auto_r)
{
  float vals[4] = {0};
  bool is_auto[4] = {false, false, false, false};
  int cnt = 0;
  const char *p = val;
  while (*p && cnt < 4) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;
    const char *st = p;
    int par = 0;
    while (*p && (par > 0 || !isspace((unsigned char)*p))) {
      if (*p == '(') par++;
      else if (*p == ')' && par > 0) par--;
      p++;
    }
    char tok[64];
    size_t tl = (size_t)(p - st);
    if (tl >= sizeof tok) tl = sizeof tok - 1u;
    memcpy(tok, st, tl); tok[tl] = '\0';
    if (ascii_ieq(tok, "auto")) {
      vals[cnt] = 0.0f;
      is_auto[cnt] = true;
      cnt++;
    } else {
      float fv = 0.0f;
      if (parse_length_ex(tok, ref_px, vw, vh, font_px, &fv, nullptr, nullptr)) {
        vals[cnt++] = fv;
      }
    }
  }
  if (cnt == 1) {
    dst[0] = dst[1] = dst[2] = dst[3] = vals[0];
    if (auto_r) *auto_r = is_auto[0];
    if (auto_l) *auto_l = is_auto[0];
  } else if (cnt == 2) {
    dst[0] = dst[2] = vals[0];
    dst[1] = dst[3] = vals[1];
    if (auto_r) *auto_r = is_auto[1];
    if (auto_l) *auto_l = is_auto[1];
  } else if (cnt == 3) {
    dst[0] = vals[0]; dst[1] = dst[3] = vals[1]; dst[2] = vals[2];
    if (auto_r) *auto_r = is_auto[1];
    if (auto_l) *auto_l = is_auto[1];
  } else if (cnt == 4) {
    for (int k = 0; k < 4; k++) dst[k] = vals[k];
    if (auto_r) *auto_r = is_auto[1];
    if (auto_l) *auto_l = is_auto[3];
  }
}

static void extract_bg_url(const char *val, char *out, size_t cap)
{
  const char *u = strstr(val, "url(");
  if (!u) return;
  u += 4;
  while (*u && isspace((unsigned char)*u)) u++;
  char quote = 0;
  if (*u == '"' || *u == '\'') { quote = *u++; }
  const char *e = u;
  if (quote) {
    while (*e && *e != quote) {
      if (*e == '\\' && e[1]) e += 2;
      else e++;
    }
  } else {
    int par = 0;
    while (*e) {
      if (*e == '(') par++;
      else if (*e == ')') { if (par == 0) break; par--; }
      e++;
    }
    while (e > u && isspace((unsigned char)e[-1])) e--;
  }
  size_t len = (size_t)(e - u);
  if (len > 0 && len < cap) {
    memcpy(out, u, len);
    out[len] = '\0';
  }
}

static bool parse_color_with_current(const char *v, ccord_color current, ccord_color *out)
{
  if (!v) return false;
  while (*v && isspace((unsigned char)*v)) v++;
  if (ccord_strncasecmp(v, "currentcolor", 12) == 0) {
    *out = current;
    return true;
  }
  return web_css_parse_color(v, out);
}

static void apply_prop(web_style *st, const char *k, const char *v, float ref_w, float ref_h, float vw, float vh)
{
  if (strcmp(k, "display") == 0) {
    st->is_inline_level = false;
    if (ascii_ieq(v, "none")) st->display = WEB_DISP_NONE;
    else if (ascii_ieq(v, "inline-flex")) { st->display = WEB_DISP_FLEX; st->is_inline_level = true; }
    else if (ascii_ieq(v, "flex")) st->display = WEB_DISP_FLEX;
    else if (ascii_ieq(v, "inline-grid")) { st->display = WEB_DISP_GRID; st->is_inline_level = true; }
    else if (ascii_ieq(v, "grid")) st->display = WEB_DISP_GRID;
    else if (ascii_ieq(v, "inline-table")) { st->display = WEB_DISP_TABLE; st->is_inline_level = true; }
    else if (ascii_ieq(v, "table")) st->display = WEB_DISP_TABLE;
    else if (ascii_ieq(v, "table-row")) st->display = WEB_DISP_TABLE_ROW;
    else if (ascii_ieq(v, "table-cell")) st->display = WEB_DISP_TABLE_CELL;
    else if (ascii_ieq(v, "inline")) { st->display = WEB_DISP_INLINE; st->is_inline_level = true; }
    else if (ascii_ieq(v, "inline-block")) { st->display = WEB_DISP_INLINE_BLOCK; st->is_inline_level = true; }
    else st->display = WEB_DISP_BLOCK;
  } else if (strcmp(k, "position") == 0) {
    if (ascii_ieq(v, "absolute")) st->position = WEB_POS_ABSOLUTE;
    else if (ascii_ieq(v, "fixed")) st->position = WEB_POS_FIXED;
    else if (ascii_ieq(v, "relative") || ascii_ieq(v, "sticky")) st->position = WEB_POS_RELATIVE;
    else st->position = WEB_POS_STATIC;
  } else if (strcmp(k, "float") == 0) {
    if (ascii_ieq(v, "left")) st->float_dir = 1;
    else if (ascii_ieq(v, "right")) st->float_dir = 2;
    else st->float_dir = 0;
  } else if (strcmp(k, "clear") == 0) {
    if (ascii_ieq(v, "left")) st->clear_dir = 1;
    else if (ascii_ieq(v, "right")) st->clear_dir = 2;
    else if (ascii_ieq(v, "both")) st->clear_dir = 3;
    else st->clear_dir = 0;
  } else if (strcmp(k, "box-sizing") == 0) {
    st->border_box = (strstr(v, "border-box") != nullptr);
  } else if (strcmp(k, "flex-direction") == 0) {
    st->flex_dir = (strstr(v, "column") != nullptr) ? WEB_FLEX_COLUMN : WEB_FLEX_ROW;
    st->flex_reverse = (strstr(v, "reverse") != nullptr);
  } else if (strcmp(k, "flex-wrap") == 0) {
    st->flex_wrap = (strstr(v, "wrap") != nullptr && strstr(v, "nowrap") == nullptr);
  } else if (strcmp(k, "flex-grow") == 0 || strcmp(k, "flex") == 0) {
    if (ascii_ieq(v, "none")) st->flex_grow = 0.0f;
    else if (ascii_ieq(v, "auto")) st->flex_grow = 1.0f;
    else {
      char *end = nullptr;
      double fg = strtod(v, &end);
      if (end != v) st->flex_grow = (float)fg;
    }
  } else if (strcmp(k, "justify-content") == 0) {
    if (strstr(v, "center")) st->justify_content = WEB_ALIGN_CENTER;
    else if (strstr(v, "end") || ascii_ieq(v, "right")) st->justify_content = WEB_ALIGN_END;
    else if (strstr(v, "between")) st->justify_content = WEB_ALIGN_SPACE_BETWEEN;
    else if (strstr(v, "around") || strstr(v, "evenly")) st->justify_content = WEB_ALIGN_SPACE_AROUND;
    else st->justify_content = WEB_ALIGN_START;
  } else if (strcmp(k, "align-items") == 0) {
    if (strstr(v, "center")) st->align_items = WEB_ALIGN_CENTER;
    else if (strstr(v, "end")) st->align_items = WEB_ALIGN_END;
    else if (strstr(v, "stretch")) st->align_items = WEB_ALIGN_STRETCH;
    else st->align_items = WEB_ALIGN_START;
  } else if (strcmp(k, "align-self") == 0) {
    if (ascii_ieq(v, "auto")) st->align_self = 0;
    else if (strstr(v, "center")) st->align_self = 2;
    else if (strstr(v, "end")) st->align_self = 3;
    else if (strstr(v, "stretch")) st->align_self = 4;
    else st->align_self = 1;
  } else if (strcmp(k, "vertical-align") == 0) {
    if (strstr(v, "middle")) st->vertical_align = 1;
    else if (strstr(v, "bottom")) st->vertical_align = 2;
    else st->vertical_align = 0;
  } else if (strcmp(k, "gap") == 0 || strcmp(k, "grid-gap") == 0 || strcmp(k, "column-gap") == 0) {
    (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->gap, nullptr, nullptr);
  } else if (strcmp(k, "grid-template-columns") == 0) {
    const char *rep = strstr(v, "repeat(");
    if (rep) {
      int cols = atoi(rep + 7);
      if (cols >= 1 && cols <= 12) st->grid_cols = cols;
    } else {
      int cols = 0;
      const char *p = v;
      while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        cols++;
        while (*p && !isspace((unsigned char)*p)) p++;
      }
      if (cols >= 1 && cols <= 12) st->grid_cols = cols;
    }
  } else if (strcmp(k, "width") == 0) {
    if (ascii_ieq(v, "auto")) {
      st->has_width = false;
      st->width_pct = false;
      st->width_fit_content = false;
    } else if (strstr(v, "fit-content") || strstr(v, "max-content") || strstr(v, "min-content")) {
      st->has_width = false;
      st->width_pct = false;
      st->width_fit_content = true;
    } else {
      st->width_fit_content = false;
      st->has_width = parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->width, &st->width_pct, &st->width_pct_val);
    }
  } else if (strcmp(k, "height") == 0) {
    if (ascii_ieq(v, "auto")) { st->has_height = false; st->height_pct = false; }
    else st->has_height = parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->height, &st->height_pct, &st->height_pct_val);
  } else if (strcmp(k, "min-width") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->min_w, nullptr, nullptr); }
  else if (strcmp(k, "min-height") == 0) { (void)parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->min_h, nullptr, nullptr); }
  else if (strcmp(k, "max-width") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->max_w, nullptr, nullptr); }
  else if (strcmp(k, "max-height") == 0) { (void)parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->max_h, nullptr, nullptr); }
  else if (strcmp(k, "top") == 0) {
    float pv = 0;
    st->has_top = parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->top, &st->top_pct, &pv);
    if (st->top_pct) st->top = pv;
  } else if (strcmp(k, "left") == 0) {
    float pv = 0;
    st->has_left = parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->left, &st->left_pct, &pv);
    if (st->left_pct) st->left = pv;
  } else if (strcmp(k, "right") == 0) {
    float pv = 0;
    st->has_right = parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->right, &st->right_pct, &pv);
    if (st->right_pct) st->right = pv;
  } else if (strcmp(k, "bottom") == 0) {
    float pv = 0;
    st->has_bottom = parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->bottom, &st->bottom_pct, &pv);
    if (st->bottom_pct) st->bottom = pv;
  } else if (strcmp(k, "inset") == 0) {
    float ib[4] = {0};
    parse_4box(v, ib, ref_w, vw, vh, st->font_size, nullptr, nullptr);
    st->has_top = st->has_right = st->has_bottom = st->has_left = true;
    st->top_pct = st->right_pct = st->bottom_pct = st->left_pct = false;
    st->top = ib[0]; st->right = ib[1]; st->bottom = ib[2]; st->left = ib[3];
  } else if (strcmp(k, "margin") == 0) {
    parse_4box(v, st->margin, ref_w, vw, vh, st->font_size, &st->margin_auto_l, &st->margin_auto_r);
  } else if (strcmp(k, "margin-top") == 0) { (void)parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->margin[0], nullptr, nullptr); }
  else if (strcmp(k, "margin-right") == 0) {
    st->margin_auto_r = ascii_ieq(v, "auto");
    if (!st->margin_auto_r) (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->margin[1], nullptr, nullptr);
  } else if (strcmp(k, "margin-bottom") == 0) { (void)parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->margin[2], nullptr, nullptr); }
  else if (strcmp(k, "margin-left") == 0) {
    st->margin_auto_l = ascii_ieq(v, "auto");
    if (!st->margin_auto_l) (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->margin[3], nullptr, nullptr);
  } else if (strcmp(k, "padding") == 0) {
    parse_4box(v, st->padding, ref_w, vw, vh, st->font_size, nullptr, nullptr);
  } else if (strcmp(k, "padding-top") == 0) { (void)parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->padding[0], nullptr, nullptr); }
  else if (strcmp(k, "padding-right") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->padding[1], nullptr, nullptr); }
  else if (strcmp(k, "padding-bottom") == 0) { (void)parse_length_ex(v, ref_h, vw, vh, st->font_size, &st->padding[2], nullptr, nullptr); }
  else if (strcmp(k, "padding-left") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->padding[3], nullptr, nullptr); }
  else if (strcmp(k, "border-radius") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->border_radius, nullptr, nullptr); }
  else if (strcmp(k, "border-color") == 0 || strcmp(k, "border-top-color") == 0 ||
           strcmp(k, "border-right-color") == 0 || strcmp(k, "border-bottom-color") == 0 ||
           strcmp(k, "border-left-color") == 0) {
    st->has_border_color = parse_color_with_current(v, st->color, &st->border_color);
  } else if (strcmp(k, "border-width") == 0) {
    parse_4box(v, st->border_w, ref_w, vw, vh, st->font_size, nullptr, nullptr);
  } else if (strcmp(k, "border-top-width") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->border_w[0], nullptr, nullptr); }
  else if (strcmp(k, "border-right-width") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->border_w[1], nullptr, nullptr); }
  else if (strcmp(k, "border-bottom-width") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->border_w[2], nullptr, nullptr); }
  else if (strcmp(k, "border-left-width") == 0) { (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->border_w[3], nullptr, nullptr); }
  else if (strcmp(k, "border-style") == 0) {
    if (ascii_ieq(v, "none") || ascii_ieq(v, "hidden")) memset(st->border_w, 0, sizeof st->border_w);
  } else if (strcmp(k, "border") == 0 || strcmp(k, "border-top") == 0 || strcmp(k, "border-right") == 0 ||
             strcmp(k, "border-bottom") == 0 || strcmp(k, "border-left") == 0) {
    if (ascii_ieq(v, "none") || strcmp(v, "0") == 0 || strcmp(v, "0px") == 0) {
      if (strcmp(k, "border") == 0) memset(st->border_w, 0, sizeof st->border_w);
      else if (strcmp(k, "border-top") == 0) st->border_w[0] = 0.0f;
      else if (strcmp(k, "border-right") == 0) st->border_w[1] = 0.0f;
      else if (strcmp(k, "border-bottom") == 0) st->border_w[2] = 0.0f;
      else if (strcmp(k, "border-left") == 0) st->border_w[3] = 0.0f;
    } else {
      float bw = 1.0f;
      bool saw_none = false;
      const char *p = v;
      while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        float tmp = 0.0f;
        if (ccord_strncasecmp(p, "none", 4) == 0 && (!p[4] || isspace((unsigned char)p[4]))) {
          saw_none = true;
        } else if (parse_color_with_current(p, st->color, &st->border_color)) {
          st->has_border_color = true;
        } else if (isdigit((unsigned char)*p) && parse_length_ex(p, ref_w, vw, vh, st->font_size, &tmp, nullptr, nullptr)) {
          bw = tmp;
        }
        int par = 0;
        while (*p && (par > 0 || !isspace((unsigned char)*p))) {
          if (*p == '(') par++; else if (*p == ')' && par > 0) par--;
          p++;
        }
      }
      if (saw_none) bw = 0.0f;
      if (strcmp(k, "border") == 0) st->border_w[0] = st->border_w[1] = st->border_w[2] = st->border_w[3] = bw;
      else if (strcmp(k, "border-top") == 0) st->border_w[0] = bw;
      else if (strcmp(k, "border-right") == 0) st->border_w[1] = bw;
      else if (strcmp(k, "border-bottom") == 0) st->border_w[2] = bw;
      else if (strcmp(k, "border-left") == 0) st->border_w[3] = bw;
    }
  } else if (strcmp(k, "background-color") == 0) {
    if (parse_color_with_current(v, st->color, &st->bg)) st->has_bg = (st->bg.a > 0);
  } else if (strcmp(k, "background-image") == 0) {
    extract_bg_url(v, st->bg_image_url, sizeof st->bg_image_url);
  } else if (strcmp(k, "background-size") == 0) {
    st->bg_cover = (strstr(v, "cover") != nullptr);
  } else if (strcmp(k, "background") == 0) {
    if (strstr(v, "url(")) extract_bg_url(v, st->bg_image_url, sizeof st->bg_image_url);
    const char *p = v;
    while (*p) {
      while (*p && isspace((unsigned char)*p)) p++;
      if (!*p) break;
      if (parse_color_with_current(p, st->color, &st->bg)) { st->has_bg = (st->bg.a > 0); break; }
      int par = 0;
      while (*p && (par > 0 || !isspace((unsigned char)*p))) {
        if (*p == '(') par++; else if (*p == ')' && par > 0) par--;
        p++;
      }
    }
  } else if (strcmp(k, "color") == 0) {
    st->has_color = web_css_parse_color(v, &st->color);
  } else if (strcmp(k, "fill") == 0) {
    if (ascii_ieq(v, "none") || ascii_ieq(v, "transparent")) { st->svg_fill_none = true; st->has_svg_fill = false; }
    else if (parse_color_with_current(v, st->color, &st->svg_fill)) { st->has_svg_fill = true; st->svg_fill_none = false; }
  } else if (strcmp(k, "stroke") == 0) {
    if (ascii_ieq(v, "none") || ascii_ieq(v, "transparent")) { st->svg_stroke_none = true; st->has_svg_stroke = false; }
    else if (parse_color_with_current(v, st->color, &st->svg_stroke)) { st->has_svg_stroke = true; st->svg_stroke_none = false; }
  } else if (strcmp(k, "stroke-width") == 0) {
    (void)parse_length_ex(v, ref_w, vw, vh, st->font_size, &st->svg_stroke_w, nullptr, nullptr);
  } else if (strcmp(k, "font-size") == 0) {
    float fs = 14.0f;
    if (parse_length_ex(v, 14.0f, vw, vh, st->font_size, &fs, nullptr, nullptr) && fs >= 6.0f && fs <= 96.0f) {
      st->font_size = fs;
    }
  } else if (strcmp(k, "line-height") == 0) {
    char *end = nullptr;
    double lh = strtod(v, &end);
    if (end != v && lh > 0.0) {
      if (*end == '\0') st->line_height = (float)lh * st->font_size;
      else if (*end == '%') st->line_height = (float)(lh * 0.01) * st->font_size;
      else st->line_height = (float)lh;
    }
  } else if (strcmp(k, "font-weight") == 0) {
    st->font_bold = (strstr(v, "bold") != nullptr || atoi(v) >= 600);
  } else if (strcmp(k, "text-align") == 0) {
    if (ascii_ieq(v, "center")) st->text_align = 1;
    else if (ascii_ieq(v, "right")) st->text_align = 2;
    else st->text_align = 0;
  } else if (strcmp(k, "white-space") == 0) {
    st->white_space_nowrap = (strstr(v, "nowrap") != nullptr);
    st->white_space_pre = (strstr(v, "pre") != nullptr);
  } else if (strcmp(k, "opacity") == 0) {
    char *end = nullptr;
    double op = strtod(v, &end);
    if (end != v) {
      st->opacity = op < 0.0 ? 0.0f : (op > 1.0 ? 1.0f : (float)op);
    }
  } else if (strcmp(k, "visibility") == 0) {
    if (ascii_ieq(v, "visible")) st->hidden = false;
    else if (ascii_ieq(v, "hidden") || ascii_ieq(v, "collapse")) st->hidden = true;
  } else if (strcmp(k, "overflow") == 0 || strcmp(k, "overflow-x") == 0 || strcmp(k, "overflow-y") == 0) {
    st->overflow_hidden = (strstr(v, "hidden") != nullptr || strstr(v, "clip") != nullptr);
  } else if (strcmp(k, "pointer-events") == 0) {
    if (ascii_ieq(v, "none")) st->pointer_none = true;
    else st->pointer_none = false;
  } else if (strcmp(k, "z-index") == 0) {
    if (ascii_ieq(v, "auto")) {
      st->has_z_index = false;
      st->z_index = 0;
    } else {
      char *end = nullptr;
      long long zv = strtoll(v, &end, 10);
      if (end != v) {
        if (zv > 2147483647LL) zv = 2147483647LL;
        if (zv < -2147483648LL) zv = -2147483648LL;
        st->z_index = (int)zv;
        st->has_z_index = true;
      }
    }
  } else if (strcmp(k, "transform") == 0 || strcmp(k, "-webkit-transform") == 0 || strcmp(k, "webkit-transform") == 0) {
    if (ascii_ieq(v, "none")) {
      st->translate_x = st->translate_y = 0.0f;
      st->translate_x_pct = st->translate_y_pct = false;
      st->scale_x = st->scale_y = 1.0f;
    } else {
      const char *p = v;
      while (*p) {
        if (ccord_strncasecmp(p, "translatex(", 11) == 0) {
          char *end = nullptr;
          double tx = strtod(p + 11, &end);
          if (end != p + 11) { st->translate_x = (float)tx; st->translate_x_pct = (*end == '%'); }
        } else if (ccord_strncasecmp(p, "translatey(", 11) == 0) {
          char *end = nullptr;
          double ty = strtod(p + 11, &end);
          if (end != p + 11) { st->translate_y = (float)ty; st->translate_y_pct = (*end == '%'); }
        } else if (ccord_strncasecmp(p, "translate(", 10) == 0 || ccord_strncasecmp(p, "translate3d(", 12) == 0) {
          const char *lp = strchr(p, '(');
          if (lp) {
            char *end = nullptr;
            double tx = strtod(lp + 1, &end);
            if (end != lp + 1) { st->translate_x = (float)tx; st->translate_x_pct = (*end == '%'); }
            const char *cm = strchr(lp + 1, ',');
            if (cm) {
              double ty = strtod(cm + 1, &end);
              if (end != cm + 1) { st->translate_y = (float)ty; st->translate_y_pct = (*end == '%'); }
            }
          }
        } else if (ccord_strncasecmp(p, "scale(", 6) == 0) {
          char *end = nullptr;
          double sx = strtod(p + 6, &end);
          if (end != p + 6) {
            st->scale_x = st->scale_y = (float)sx;
            const char *cm = strchr(end, ',');
            const char *rp = strchr(end, ')');
            if (cm && (!rp || cm < rp)) {
              double sy = strtod(cm + 1, &end);
              if (end != cm + 1) st->scale_y = (float)sy;
            }
          }
        }
        p++;
      }
    }
  }
}

static void apply_decl_with_vars(web_node *n, web_style *st, const char *k, const char *v,
                                 float ref_w, float ref_h, float vw, float vh)
{
  if (k[0] == '-' && k[1] == '-') return; // handled in var pass
  if (strstr(v, "var(")) {
    char resolved[512];
    resolve_css_vars(n, v, resolved, sizeof resolved, 0);
    if (resolved[0]) apply_prop(st, k, resolved, ref_w, ref_h, vw, vh);
  } else {
    apply_prop(st, k, v, ref_w, ref_h, vw, vh);
  }
}

static void compute_style_tree(web_frame *f, web_node *n, const web_style *parent_st,
                               float cw, float ch, float vw, float vh)
{
  web_style *st = &n->computed;
  memset(st, 0, sizeof *st);
  st->opacity = 1.0f;
  st->scale_x = 1.0f;
  st->scale_y = 1.0f;
  st->border_box = true; // default border-box avoids unexpected clipping in compact UI cards
  st->hidden = parent_st ? parent_st->hidden : false;
  st->pointer_none = parent_st ? parent_st->pointer_none : false;
  st->font_size = parent_st ? parent_st->font_size : 14.0f;
  st->line_height = parent_st ? parent_st->line_height : 0.0f;
  st->font_bold = parent_st ? parent_st->font_bold : false;
  st->text_align = parent_st ? parent_st->text_align : 0;
  st->white_space_nowrap = parent_st ? parent_st->white_space_nowrap : false;
  st->white_space_pre = parent_st ? parent_st->white_space_pre : false;
  st->color = parent_st ? parent_st->color : (f->view->cfg.dark_theme ? (ccord_color){235, 237, 240, 255} : (ccord_color){30, 32, 36, 255});
  st->has_color = true;
  st->align_items = WEB_ALIGN_STRETCH;

  node_clear_vars(n);

  if (n->type == WEB_NODE_ELEMENT && n->tag) {
    // UA defaults
    if (strcmp(n->tag, "head") == 0 || strcmp(n->tag, "script") == 0 || strcmp(n->tag, "style") == 0 ||
        strcmp(n->tag, "meta") == 0 || strcmp(n->tag, "title") == 0 || strcmp(n->tag, "link") == 0 ||
        strcmp(n->tag, "noscript") == 0 || strcmp(n->tag, "defs") == 0 || strcmp(n->tag, "clippath") == 0) {
      st->display = WEB_DISP_NONE;
    } else if (strcmp(n->tag, "span") == 0 || strcmp(n->tag, "a") == 0 || strcmp(n->tag, "b") == 0 ||
               strcmp(n->tag, "strong") == 0 || strcmp(n->tag, "em") == 0 || strcmp(n->tag, "label") == 0 ||
               strcmp(n->tag, "small") == 0) {
      st->display = WEB_DISP_INLINE;
      if (strcmp(n->tag, "b") == 0 || strcmp(n->tag, "strong") == 0) st->font_bold = true;
    } else if (strcmp(n->tag, "table") == 0) {
      st->display = WEB_DISP_TABLE;
    } else if (strcmp(n->tag, "tr") == 0) {
      st->display = WEB_DISP_TABLE_ROW;
    } else if (strcmp(n->tag, "td") == 0 || strcmp(n->tag, "th") == 0) {
      st->display = WEB_DISP_TABLE_CELL;
      st->vertical_align = 1;
    } else if (strcmp(n->tag, "button") == 0) {
      st->display = WEB_DISP_INLINE_BLOCK;
      st->padding[0] = st->padding[2] = 7.0f;
      st->padding[1] = st->padding[3] = 14.0f;
      st->border_radius = 4.0f;
      st->bg = (ccord_color){88, 101, 242, 255};
      st->has_bg = true;
      st->color = (ccord_color){255, 255, 255, 255};
      st->font_bold = true;
    } else if (strcmp(n->tag, "input") == 0) {
      st->display = WEB_DISP_INLINE_BLOCK;
      const char *tp = web_node_get_attr(n, "type");
      if (tp && ascii_ieq(tp, "hidden")) {
        st->display = WEB_DISP_NONE;
      } else if (tp && ascii_ieq(tp, "checkbox")) {
        st->has_width = st->has_height = true;
        st->width = st->height = 24.0f;
        st->border_radius = 4.0f;
        st->border_w[0] = st->border_w[1] = st->border_w[2] = st->border_w[3] = 2.0f;
        st->border_color = (ccord_color){140, 145, 155, 255};
        st->has_border_color = true;
      } else {
        st->has_width = true; st->width = 200.0f;
        st->has_height = true; st->height = 32.0f;
        st->padding[0] = st->padding[2] = 6.0f;
        st->padding[1] = st->padding[3] = 8.0f;
        st->border_w[0] = st->border_w[1] = st->border_w[2] = st->border_w[3] = 1.0f;
        st->border_color = (ccord_color){100, 105, 115, 255};
        st->has_border_color = true;
        st->border_radius = 4.0f;
      }
    } else if (strcmp(n->tag, "h1") == 0) { st->font_size = 22.0f; st->font_bold = true; st->margin[2] = 8.0f; }
    else if (strcmp(n->tag, "h2") == 0) { st->font_size = 18.0f; st->font_bold = true; st->margin[2] = 6.0f; }
    else if (strcmp(n->tag, "h3") == 0) { st->font_size = 15.0f; st->font_bold = true; st->margin[2] = 4.0f; }
    else if (strcmp(n->tag, "p") == 0) { st->margin[2] = 6.0f; }
    else if (strcmp(n->tag, "iframe") == 0 || strcmp(n->tag, "canvas") == 0 || strcmp(n->tag, "img") == 0 ||
             strcmp(n->tag, "svg") == 0) {
      st->display = WEB_DISP_INLINE_BLOCK;
      const char *wa = web_node_get_attr(n, "width"), *ha = web_node_get_attr(n, "height");
      if (wa && wa[0]) {
        st->has_width = parse_length_ex(wa, cw, vw, vh, st->font_size, &st->width, &st->width_pct, &st->width_pct_val);
      }
      if (ha && ha[0]) {
        st->has_height = parse_length_ex(ha, ch, vw, vh, st->font_size, &st->height, &st->height_pct, &st->height_pct_val);
      }
      if (strcmp(n->tag, "svg") == 0 && (!st->has_width || !st->has_height)) {
        const char *vb = web_node_get_attr(n, "viewbox");
        if (vb) {
          float vx = 0, vy = 0, vbw = 0, vbh = 0;
          if (sscanf(vb, "%f %f %f %f", &vx, &vy, &vbw, &vbh) == 4 && vbw > 0 && vbh > 0) {
            if (!st->has_width) { st->has_width = true; st->width = vbw; }
            if (!st->has_height) { st->has_height = true; st->height = vbh; }
          }
        }
      }
    }

    // Collect matching rules and stable-sort by specificity ascending
    if (st->display != WEB_DISP_NONE || strcmp(n->tag, "div") == 0) {
      size_t matched[128];
      size_t nm = 0;
      bool dark = f->view->cfg.dark_theme;
      for (size_t i = 0; i < f->sheet.nrules && nm < 128u; i++) {
        const web_css_rule *rule = &f->sheet.rules[i];
        if (rule->media_query && !media_query_matches(rule->media_query, vw, vh, dark)) continue;
        if (web_dom_matches_selector(n, rule->selector)) {
          matched[nm++] = i;
        }
      }
      // Stable insertion sort by specificity ascending
      for (size_t i = 1; i < nm; i++) {
        size_t idx = matched[i];
        int sp = f->sheet.rules[idx].specificity;
        size_t j = i;
        while (j > 0 && f->sheet.rules[matched[j - 1u]].specificity > sp) {
          matched[j] = matched[j - 1u];
          j--;
        }
        matched[j] = idx;
      }

      // Pass 1: collect CSS custom properties (--*)
      for (size_t i = 0; i < nm; i++) {
        const web_css_rule *rule = &f->sheet.rules[matched[i]];
        for (size_t j = 0; j < rule->nprops; j++) {
          if (rule->props[j].name[0] == '-' && rule->props[j].name[1] == '-') {
            node_set_var(n, rule->props[j].name, rule->props[j].value);
          }
        }
      }
      for (size_t j = 0; j < n->ninline; j++) {
        if (n->inline_props[j].name[0] == '-' && n->inline_props[j].name[1] == '-') {
          node_set_var(n, n->inline_props[j].name, n->inline_props[j].value);
        }
      }

      // Pass 2: apply matched rules in specificity order, then inline styles
      for (size_t i = 0; i < nm; i++) {
        const web_css_rule *rule = &f->sheet.rules[matched[i]];
        for (size_t j = 0; j < rule->nprops; j++) {
          apply_decl_with_vars(n, st, rule->props[j].name, rule->props[j].value, cw, ch, vw, vh);
        }
      }
      for (size_t j = 0; j < n->ninline; j++) {
        apply_decl_with_vars(n, st, n->inline_props[j].name, n->inline_props[j].value, cw, ch, vw, vh);
      }
    }

    // Trigger lazy background-image fetch if present
    if (st->bg_image_url[0]) {
      if (!n->img) {
        n->img = ccord_calloc(1, sizeof *n->img);
        if (n->img) snprintf(n->img->tex_key, sizeof n->img->tex_key, "web_bg_%u_%u", f->id, n->id);
      }
      if (n->img) web_net_fetch_image(f, n, n->img, st->bg_image_url);
    }
  }

  float child_cw = (st->has_width && !st->width_pct && st->width > 0.0f) ? st->width : cw;
  float child_ch = (st->has_height && !st->height_pct && st->height > 0.0f) ? st->height : ch;
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    compute_style_tree(f, c, st, child_cw, child_ch, vw, vh);
  }
}

// ---- HTML text whitespace collapsing & measurement ----

static bool is_ws_only(const char *s)
{
  if (!s) return true;
  while (*s) { if (!isspace((unsigned char)*s)) return false; s++; }
  return true;
}

static size_t collapse_html_text(const char *src, char *dst, size_t cap, bool preserve)
{
  if (!src || !dst || cap == 0) return 0;
  if (preserve) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
  }
  size_t w = 0;
  bool in_ws = true; // trim leading whitespace
  for (const char *p = src; *p && w + 1u < cap; p++) {
    if (isspace((unsigned char)*p)) {
      if (!in_ws) {
        dst[w++] = ' ';
        in_ws = true;
      }
    } else {
      dst[w++] = *p;
      in_ws = false;
    }
  }
  while (w > 0 && dst[w - 1u] == ' ') w--;
  dst[w] = '\0';
  return w;
}

static ccord_vec2 measure_text_safe(ccord_renderer *r, const char *s, size_t n, float fs, bool bold, float wrap_w)
{
  if (!s || n == 0) return (ccord_vec2){0.0f, 0.0f};
  if (r) {
    ccord_text_style ts = {.font = bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, .size = fs, .color = {255, 255, 255, 255}};
    return ccord_text_measure(r, s, n, &ts, wrap_w);
  }
  float cw = fs * 0.56f;
  float total = (float)n * cw;
  if (wrap_w > 0.0f && total > wrap_w) {
    float lines = ceilf(total / wrap_w);
    return (ccord_vec2){wrap_w, lines * (fs * 1.35f)};
  }
  return (ccord_vec2){total, fs * 1.35f};
}

// ---- Box / Flex / Grid / Table / Positioned Layout ----

static void shift_subtree(web_node *n, float dx, float dy)
{
  if (!n || (fabsf(dx) < 0.01f && fabsf(dy) < 0.01f)) return;
  n->box.x += dx;
  n->box.y += dy;
  if (n->child_frame) return; // child frames paint in iframe-local coordinates
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    shift_subtree(c, dx, dy);
  }
}

static bool is_inline_flow(const web_node *c)
{
  if (c->type == WEB_NODE_TEXT) return true;
  if (c->type == WEB_NODE_ELEMENT) {
    if (c->computed.float_dir != 0) return false;
    return c->computed.display == WEB_DISP_INLINE ||
           c->computed.display == WEB_DISP_INLINE_BLOCK ||
           c->computed.is_inline_level;
  }
  return false;
}

static float estimate_intrinsic_width(const web_node *n, ccord_renderer *r)
{
  if (!n) return 0.0f;
  const web_style *st = &n->computed;
  if (n->type == WEB_NODE_TEXT) {
    if (is_ws_only(n->text)) return 0.0f;
    char norm[512];
    size_t nl = collapse_html_text(n->text, norm, sizeof norm, st->white_space_pre);
    return nl > 0 ? measure_text_safe(r, norm, nl, st->font_size, st->font_bold, 0.0f).x : 0.0f;
  }
  if (st->has_width && !st->width_pct) {
    float ew = st->width;
    if (st->min_w > 0.0f && ew < st->min_w) ew = st->min_w;
    if (st->max_w > 0.0f && ew > st->max_w) ew = st->max_w;
    return ew;
  }
  float pad = st->padding[1] + st->padding[3] + st->border_w[1] + st->border_w[3];
  float w = 0.0f;
  if (n->canvas) {
    w = (float)web_canvas_width(n->canvas) + pad;
  } else if (n->img && n->img->loaded && n->tag && strcmp(n->tag, "img") == 0) {
    w = (float)n->img->w + pad;
  } else if (n->tag && strcmp(n->tag, "iframe") == 0) {
    float iw = (n->child_frame && n->child_frame->viewport_w > 0.0f) ? n->child_frame->viewport_w : 360.0f;
    w = iw + pad;
  } else if (n->tag && strcmp(n->tag, "svg") == 0 && st->has_width) {
    w = st->width;
  } else {
    // Sum horizontal runs (flex-row, table-row, inline, or consecutive inline/floated siblings) and max across rows
    bool horiz = (st->display == WEB_DISP_FLEX && st->flex_dir == WEB_FLEX_ROW) ||
                 st->display == WEB_DISP_TABLE_ROW || st->display == WEB_DISP_INLINE ||
                 st->display == WEB_DISP_INLINE_BLOCK;
    float run_w = 0.0f;
    size_t cnt = 0;
    for (const web_node *c = n->first_child; c; c = c->next_sib) {
      if (c->type == WEB_NODE_COMMENT) continue;
      if (c->type == WEB_NODE_ELEMENT &&
          (c->computed.display == WEB_DISP_NONE || c->computed.position == WEB_POS_ABSOLUTE ||
           c->computed.position == WEB_POS_FIXED)) continue;
      float cw = estimate_intrinsic_width(c, r);
      if (cw <= 0.0f) continue;
      cw += c->computed.margin[1] + c->computed.margin[3];
      if (horiz) {
        w += cw;
        cnt++;
      } else if (is_inline_flow(c) || (c->type == WEB_NODE_ELEMENT && c->computed.float_dir != 0)) {
        run_w += cw;
        if (run_w > w) w = run_w;
      } else {
        run_w = 0.0f;
        if (cw > w) w = cw;
      }
    }
    if (horiz && cnt > 1u) w += st->gap * (float)(cnt - 1u);
    if (w > 0.0f) w += pad;
    else if (st->min_w > 0.0f) w = st->min_w;
  }
  if (w > 0.0f) {
    if (st->min_w > 0.0f && w < st->min_w) w = st->min_w;
    if (st->max_w > 0.0f && w > st->max_w) w = st->max_w;
  }
  return w;
}

static void layout_node(web_frame *f, web_node *n, ccord_renderer *r,
                        float x, float y, float avail_w, float avail_h, ccord_rect pos_cb)
{
  const web_style *st = &n->computed;
  if (n->type == WEB_NODE_COMMENT || (n->type == WEB_NODE_ELEMENT && st->display == WEB_DISP_NONE)) {
    n->box = (ccord_rect){x, y, 0, 0};
    n->box_valid = false;
    return;
  }

  if (n->type == WEB_NODE_TEXT) {
    if (is_ws_only(n->text)) {
      n->box = (ccord_rect){x, y, 0, 0};
      n->box_valid = false;
      return;
    }
    char norm[1024];
    size_t nl = collapse_html_text(n->text, norm, sizeof norm, st->white_space_pre);
    if (nl == 0) {
      n->box = (ccord_rect){x, y, 0, 0};
      n->box_valid = false;
      return;
    }
    float wrap = (st->white_space_nowrap || avail_w <= 4.0f) ? 0.0f : avail_w;
    ccord_vec2 sz = measure_text_safe(r, norm, nl, st->font_size, st->font_bold, wrap);
    if (st->line_height > sz.y) sz.y = st->line_height;
    n->box = (ccord_rect){x, y, sz.x, sz.y};
    n->box_valid = true;
    return;
  }

  if (n->type == WEB_NODE_DOCUMENT) {
    n->box = (ccord_rect){0, 0, avail_w, avail_h};
    n->box_valid = true;
    ccord_rect icb = {0, 0, f->viewport_w, f->viewport_h};
    for (web_node *c = n->first_child; c; c = c->next_sib) {
      layout_node(f, c, r, 0, 0, avail_w, avail_h, icb);
    }
    return;
  }

  float bx = x + st->margin[3];
  float by = y + st->margin[0];
  float max_avail = fmaxf(0.0f, avail_w - st->margin[3] - st->margin[1]);
  float bw = 0.0f;
  bool is_abs_or_fixed = (st->position == WEB_POS_ABSOLUTE || st->position == WEB_POS_FIXED);
  if (st->has_width) {
    bw = st->width_pct ? (avail_w * st->width_pct_val * 0.01f) : st->width;
  } else {
    bw = max_avail;
  }

  // Intrinsic / CSS shrink-to-fit widths for replaced, inline-level, floated, table, and positioned elements
  if (!st->has_width) {
    bool shrink_to_fit = st->width_fit_content ||
                         st->display == WEB_DISP_INLINE ||
                         st->display == WEB_DISP_INLINE_BLOCK ||
                         st->display == WEB_DISP_TABLE ||
                         st->is_inline_level ||
                         st->float_dir != 0 ||
                         (is_abs_or_fixed && !(st->has_left && st->has_right));
    if (n->canvas) {
      bw = (float)web_canvas_width(n->canvas);
    } else if (n->img && n->img->loaded && n->tag && strcmp(n->tag, "img") == 0) {
      bw = (float)n->img->w;
    } else if (n->tag && strcmp(n->tag, "iframe") == 0) {
      float iw = (n->child_frame && n->child_frame->viewport_w > 0.0f) ? n->child_frame->viewport_w : 360.0f;
      bw = max_avail > 0.0f ? fminf(max_avail, iw) : iw;
    } else if (shrink_to_fit) {
      float intr = estimate_intrinsic_width(n, r);
      if (intr > 0.0f) {
        bw = (max_avail > 0.0f && intr > max_avail) ? max_avail : intr;
      } else if (is_abs_or_fixed && !(st->has_left && st->has_right)) {
        bw = st->padding[1] + st->padding[3] + st->border_w[1] + st->border_w[3];
      }
    }
  }

  // Never let a transient 0px inline width on <html> collapse the entire iframe document to 0px wide
  if (n->tag && strcmp(n->tag, "html") == 0 && bw < 1.0f && f->viewport_w > 1.0f) {
    bw = f->viewport_w;
  }
  if (st->min_w > 0.0f && bw < st->min_w) bw = st->min_w;
  if (st->max_w > 0.0f && bw > st->max_w) bw = st->max_w;

  float pad_l = st->padding[3] + st->border_w[3];
  float pad_r = st->padding[1] + st->border_w[1];
  float pad_t = st->padding[0] + st->border_w[0];
  float pad_b = st->padding[2] + st->border_w[2];
  float inner_w = fmaxf(0.0f, bw - pad_l - pad_r);
  float inner_x = bx + pad_l;
  float inner_y = by + pad_t;
  float explicit_h = 0.0f;
  bool has_explicit_h = st->has_height;
  if (has_explicit_h) {
    explicit_h = st->height_pct ? (avail_h * st->height_pct_val * 0.01f) : st->height;
    if (n->tag && strcmp(n->tag, "html") == 0 && explicit_h < 1.0f && f->viewport_h > 1.0f) {
      explicit_h = f->viewport_h;
    }
  } else if (is_abs_or_fixed && st->has_top && st->has_bottom) {
    has_explicit_h = true;
    explicit_h = fmaxf(0.0f, avail_h - st->margin[0] - st->margin[2]);
  }
  float child_avail_h = has_explicit_h ? fmaxf(0.0f, explicit_h - pad_t - pad_b) : avail_h;

  // Determine the containing block for positioned descendants
  ccord_rect child_cb = pos_cb;
  if (st->position != WEB_POS_STATIC) {
    child_cb = (ccord_rect){bx, by, bw, has_explicit_h ? explicit_h : child_avail_h};
  }

  // Collect flow vs absolute children
  web_node *flow[128], *abs_c[64];
  size_t nf = 0, na = 0;
  bool any_table_cell = false;
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    if (c->type == WEB_NODE_COMMENT) continue;
    if (c->type == WEB_NODE_ELEMENT && c->computed.display == WEB_DISP_NONE) continue;
    if (c->type == WEB_NODE_TEXT && is_ws_only(c->text)) continue;
    if (c->type == WEB_NODE_ELEMENT && (c->computed.position == WEB_POS_ABSOLUTE || c->computed.position == WEB_POS_FIXED)) {
      if (na < 64u) abs_c[na++] = c;
    } else {
      if (c->type == WEB_NODE_ELEMENT && c->computed.display == WEB_DISP_TABLE_CELL) any_table_cell = true;
      if (nf < 128u) flow[nf++] = c;
    }
  }
  if (st->display == WEB_DISP_FLEX && st->flex_reverse && nf > 1u) {
    for (size_t lo = 0, hi = nf - 1u; lo < hi; lo++, hi--) {
      web_node *tmp = flow[lo]; flow[lo] = flow[hi]; flow[hi] = tmp;
    }
  }

  float content_h = 0.0f;
  bool is_horiz_flex = (st->display == WEB_DISP_FLEX && st->flex_dir == WEB_FLEX_ROW);
  bool is_table_horiz = (st->display == WEB_DISP_TABLE_ROW) || (st->display == WEB_DISP_TABLE && any_table_cell);

  if (st->display == WEB_DISP_GRID && st->grid_cols > 0 && nf > 0) {
    int cols = st->grid_cols;
    float gap = st->gap;
    float cell_w = fmaxf(1.0f, (inner_w - gap * (float)(cols - 1)) / (float)cols);
    float cy = inner_y;
    for (size_t i = 0; i < nf; i += (size_t)cols) {
      float row_h = 0.0f;
      for (int c = 0; c < cols && i + (size_t)c < nf; c++) {
        web_node *ch = flow[i + (size_t)c];
        float cx = inner_x + (float)c * (cell_w + gap);
        layout_node(f, ch, r, cx, cy, cell_w, child_avail_h, child_cb);
        if (ch->box.h > row_h) row_h = ch->box.h;
      }
      cy += row_h + gap;
    }
    content_h = fmaxf(0.0f, cy - inner_y - (nf > 0 ? gap : 0.0f));
  } else if ((is_horiz_flex || is_table_horiz) && nf > 0) {
    float gap = is_horiz_flex ? st->gap : 0.0f;
    float total_gap = nf > 1u ? gap * (float)(nf - 1u) : 0.0f;

    // Smart width distribution: give fixed/intrinsic widths first, then split remaining space among flex-grow / auto items.
    float hints[128] = {0};
    float fixed_sum = 0.0f;
    float grow_sum = 0.0f;
    size_t auto_cnt = 0;
    for (size_t i = 0; i < nf; i++) {
      web_node *ch = flow[i];
      float m_h = ch->computed.margin[1] + ch->computed.margin[3];
      if (ch->computed.has_width) {
        hints[i] = ch->computed.width_pct ? (inner_w * ch->computed.width_pct_val * 0.01f) : ch->computed.width;
        fixed_sum += hints[i] + m_h;
      } else if (ch->computed.flex_grow > 0.0f) {
        grow_sum += ch->computed.flex_grow;
        fixed_sum += m_h;
      } else {
        float intr = estimate_intrinsic_width(ch, r);
        if (intr > 0.0f && intr < inner_w) {
          hints[i] = intr;
          fixed_sum += intr + m_h;
        } else {
          auto_cnt++;
          fixed_sum += m_h;
        }
      }
    }
    float rem = fmaxf(0.0f, inner_w - total_gap - fixed_sum);
    for (size_t i = 0; i < nf; i++) {
      if (hints[i] <= 0.0f) {
        if (grow_sum > 0.0f && flow[i]->computed.flex_grow > 0.0f) {
          hints[i] = fmaxf(1.0f, rem * (flow[i]->computed.flex_grow / grow_sum));
        } else if (auto_cnt > 0) {
          hints[i] = fmaxf(1.0f, rem / (float)auto_cnt);
        } else {
          hints[i] = fmaxf(1.0f, inner_w / (float)nf);
        }
      } else if (grow_sum > 0.0f && flow[i]->computed.flex_grow > 0.0f && !flow[i]->computed.has_width) {
        hints[i] += rem * (flow[i]->computed.flex_grow / grow_sum);
      }
    }

    float cx = inner_x, cy = inner_y, row_h = 0.0f;
    size_t line_start = 0;
    for (size_t i = 0; i < nf; i++) {
      web_node *ch = flow[i];
      float cw_hint = st->flex_wrap ? (ch->computed.has_width ? hints[i] : inner_w) : hints[i];
      if (st->flex_wrap && cx > inner_x && (cx - inner_x) + cw_hint > inner_w + 1.0f) {
        cx = inner_x;
        cy += row_h + gap;
        row_h = 0.0f;
        line_start = i;
      }
      layout_node(f, ch, r, cx, cy, cw_hint, child_avail_h, child_cb);
      cx += ch->box.w + ch->computed.margin[1] + ch->computed.margin[3] + gap;
      float th = ch->box.h + ch->computed.margin[0] + ch->computed.margin[2];
      if (th > row_h) row_h = th;
    }

    // Horizontal distribution (single line): auto margins take precedence per CSS Flexbox §8.1
    if (!st->flex_wrap && line_start == 0) {
      float used = (cx - inner_x) - gap;
      float extra = fmaxf(0.0f, inner_w - used);
      size_t auto_slots = 0;
      for (size_t i = 0; i < nf; i++) {
        if (flow[i]->computed.margin_auto_l) auto_slots++;
        if (flow[i]->computed.margin_auto_r) auto_slots++;
      }
      if (auto_slots > 0u && extra > 0.5f) {
        float unit = extra / (float)auto_slots;
        float acc = 0.0f;
        for (size_t i = 0; i < nf; i++) {
          if (flow[i]->computed.margin_auto_l) acc += unit;
          if (acc > 0.01f) shift_subtree(flow[i], acc, 0.0f);
          if (flow[i]->computed.margin_auto_r) acc += unit;
        }
      } else if (st->justify_content == WEB_ALIGN_CENTER && extra > 0.5f) {
        for (size_t i = 0; i < nf; i++) shift_subtree(flow[i], extra * 0.5f, 0.0f);
      } else if (st->justify_content == WEB_ALIGN_END && extra > 0.5f) {
        for (size_t i = 0; i < nf; i++) shift_subtree(flow[i], extra, 0.0f);
      } else if (st->justify_content == WEB_ALIGN_SPACE_BETWEEN && nf > 1u && extra > 0.5f) {
        float step = extra / (float)(nf - 1u);
        for (size_t i = 1; i < nf; i++) shift_subtree(flow[i], step * (float)i, 0.0f);
      } else if (st->justify_content == WEB_ALIGN_SPACE_AROUND && nf > 0u && extra > 0.5f) {
        float step = extra / (float)nf;
        for (size_t i = 0; i < nf; i++) shift_subtree(flow[i], step * ((float)i + 0.5f), 0.0f);
      }
    }

    content_h = (cy - inner_y) + row_h;
    float align_h = has_explicit_h ? fmaxf(row_h, child_avail_h) : row_h;
    if (!st->flex_wrap && line_start == 0) {
      for (size_t i = 0; i < nf; i++) {
        web_node *ch = flow[i];
        uint8_t as = ch->computed.align_self;
        bool center_v = (is_horiz_flex && ((as == 2) || (as == 0 && st->align_items == WEB_ALIGN_CENTER))) ||
                        (is_table_horiz && ch->computed.vertical_align == 1);
        bool end_v = (is_horiz_flex && ((as == 3) || (as == 0 && st->align_items == WEB_ALIGN_END))) ||
                     (is_table_horiz && ch->computed.vertical_align == 2);
        float ch_tot = ch->box.h + ch->computed.margin[0] + ch->computed.margin[2];
        if (center_v && ch_tot < align_h) {
          shift_subtree(ch, 0.0f, (align_h - ch_tot) * 0.5f);
        } else if (end_v && ch_tot < align_h) {
          shift_subtree(ch, 0.0f, align_h - ch_tot);
        }
      }
    }
  } else {
    // Block or flex-column flow (with float bands and inline horizontal runs inside block containers)
    float cy = inner_y;
    float gap = (st->display == WEB_DISP_FLEX) ? st->gap : 0.0f;
    bool allow_inline_run = (st->display != WEB_DISP_FLEX);
    size_t i = 0;
    while (i < nf) {
      if (allow_inline_run && flow[i]->type == WEB_NODE_ELEMENT && flow[i]->computed.float_dir != 0) {
        // Pack consecutive floated siblings: float:left left-to-right, float:right right-to-left
        float left_x = inner_x, right_x = inner_x + inner_w, band_h = 0.0f;
        while (i < nf && flow[i]->type == WEB_NODE_ELEMENT && flow[i]->computed.float_dir != 0) {
          web_node *ch = flow[i];
          if (ch->computed.clear_dir != 0 && band_h > 0.0f) break;
          layout_node(f, ch, r, left_x, cy, inner_w, child_avail_h, child_cb);
          if (ch->box_valid) {
            float tw = ch->box.w + ch->computed.margin[1] + ch->computed.margin[3];
            float avail_band = right_x - left_x;
            if (tw > avail_band + 1.0f && (left_x > inner_x || right_x < inner_x + inner_w)) {
              cy += band_h;
              left_x = inner_x;
              right_x = inner_x + inner_w;
              band_h = 0.0f;
              layout_node(f, ch, r, left_x, cy, inner_w, child_avail_h, child_cb);
            }
            if (ch->computed.float_dir == 2) {
              float rx_target = right_x - ch->computed.margin[1] - ch->box.w;
              float ch_tx = ch->computed.translate_x_pct ? (ch->box.w * ch->computed.translate_x * 0.01f) : ch->computed.translate_x;
              float base_x = ch->box.x - ch_tx;
              shift_subtree(ch, rx_target - base_x, 0.0f);
              right_x -= tw;
            } else {
              left_x += tw;
            }
            float th = ch->box.h + ch->computed.margin[0] + ch->computed.margin[2];
            if (th > band_h) band_h = th;
          }
          i++;
        }
        cy += band_h + gap;
      } else if (allow_inline_run && is_inline_flow(flow[i])) {
        // Place consecutive inline/text siblings horizontally, wrapping at inner_w
        float lx = inner_x, row_h = 0.0f;
        size_t run_start = i;
        while (i < nf && is_inline_flow(flow[i])) {
          web_node *ch = flow[i];
          float rem_w = fmaxf(1.0f, inner_w - (lx - inner_x));
          layout_node(f, ch, r, lx, cy, rem_w, child_avail_h, child_cb);
          if (ch->box_valid) {
            if (lx > inner_x && (lx - inner_x) + ch->box.w > inner_w + 1.0f) {
              // Wrap to next line
              cy += row_h;
              lx = inner_x;
              row_h = 0.0f;
              layout_node(f, ch, r, lx, cy, inner_w, child_avail_h, child_cb);
            }
            lx += ch->box.w + ch->computed.margin[1] + ch->computed.margin[3];
            float th = ch->box.h + ch->computed.margin[0] + ch->computed.margin[2];
            if (th > row_h) row_h = th;
          }
          i++;
        }
        // Align inline run if text-align: center/right on parent
        if (st->text_align == 1 && lx > inner_x && (lx - inner_x) < inner_w) {
          float dx = (inner_w - (lx - inner_x)) * 0.5f;
          for (size_t k = run_start; k < i; k++) shift_subtree(flow[k], dx, 0.0f);
        } else if (st->text_align == 2 && lx > inner_x && (lx - inner_x) < inner_w) {
          float dx = inner_w - (lx - inner_x);
          for (size_t k = run_start; k < i; k++) shift_subtree(flow[k], dx, 0.0f);
        }
        cy += row_h + gap;
      } else {
        web_node *ch = flow[i++];
        float flow_y = cy;
        layout_node(f, ch, r, inner_x, cy, inner_w, child_avail_h, child_cb);
        if (ch->box_valid) {
          uint8_t as = ch->computed.align_self;
          bool center_h = (st->display == WEB_DISP_FLEX && ((as == 2) || (as == 0 && st->align_items == WEB_ALIGN_CENTER))) ||
                          (ch->computed.margin_auto_l && ch->computed.margin_auto_r);
          bool end_h = (st->display == WEB_DISP_FLEX && ((as == 3) || (as == 0 && st->align_items == WEB_ALIGN_END))) ||
                       (ch->computed.margin_auto_l && !ch->computed.margin_auto_r);
          if (center_h && ch->box.w < inner_w) {
            float dx = (inner_w - ch->box.w) * 0.5f;
            shift_subtree(ch, dx, 0.0f);
          } else if (end_h && ch->box.w < inner_w) {
            float dx = inner_w - ch->box.w - ch->computed.margin[1];
            if (dx > 0.0f) shift_subtree(ch, dx, 0.0f);
          }
          // Advance flow Y using unshifted flow position so position:relative / transform don't distort sibling flow
          cy = flow_y + ch->computed.margin[0] + ch->box.h + ch->computed.margin[2] + gap;
        }
      }
    }
    content_h = fmaxf(0.0f, cy - inner_y - (nf > 0 ? gap : 0.0f));
  }

  float bh = has_explicit_h ? explicit_h : (content_h + pad_t + pad_b);
  if (!has_explicit_h) {
    if (n->canvas) bh = (float)web_canvas_height(n->canvas) + pad_t + pad_b;
    else if (n->img && n->img->loaded && n->tag && strcmp(n->tag, "img") == 0) {
      bh = (bw > 0 && n->img->w > 0) ? (bw * (float)n->img->h / (float)n->img->w) : (float)n->img->h;
    } else if (n->tag && strcmp(n->tag, "iframe") == 0) {
      bh = (n->child_frame && n->child_frame->viewport_h > 0) ? n->child_frame->viewport_h : 80.0f;
    } else if (n->tag && (strcmp(n->tag, "html") == 0 || strcmp(n->tag, "body") == 0) && bh < f->viewport_h) {
      // Ensure root html/body at least covers the viewport height so children & backgrounds aren't clipped to 0
      bh = f->viewport_h;
    }
  }
  if (st->min_h > 0.0f && bh < st->min_h) bh = st->min_h;
  if (st->max_h > 0.0f && bh > st->max_h) bh = st->max_h;

  // Vertical alignment for flex-column or table-cell inside explicit height
  if (has_explicit_h && content_h < bh - pad_t - pad_b) {
    float extra_v = (bh - pad_t - pad_b) - content_h;
    if ((st->display == WEB_DISP_FLEX && st->flex_dir == WEB_FLEX_COLUMN && st->justify_content == WEB_ALIGN_CENTER) ||
        (st->display == WEB_DISP_TABLE_CELL && st->vertical_align == 1)) {
      for (size_t i = 0; i < nf; i++) shift_subtree(flow[i], 0.0f, extra_v * 0.5f);
    } else if ((st->display == WEB_DISP_FLEX && st->flex_dir == WEB_FLEX_COLUMN && st->justify_content == WEB_ALIGN_END) ||
               (st->display == WEB_DISP_TABLE_CELL && st->vertical_align == 2)) {
      for (size_t i = 0; i < nf; i++) shift_subtree(flow[i], 0.0f, extra_v);
    }
  }

  n->box = (ccord_rect){bx, by, bw, bh};
  n->box_valid = true;

  // Apply CSS position: relative visual offsets (top/left/right/bottom)
  if (st->position == WEB_POS_RELATIVE) {
    float rx_off = 0.0f, ry_off = 0.0f;
    if (st->has_left) rx_off = st->left_pct ? (avail_w * st->left * 0.01f) : st->left;
    else if (st->has_right) rx_off = -(st->right_pct ? (avail_w * st->right * 0.01f) : st->right);
    if (st->has_top) ry_off = st->top_pct ? (avail_h * st->top * 0.01f) : st->top;
    else if (st->has_bottom) ry_off = -(st->bottom_pct ? (avail_h * st->bottom * 0.01f) : st->bottom);
    if (fabsf(rx_off) >= 0.01f || fabsf(ry_off) >= 0.01f) {
      shift_subtree(n, rx_off, ry_off);
    }
  }

  // Apply CSS transform: translate(x, y) (supporting % of element's own size!)
  float tx = st->translate_x_pct ? (bw * st->translate_x * 0.01f) : st->translate_x;
  float ty = st->translate_y_pct ? (bh * st->translate_y * 0.01f) : st->translate_y;
  if (fabsf(tx) >= 0.01f || fabsf(ty) >= 0.01f) {
    shift_subtree(n, tx, ty);
  }

  // Update positioned containing block with finalized box (bx, by, bw, bh)
  if (st->position != WEB_POS_STATIC) {
    child_cb = n->box;
  }

  // Layout absolute/fixed children relative to nearest positioned ancestor (or frame viewport for fixed/root)
  for (size_t i = 0; i < na; i++) {
    web_node *ch = abs_c[i];
    const web_style *cst = &ch->computed;
    bool is_fixed = (cst->position == WEB_POS_FIXED);
    float rx = is_fixed ? 0.0f : child_cb.x;
    float ry = is_fixed ? 0.0f : child_cb.y;
    float rw = is_fixed ? f->viewport_w : (child_cb.w > 0.0f ? child_cb.w : f->viewport_w);
    float rh = is_fixed ? f->viewport_h : (child_cb.h > 0.0f ? child_cb.h : f->viewport_h);

    float l_px = cst->left_pct ? (rw * cst->left * 0.01f) : cst->left;
    float r_px = cst->right_pct ? (rw * cst->right * 0.01f) : cst->right;
    float t_px = cst->top_pct ? (rh * cst->top * 0.01f) : cst->top;
    float b_px = cst->bottom_pct ? (rh * cst->bottom * 0.01f) : cst->bottom;

    float ax = cst->has_left ? (rx + l_px) : rx;
    float ay = cst->has_top ? (ry + t_px) : ry;
    float aw = (cst->has_left && cst->has_right && !cst->has_width) ? fmaxf(0.0f, rw - l_px - r_px) : rw;
    float ah = (cst->has_top && cst->has_bottom && !cst->has_height) ? fmaxf(0.0f, rh - t_px - b_px) : rh;
    layout_node(f, ch, r, ax, ay, aw, ah, child_cb);
    float ch_tx = cst->translate_x_pct ? (ch->box.w * cst->translate_x * 0.01f) : cst->translate_x;
    float ch_ty = cst->translate_y_pct ? (ch->box.h * cst->translate_y * 0.01f) : cst->translate_y;
    float base_x = ch->box.x - ch_tx;
    float base_y = ch->box.y - ch_ty;
    if (!cst->has_left && cst->has_right) {
      float nx = rx + rw - ch->box.w - r_px - cst->margin[1];
      shift_subtree(ch, nx - base_x, 0.0f);
    } else if (cst->has_left && cst->has_right && cst->has_width && cst->margin_auto_l && cst->margin_auto_r) {
      float span = fmaxf(0.0f, rw - l_px - r_px);
      float nx = rx + l_px + (span - ch->box.w) * 0.5f;
      shift_subtree(ch, nx - base_x, 0.0f);
    }
    if (!cst->has_top && cst->has_bottom) {
      float ny = ry + rh - ch->box.h - b_px - cst->margin[2];
      shift_subtree(ch, 0.0f, ny - base_y);
    }
  }

  // Propagate iframe dimensions into child frame (syncing JS innerWidth/Height + deferred resize event)
  if (n->child_frame && bw > 1.0f && bh > 1.0f) {
    if (fabsf(n->child_frame->viewport_w - bw) > 0.5f || fabsf(n->child_frame->viewport_h - bh) > 0.5f) {
      n->child_frame->viewport_w = bw;
      n->child_frame->viewport_h = bh;
      n->child_frame->style_dirty = true;
      n->child_frame->layout_dirty = true;
      if (n->child_frame->ctx && !f->view->destroying) {
        JS_SetPropertyStr(n->child_frame->ctx, n->child_frame->win_obj, "innerWidth", JS_NewFloat64(n->child_frame->ctx, (double)bw));
        JS_SetPropertyStr(n->child_frame->ctx, n->child_frame->win_obj, "innerHeight", JS_NewFloat64(n->child_frame->ctx, (double)bh));
        JS_SetPropertyStr(n->child_frame->ctx, n->child_frame->win_obj, "outerWidth", JS_NewFloat64(n->child_frame->ctx, (double)bw));
        JS_SetPropertyStr(n->child_frame->ctx, n->child_frame->win_obj, "outerHeight", JS_NewFloat64(n->child_frame->ctx, (double)bh));
        if (!n->child_frame->resize_pending) {
          n->child_frame->resize_pending = true;
          (void)web_add_timer(n->child_frame, JS_UNDEFINED,
                              "try{dispatchEvent(new Event('resize'));}catch(_){}", 0u, false, false);
        }
      }
    }
    web_frame_ensure_layout(n->child_frame, r);
  }
}

void web_frame_ensure_layout(web_frame *f, ccord_renderer *r)
{
  if (!f || !f->doc || f->is_worker) return;
  if (f->style_dirty) {
    compute_style_tree(f, f->doc, nullptr, f->viewport_w, f->viewport_h, f->viewport_w, f->viewport_h);
    f->style_dirty = false;
    f->layout_dirty = true;
  }
  if (f->layout_dirty) {
    ccord_rect icb = {0, 0, f->viewport_w, f->viewport_h};
    layout_node(f, f->doc, r, 0.0f, 0.0f, f->viewport_w, f->viewport_h, icb);
    f->layout_dirty = false;
  }
}

// ---- Diagnostic DOM/Layout Tree Dumper ----

static void dump_node_rec(const web_node *n, int depth, int *budget)
{
  if (!n || *budget <= 0 || depth > 14) return;
  if (n->type == WEB_NODE_COMMENT) return;
  if (n->type == WEB_NODE_ELEMENT && n->computed.display == WEB_DISP_NONE) return;
  if (n->type == WEB_NODE_TEXT && is_ws_only(n->text)) return;

  (*budget)--;
  char ind[32];
  int sp = depth * 2;
  if (sp > 28) sp = 28;
  memset(ind, ' ', (size_t)sp);
  ind[sp] = '\0';

  if (n->type == WEB_NODE_TEXT) {
    char norm[80];
    (void)collapse_html_text(n->text, norm, sizeof norm, n->computed.white_space_pre);
    CCORD_LOG_D("dom: %s\"%.60s\" box=(%.0f,%.0f %.0fx%.0f) col=%02x%02x%02x hid=%d",
                ind, norm, (double)n->box.x, (double)n->box.y, (double)n->box.w, (double)n->box.h,
                n->computed.color.r, n->computed.color.g, n->computed.color.b, n->computed.hidden ? 1 : 0);
    return;
  }
  if (n->type == WEB_NODE_ELEMENT) {
    const char *id = web_node_get_attr(n, "id");
    const char *cls = web_node_get_attr(n, "class");
    CCORD_LOG_D("dom: %s<%s%s%s%s%s> box=(%.0f,%.0f %.0fx%.0f) d=%u p=%u fl=%u z=%d%s op=%.2f hid=%d ov=%d pe=%d bg=%d%s",
                ind, n->tag ? n->tag : "?",
                id ? "#" : "", id ? id : "",
                cls ? "." : "", cls ? cls : "",
                (double)n->box.x, (double)n->box.y, (double)n->box.w, (double)n->box.h,
                (unsigned)n->computed.display, (unsigned)n->computed.position,
                (unsigned)n->computed.float_dir,
                n->computed.z_index, n->computed.has_z_index ? "!" : "",
                (double)n->computed.opacity, n->computed.hidden ? 1 : 0,
                n->computed.overflow_hidden ? 1 : 0, n->computed.pointer_none ? 0 : 1,
                n->computed.has_bg ? 1 : 0,
                (n->computed.bg_image_url[0] || (n->img && n->img->svg_xml)) ? " bgimg=1" : "");
    // If <html> is smaller than its frame viewport, log matching rules to diagnose any layout mismatch
    if (n->tag && strcmp(n->tag, "html") == 0 && n->frame && n->box.w < n->frame->viewport_w - 5.0f) {
      for (size_t ri = 0; ri < n->frame->sheet.nrules; ri++) {
        const web_css_rule *rl = &n->frame->sheet.rules[ri];
        if (web_dom_matches_selector(n, rl->selector)) {
          CCORD_LOG_D("dom:   [html rule #%zu] sel='%s' mq='%s' nprops=%zu",
                      ri, rl->selector, rl->media_query ? rl->media_query : "", rl->nprops);
          for (size_t pi = 0; pi < rl->nprops && pi < 10u; pi++) {
            CCORD_LOG_D("dom:     %s: %s", rl->props[pi].name, rl->props[pi].value);
          }
        }
      }
      for (size_t pi = 0; pi < n->ninline; pi++) {
        CCORD_LOG_D("dom:   [html inline] %s: %s", n->inline_props[pi].name, n->inline_props[pi].value);
      }
    }
  }
  if (n->child_frame) {
    CCORD_LOG_D("dom: %s  [child frame f%u %.0fx%.0f nodes=%zu rules=%zu]",
                ind, n->child_frame->id, (double)n->child_frame->viewport_w, (double)n->child_frame->viewport_h,
                n->child_frame->all_nodes_len, n->child_frame->sheet.nrules);
    if (n->child_frame->doc) dump_node_rec(n->child_frame->doc, depth + 2, budget);
    return;
  }
  for (const web_node *c = n->first_child; c; c = c->next_sib) {
    dump_node_rec(c, depth + 1, budget);
  }
}

void web_frame_dump_tree(web_frame *f)
{
  if (!f || !f->doc) return;
  CCORD_LOG_D("dom: === frame f%u (%.0fx%.0f, %zu nodes, %zu css rules) %s ===",
              f->id, (double)f->viewport_w, (double)f->viewport_h,
              f->all_nodes_len, f->sheet.nrules, f->url.hash[0] ? f->url.hash : f->url.pathname);
  int budget = 160;
  dump_node_rec(f->doc, 0, &budget);
}

// ---- SDL3 Painter ----

static void paint_svg_children(web_node *n, ccord_renderer *r, ccord_rect rc,
                               float vx, float vy, float sx, float sy, ccord_color inherit_col);

static void paint_svg_node(web_node *svg, ccord_renderer *r, ccord_rect rc, ccord_color fallback_col)
{
  if (!svg || rc.w <= 0.5f || rc.h <= 0.5f) return;
  float vx = 0, vy = 0, vbw = rc.w, vbh = rc.h;
  const char *vb = web_node_get_attr(svg, "viewbox");
  if (vb) (void)sscanf(vb, "%f %f %f %f", &vx, &vy, &vbw, &vbh);
  if (vbw <= 0.1f) {
    const char *wa = web_node_get_attr(svg, "width");
    vbw = (wa && strtod(wa, nullptr) > 0.1) ? (float)strtod(wa, nullptr) : rc.w;
  }
  if (vbh <= 0.1f) {
    const char *ha = web_node_get_attr(svg, "height");
    vbh = (ha && strtod(ha, nullptr) > 0.1) ? (float)strtod(ha, nullptr) : rc.h;
  }
  if (vbw <= 0.1f) vbw = rc.w;
  if (vbh <= 0.1f) vbh = rc.h;
  ccord_color inherit_c = svg->computed.has_svg_fill ? svg->computed.svg_fill : fallback_col;
  paint_svg_children(svg, r, rc, vx, vy, rc.w / vbw, rc.h / vbh, inherit_c);
}

static void paint_image(web_frame *f, web_image *img, ccord_renderer *r, ccord_rect dst, float radius, bool cover,
                        ccord_color fallback_col)
{
  if (!img || !img->loaded || dst.w <= 0.0f || dst.h <= 0.0f) return;
  if (img->svg_xml) {
    if (!img->svg_root && f) {
      web_node *frag = web_node_create(f, WEB_NODE_FRAGMENT, "#svg-img");
      if (frag && web_dom_parse_fragment_ex(f, frag, img->svg_xml, strlen(img->svg_xml), true)) {
        img->svg_root = web_dom_query_one(frag, "svg");
        if (img->svg_root) {
          // Parse any inline styles on the SVG subtree so fill/stroke work cleanly
          compute_style_tree(f, img->svg_root, nullptr, dst.w, dst.h, f->viewport_w, f->viewport_h);
        }
      }
    }
    if (img->svg_root) {
      paint_svg_node(img->svg_root, r, dst, fallback_col);
    }
    return;
  }
  if (!img->rgba) return;
  if (!img->uploaded || !ccord_render_has_texture(r, img->tex_key, nullptr, nullptr)) {
    ccord_error err = {0};
    if (ccord_render_upload_rgba(r, img->tex_key, img->rgba, img->w, img->h, &err) == CCORD_OK) {
      img->uploaded = true;
    }
  }
  if (cover) ccord_draw_texture_cover(r, img->tex_key, dst, radius, 255, false);
  else if (radius > 0.5f) ccord_draw_texture_rounded(r, img->tex_key, dst, radius, 255);
  else ccord_draw_texture(r, img->tex_key, dst, 255);
}

// ---- Inline <svg> vector painter ----

static bool svg_attr_color(const web_node *n, const char *attr, ccord_color fallback, ccord_color *out)
{
  const char *v = web_node_get_attr(n, attr);
  if (!v || !v[0]) return false;
  if (ascii_ieq(v, "none") || ascii_ieq(v, "transparent")) return false;
  if (ascii_ieq(v, "currentcolor")) { *out = fallback; return true; }
  return web_css_parse_color(v, out);
}

static bool svg_next_float(const char **p, float *out)
{
  while (**p && (isspace((unsigned char)**p) || **p == ',')) (*p)++;
  if (!**p) return false;
  char *end = nullptr;
  double v = strtod(*p, &end);
  if (end == *p) return false;
  *p = end;
  *out = (float)v;
  return true;
}

static void paint_svg_path(ccord_renderer *r, const char *d, ccord_rect rc,
                           float vx, float vy, float sx, float sy,
                           bool has_fill, ccord_color fill, bool has_stroke, ccord_color stroke, float sw)
{
  if (!d) return;
  ccord_vec2 pts[128];
  size_t npts = 0;
  float cx = 0.0f, cy = 0.0f, mx = 0.0f, my = 0.0f;
  char cmd = 0;
  const char *p = d;
  while (*p) {
    while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
    if (!*p) break;
    if (isalpha((unsigned char)*p)) { cmd = *p++; }
    if (cmd == 'Z' || cmd == 'z') {
      if (npts >= 2u) {
        if (has_fill && npts >= 3u) ccord_draw_polygon(r, pts, npts, fill, true, sw);
        if (has_stroke) {
          ccord_draw_polygon(r, pts, npts, stroke, false, sw);
          ccord_draw_line(r, pts[npts - 1u], pts[0], sw, stroke);
        }
      }
      cx = mx; cy = my;
      npts = 0;
      cmd = 0;
      continue;
    }
    float x = 0, y = 0;
    if (cmd == 'M' || cmd == 'm' || cmd == 'L' || cmd == 'l' || cmd == 'T' || cmd == 't') {
      if (!svg_next_float(&p, &x) || !svg_next_float(&p, &y)) break;
      if (cmd == 'm' || cmd == 'l' || cmd == 't') { x += cx; y += cy; }
      if (cmd == 'M' || cmd == 'm') {
        if (npts >= 2u) {
          if (has_fill && npts >= 3u) ccord_draw_polygon(r, pts, npts, fill, true, sw);
          if (has_stroke) ccord_draw_polygon(r, pts, npts, stroke, false, sw);
        }
        npts = 0;
        mx = x; my = y;
        cmd = (cmd == 'M') ? 'L' : 'l';
      }
      cx = x; cy = y;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (cx - vx) * sx, rc.y + (cy - vy) * sy};
    } else if (cmd == 'H' || cmd == 'h') {
      if (!svg_next_float(&p, &x)) break;
      cx = (cmd == 'h') ? (cx + x) : x;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (cx - vx) * sx, rc.y + (cy - vy) * sy};
    } else if (cmd == 'V' || cmd == 'v') {
      if (!svg_next_float(&p, &y)) break;
      cy = (cmd == 'v') ? (cy + y) : y;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (cx - vx) * sx, rc.y + (cy - vy) * sy};
    } else if (cmd == 'C' || cmd == 'c') {
      float x1, y1, x2, y2;
      if (!svg_next_float(&p, &x1) || !svg_next_float(&p, &y1) ||
          !svg_next_float(&p, &x2) || !svg_next_float(&p, &y2) ||
          !svg_next_float(&p, &x) || !svg_next_float(&p, &y)) break;
      if (cmd == 'c') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
      // Sample cubic Bezier at t=0.5 and t=1.0 for smooth curves
      float midx = 0.125f * cx + 0.375f * x1 + 0.375f * x2 + 0.125f * x;
      float midy = 0.125f * cy + 0.375f * y1 + 0.375f * y2 + 0.125f * y;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (midx - vx) * sx, rc.y + (midy - vy) * sy};
      cx = x; cy = y;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (cx - vx) * sx, rc.y + (cy - vy) * sy};
    } else if (cmd == 'Q' || cmd == 'q' || cmd == 'S' || cmd == 's') {
      float x1, y1;
      if (!svg_next_float(&p, &x1) || !svg_next_float(&p, &y1) ||
          !svg_next_float(&p, &x) || !svg_next_float(&p, &y)) break;
      if (cmd == 'q' || cmd == 's') { x1 += cx; y1 += cy; x += cx; y += cy; }
      float midx = 0.25f * cx + 0.5f * x1 + 0.25f * x;
      float midy = 0.25f * cy + 0.5f * y1 + 0.25f * y;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (midx - vx) * sx, rc.y + (midy - vy) * sy};
      cx = x; cy = y;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (cx - vx) * sx, rc.y + (cy - vy) * sy};
    } else if (cmd == 'A' || cmd == 'a') {
      float rx, ry, rot, large, sweep;
      if (!svg_next_float(&p, &rx) || !svg_next_float(&p, &ry) || !svg_next_float(&p, &rot) ||
          !svg_next_float(&p, &large) || !svg_next_float(&p, &sweep) ||
          !svg_next_float(&p, &x) || !svg_next_float(&p, &y)) break;
      (void)rx; (void)ry; (void)rot; (void)large; (void)sweep;
      if (cmd == 'a') { x += cx; y += cy; }
      cx = x; cy = y;
      if (npts < 128u) pts[npts++] = (ccord_vec2){rc.x + (cx - vx) * sx, rc.y + (cy - vy) * sy};
    } else {
      break;
    }
  }
  if (npts >= 2u) {
    if (has_fill && npts >= 3u) ccord_draw_polygon(r, pts, npts, fill, true, sw);
    if (has_stroke) ccord_draw_polygon(r, pts, npts, stroke, false, sw);
  }
}

static void paint_svg_children(web_node *n, ccord_renderer *r, ccord_rect rc,
                               float vx, float vy, float sx, float sy, ccord_color inherit_col)
{
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    if (c->type != WEB_NODE_ELEMENT || !c->tag) continue;
    if (c->computed.display == WEB_DISP_NONE || c->computed.opacity <= 0.01f) continue;
    if (strcmp(c->tag, "defs") == 0 || strcmp(c->tag, "clippath") == 0 ||
        strcmp(c->tag, "mask") == 0 || strcmp(c->tag, "title") == 0 || strcmp(c->tag, "desc") == 0) continue;

    ccord_color fill = inherit_col, stroke = inherit_col;
    bool has_fill = c->computed.has_svg_fill ? (fill = c->computed.svg_fill, true)
                                             : (!c->computed.svg_fill_none && svg_attr_color(c, "fill", inherit_col, &fill));
    bool has_stroke = c->computed.has_svg_stroke ? (stroke = c->computed.svg_stroke, true)
                                                 : (!c->computed.svg_stroke_none && svg_attr_color(c, "stroke", inherit_col, &stroke));
    if (!has_fill && !has_stroke && !c->computed.svg_fill_none && !web_node_get_attr(c, "fill")) {
      has_fill = true;
      fill = inherit_col;
    }
    const char *swa = web_node_get_attr(c, "stroke-width");
    float sw_raw = c->computed.svg_stroke_w > 0.0f ? c->computed.svg_stroke_w
                                                   : (swa ? (float)strtod(swa, nullptr) : 1.5f);
    float sw = fmaxf(1.0f, sw_raw * fminf(sx, sy));

    if (strcmp(c->tag, "rect") == 0) {
      float rx = web_node_get_attr(c, "x") ? (float)strtod(web_node_get_attr(c, "x"), nullptr) : 0.0f;
      float ry = web_node_get_attr(c, "y") ? (float)strtod(web_node_get_attr(c, "y"), nullptr) : 0.0f;
      float rw = web_node_get_attr(c, "width") ? (float)strtod(web_node_get_attr(c, "width"), nullptr) : 0.0f;
      float rh = web_node_get_attr(c, "height") ? (float)strtod(web_node_get_attr(c, "height"), nullptr) : 0.0f;
      float rad = web_node_get_attr(c, "rx") ? (float)strtod(web_node_get_attr(c, "rx"), nullptr) * fminf(sx, sy) : 0.0f;
      ccord_rect dr = {rc.x + (rx - vx) * sx, rc.y + (ry - vy) * sy, rw * sx, rh * sy};
      if (has_fill) {
        if (rad > 0.5f) ccord_draw_rrect(r, dr, rad, fill);
        else ccord_draw_rect(r, dr, fill);
      }
      if (has_stroke) ccord_draw_rrect_stroke(r, dr, rad, sw, stroke);
    } else if (strcmp(c->tag, "circle") == 0 || strcmp(c->tag, "ellipse") == 0) {
      float cx = web_node_get_attr(c, "cx") ? (float)strtod(web_node_get_attr(c, "cx"), nullptr) : 0.0f;
      float cy = web_node_get_attr(c, "cy") ? (float)strtod(web_node_get_attr(c, "cy"), nullptr) : 0.0f;
      float cr = web_node_get_attr(c, "r") ? (float)strtod(web_node_get_attr(c, "r"), nullptr)
                                           : (web_node_get_attr(c, "rx") ? (float)strtod(web_node_get_attr(c, "rx"), nullptr) : 0.0f);
      float px = rc.x + (cx - vx) * sx, py = rc.y + (cy - vy) * sy, pr = cr * fminf(sx, sy);
      if (has_fill) ccord_draw_circle(r, px, py, pr, fill);
      if (has_stroke) ccord_draw_circle_stroke(r, px, py, pr, sw, stroke);
    } else if (strcmp(c->tag, "line") == 0) {
      float x1 = web_node_get_attr(c, "x1") ? (float)strtod(web_node_get_attr(c, "x1"), nullptr) : 0.0f;
      float y1 = web_node_get_attr(c, "y1") ? (float)strtod(web_node_get_attr(c, "y1"), nullptr) : 0.0f;
      float x2 = web_node_get_attr(c, "x2") ? (float)strtod(web_node_get_attr(c, "x2"), nullptr) : 0.0f;
      float y2 = web_node_get_attr(c, "y2") ? (float)strtod(web_node_get_attr(c, "y2"), nullptr) : 0.0f;
      ccord_draw_line(r, (ccord_vec2){rc.x + (x1 - vx) * sx, rc.y + (y1 - vy) * sy},
                      (ccord_vec2){rc.x + (x2 - vx) * sx, rc.y + (y2 - vy) * sy}, sw, has_stroke ? stroke : fill);
    } else if (strcmp(c->tag, "path") == 0) {
      paint_svg_path(r, web_node_get_attr(c, "d"), rc, vx, vy, sx, sy, has_fill, fill, has_stroke, stroke, sw);
    } else if (strcmp(c->tag, "polyline") == 0 || strcmp(c->tag, "polygon") == 0) {
      const char *ps = web_node_get_attr(c, "points");
      if (ps) {
        ccord_vec2 pts[128];
        size_t np = 0;
        float x = 0, y = 0;
        while (np < 128u && svg_next_float(&ps, &x) && svg_next_float(&ps, &y)) {
          pts[np++] = (ccord_vec2){rc.x + (x - vx) * sx, rc.y + (y - vy) * sy};
        }
        bool is_poly = strcmp(c->tag, "polygon") == 0;
        if (is_poly && has_fill && np >= 3u) ccord_draw_polygon(r, pts, np, fill, true, sw);
        if (has_stroke && np >= 2u) ccord_draw_polygon(r, pts, np, stroke, false, sw);
      }
    } else if (strcmp(c->tag, "g") == 0 || strcmp(c->tag, "svg") == 0) {
      paint_svg_children(c, r, rc, vx, vy, sx, sy, has_fill ? fill : inherit_col);
    }
  }
}

static void paint_borders(ccord_renderer *r, ccord_rect rc, const web_style *st)
{
  if (rc.w <= 0.0f || rc.h <= 0.0f) return;
  float bt = st->border_w[0], br = st->border_w[1], bb = st->border_w[2], bl = st->border_w[3];
  if (bt <= 0.0f && br <= 0.0f && bb <= 0.0f && bl <= 0.0f) return;
  ccord_color bc = st->has_border_color ? st->border_color : st->color;
  if (bc.a == 0) return;
  if (fabsf(bt - br) < 0.1f && fabsf(bt - bb) < 0.1f && fabsf(bt - bl) < 0.1f && bt > 0.0f) {
    ccord_draw_rrect_stroke(r, rc, st->border_radius, bt, bc);
  } else {
    if (bt > 0.0f) ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, bt}, bc);
    if (bb > 0.0f) ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - bb, rc.w, bb}, bc);
    if (bl > 0.0f) ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, bl, rc.h}, bc);
    if (br > 0.0f) ccord_draw_rect(r, (ccord_rect){rc.x + rc.w - br, rc.y, br, rc.h}, bc);
  }
}

static bool node_is_invisible(const web_node *n)
{
  if (!n || n->type != WEB_NODE_ELEMENT) return false;
  const web_style *st = &n->computed;
  return st->display == WEB_DISP_NONE || st->opacity <= 0.01f ||
         fabsf(st->scale_x) <= 0.01f || fabsf(st->scale_y) <= 0.01f;
}

static bool node_is_stacking_context(const web_node *n)
{
  if (!n) return false;
  if (n->type == WEB_NODE_DOCUMENT) return true;
  if (n->type != WEB_NODE_ELEMENT) return false;
  const web_style *st = &n->computed;
  if (n->tag && strcmp(n->tag, "html") == 0) return true;
  bool is_root_el = n->tag && (strcmp(n->tag, "html") == 0 || strcmp(n->tag, "body") == 0);
  if (st->overflow_hidden && !is_root_el) return true;
  if (st->opacity < 0.99f) return true;
  if (fabsf(st->translate_x) >= 0.01f || fabsf(st->translate_y) >= 0.01f ||
      fabsf(st->scale_x - 1.0f) >= 0.01f || fabsf(st->scale_y - 1.0f) >= 0.01f) {
    return true;
  }
  bool is_fg_item = n->parent && n->parent->type == WEB_NODE_ELEMENT &&
                    (n->parent->computed.display == WEB_DISP_FLEX ||
                     n->parent->computed.display == WEB_DISP_GRID);
  if ((st->position != WEB_POS_STATIC || is_fg_item) && st->has_z_index) return true;
  return false;
}

static void sort_nodes_by_z(web_node **arr, size_t n)
{
  for (size_t i = 1; i < n; i++) {
    web_node *key = arr[i];
    int kz = key->computed.z_index;
    size_t j = i;
    while (j > 0 && arr[j - 1u]->computed.z_index > kz) {
      arr[j] = arr[j - 1u];
      j--;
    }
    arr[j] = key;
  }
}

static void collect_sc_layers(web_node *parent,
                              web_node **neg, size_t *nneg, size_t cap_neg,
                              web_node **zpos, size_t *nzpos, size_t cap_zpos,
                              web_node **pos, size_t *npos, size_t cap_pos)
{
  for (web_node *c = parent->first_child; c; c = c->next_sib) {
    if (!c->box_valid || node_is_invisible(c)) continue;
    if (c->type == WEB_NODE_ELEMENT) {
      if (node_is_stacking_context(c)) {
        if (c->computed.has_z_index && c->computed.z_index < 0) {
          if (*nneg < cap_neg) neg[(*nneg)++] = c;
        } else if (c->computed.has_z_index && c->computed.z_index > 0) {
          if (*npos < cap_pos) pos[(*npos)++] = c;
        } else {
          if (*nzpos < cap_zpos) zpos[(*nzpos)++] = c;
        }
        continue;
      }
      if (c->computed.position != WEB_POS_STATIC) {
        if (*nzpos < cap_zpos) zpos[(*nzpos)++] = c;
      }
      if (!c->child_frame && !(c->tag && strcmp(c->tag, "svg") == 0)) {
        collect_sc_layers(c, neg, nneg, cap_neg, zpos, nzpos, cap_zpos, pos, npos, cap_pos);
      }
    } else if (c->type != WEB_NODE_TEXT && c->type != WEB_NODE_COMMENT) {
      collect_sc_layers(c, neg, nneg, cap_neg, zpos, nzpos, cap_zpos, pos, npos, cap_pos);
    }
  }
}

static void paint_node_self(web_frame *f, web_node *n, ccord_renderer *r)
{
  const web_style *st = &n->computed;
  if (n->type == WEB_NODE_TEXT && n->text && !is_ws_only(n->text) && !st->hidden) {
    char norm[1024];
    size_t nl = collapse_html_text(n->text, norm, sizeof norm, st->white_space_pre);
    if (nl > 0) {
      ccord_text_style ts = {
        .font = st->font_bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR,
        .size = st->font_size,
        .color = st->color,
      };
      float wrap_w = 0.0f;
      if (!st->white_space_nowrap && n->parent) {
        float pw = fmaxf(0.0f, n->parent->box.w - n->parent->computed.padding[1] - n->parent->computed.padding[3] -
                                   n->parent->computed.border_w[1] - n->parent->computed.border_w[3]);
        wrap_w = pw > 4.0f ? fmaxf(pw, n->box.w + 1.0f) : 0.0f;
      }
      float tx = n->box.x;
      if (st->text_align == 1 && n->parent && n->box.w < n->parent->box.w) {
        tx = n->parent->box.x + (n->parent->box.w - n->box.w) * 0.5f;
      } else if (st->text_align == 2 && n->parent && n->box.w < n->parent->box.w) {
        tx = n->parent->box.x + n->parent->box.w - n->box.w;
      }
      ccord_text_draw(r, norm, nl, tx, n->box.y, &ts, wrap_w);
    }
  } else if (n->type == WEB_NODE_ELEMENT && !st->hidden) {
    ccord_rect rc = n->box;
    if (st->has_bg && rc.w > 0.0f && rc.h > 0.0f) {
      if (st->border_radius > 0.5f) ccord_draw_rrect(r, rc, st->border_radius, st->bg);
      else ccord_draw_rect(r, rc, st->bg);
    }
    if (n->img && st->bg_image_url[0]) {
      paint_image(f, n->img, r, rc, st->border_radius, st->bg_cover, st->color);
    }
    if (n->tag && strcmp(n->tag, "img") == 0 && n->img) {
      paint_image(f, n->img, r, rc, st->border_radius, false, st->color);
    }
    if (n->canvas) {
      web_canvas_paint(n->canvas, r, rc);
    }
    if (n->tag && strcmp(n->tag, "svg") == 0 && rc.w > 0.5f && rc.h > 0.5f) {
      paint_svg_node(n, r, rc, st->color);
    }
    if (n->tag && strcmp(n->tag, "input") == 0) {
      const char *tp = web_node_get_attr(n, "type");
      if (tp && ascii_ieq(tp, "checkbox") && n->checked) {
        ccord_draw_rrect(r, rc, st->border_radius, (ccord_color){35, 165, 90, 255});
        ccord_draw_line(r, (ccord_vec2){rc.x + 5, rc.y + rc.h * 0.55f},
                        (ccord_vec2){rc.x + rc.w * 0.42f, rc.y + rc.h - 5}, 2.5f, (ccord_color){255, 255, 255, 255});
        ccord_draw_line(r, (ccord_vec2){rc.x + rc.w * 0.42f, rc.y + rc.h - 5},
                        (ccord_vec2){rc.x + rc.w - 4, rc.y + 5}, 2.5f, (ccord_color){255, 255, 255, 255});
      } else if (n->text && n->text[0]) {
        ccord_text_style ts = {.font = CCORD_FONT_REGULAR, .size = st->font_size, .color = st->color};
        ccord_text_draw(r, n->text, strlen(n->text), rc.x + 6.0f, rc.y + 6.0f, &ts, rc.w - 12.0f);
      }
    }
    paint_borders(r, rc, st);
  }
}

static void paint_flow_children(web_frame *f, web_node *parent, ccord_renderer *r, int depth)
{
  if (!parent || depth > 32) return;
  for (web_node *c = parent->first_child; c; c = c->next_sib) {
    if (!c->box_valid || node_is_invisible(c)) continue;
    if (c->type == WEB_NODE_ELEMENT) {
      if (node_is_stacking_context(c) || c->computed.position != WEB_POS_STATIC) continue;
      paint_node_self(f, c, r);
      if (c->child_frame) {
        if (!c->computed.hidden) web_frame_paint(c->child_frame, r, c->box);
      } else if (!(c->tag && strcmp(c->tag, "svg") == 0)) {
        paint_flow_children(f, c, r, depth + 1);
      }
    } else if (c->type == WEB_NODE_TEXT) {
      paint_node_self(f, c, r);
    } else if (c->type != WEB_NODE_COMMENT) {
      paint_flow_children(f, c, r, depth + 1);
    }
  }
}

static void paint_node_sc(web_frame *f, web_node *n, ccord_renderer *r, int depth)
{
  if (!n || !n->box_valid || depth > 32 || node_is_invisible(n)) return;
  const web_style *st = &n->computed;

  bool pushed_alpha = false;
  if (n->type == WEB_NODE_ELEMENT && st->opacity < 0.99f) {
    ccord_push_alpha(r, st->opacity);
    pushed_alpha = true;
  }

  paint_node_self(f, n, r);

  bool is_root_el = n->type == WEB_NODE_ELEMENT && n->tag &&
                    (strcmp(n->tag, "html") == 0 || strcmp(n->tag, "body") == 0);
  bool clipped = false;
  if (n->type == WEB_NODE_ELEMENT && st->overflow_hidden && !is_root_el &&
      n->box.w > 0.0f && n->box.h > 0.0f) {
    ccord_push_clip(r, n->box);
    clipped = true;
  }

  if (n->child_frame) {
    if (!st->hidden) web_frame_paint(n->child_frame, r, n->box);
  } else if (!(n->type == WEB_NODE_ELEMENT && n->tag && strcmp(n->tag, "svg") == 0)) {
    web_node *neg[128], *zpos[256], *pos[256];
    size_t nneg = 0, nzpos = 0, npos = 0;
    collect_sc_layers(n, neg, &nneg, 128u, zpos, &nzpos, 256u, pos, &npos, 256u);

    // CSS 2.1 Appendix E stacking order: negative z -> normal flow -> positioned/auto-z -> positive z
    sort_nodes_by_z(neg, nneg);
    for (size_t i = 0; i < nneg; i++) paint_node_sc(f, neg[i], r, depth + 1);

    paint_flow_children(f, n, r, depth + 1);

    for (size_t i = 0; i < nzpos; i++) {
      web_node *c = zpos[i];
      if (node_is_stacking_context(c)) {
        paint_node_sc(f, c, r, depth + 1);
      } else {
        paint_node_self(f, c, r);
        if (c->child_frame) {
          if (!c->computed.hidden) web_frame_paint(c->child_frame, r, c->box);
        } else if (!(c->tag && strcmp(c->tag, "svg") == 0)) {
          paint_flow_children(f, c, r, depth + 1);
        }
      }
    }

    sort_nodes_by_z(pos, npos);
    for (size_t i = 0; i < npos; i++) paint_node_sc(f, pos[i], r, depth + 1);
  }

  if (clipped) ccord_pop_clip(r);
  if (pushed_alpha) ccord_pop_alpha(r);
}

static bool frame_has_populated_canvas(const web_frame *f)
{
  if (!f) return false;
  for (size_t i = 0; i < f->all_nodes_len; i++) {
    const web_node *n = f->all_nodes[i];
    if (n && n->type == WEB_NODE_ELEMENT && n->tag && strcmp(n->tag, "canvas") == 0 &&
        n->box_valid && n->box.h > 40.0f) {
      return true;
    }
  }
  return false;
}

void web_frame_paint(web_frame *f, ccord_renderer *r, ccord_rect bounds)
{
  if (!f || !r || !f->doc || f->is_worker) return;
  web_frame_ensure_layout(f, r);
  if (!f->diag_dumped && f->view && !f->is_root && f->all_nodes_len > 25u) {
    f->diag_dumped = true;
    if (f->view->root && !f->view->root->diag_dumped) {
      f->view->root->diag_dumped = true;
      web_frame_dump_tree(f->view->root);
    } else {
      web_frame_dump_tree(f);
    }
  } else if (f->diag_dumped && !f->diag_dumped_populated && f->view && !f->is_root &&
             frame_has_populated_canvas(f)) {
    f->diag_dumped_populated = true;
    if (f->view->root) web_frame_dump_tree(f->view->root);
    web_frame_dump_tree(f);
  }
  ccord_push_clip(r, bounds);
  if (f->body && f->body->computed.has_bg && !f->body->computed.hidden) {
    ccord_draw_rect(r, bounds, f->body->computed.bg);
  }
  ccord_push_offset(r, bounds.x, bounds.y - f->scroll_y);
  paint_node_sc(f, f->doc, r, 0);
  ccord_pop_offset(r);
  ccord_pop_clip(r);
}

// ---- Hit-testing & Input Dispatch ----

static bool pt_in_box(ccord_rect box, float x, float y)
{
  return x >= box.x && x <= box.x + box.w && y >= box.y && y <= box.y + box.h;
}

static bool hit_test_node_sc(web_frame *f, web_node *n, float x, float y,
                             web_frame **out_frame, web_node **out_node, float *out_lx, float *out_ly, int depth);

static bool hit_test_iframe(web_frame *f, web_node *n, float x, float y,
                            web_frame **out_frame, web_node **out_node, float *out_lx, float *out_ly)
{
  if (!n->child_frame || n->computed.hidden || n->computed.pointer_none || !pt_in_box(n->box, x, y)) {
    return false;
  }
  float cx = x - n->box.x;
  float cy = y - n->box.y + n->child_frame->scroll_y;
  web_frame_ensure_layout(n->child_frame, f->view->measure_r);
  if (hit_test_node_sc(n->child_frame, n->child_frame->doc, cx, cy, out_frame, out_node, out_lx, out_ly, 0)) {
    return true;
  }
  *out_frame = n->child_frame;
  *out_node = n->child_frame->body ? n->child_frame->body : n->child_frame->doc;
  *out_lx = cx;
  *out_ly = cy;
  return true;
}

static bool hit_test_self(web_frame *f, web_node *n, float x, float y,
                          web_frame **out_frame, web_node **out_node, float *out_lx, float *out_ly)
{
  if (n->type == WEB_NODE_ELEMENT && !n->computed.hidden && !n->computed.pointer_none && pt_in_box(n->box, x, y)) {
    *out_frame = f;
    *out_node = n;
    *out_lx = x;
    *out_ly = y;
    return true;
  }
  return false;
}

static bool hit_test_flow_children(web_frame *f, web_node *parent, float x, float y,
                                   web_frame **out_frame, web_node **out_node, float *out_lx, float *out_ly, int depth)
{
  if (!parent || depth > 32) return false;
  web_node *kids[256];
  size_t nk = 0;
  for (web_node *c = parent->first_child; c && nk < 256u; c = c->next_sib) kids[nk++] = c;
  for (size_t i = nk; i > 0; i--) {
    web_node *c = kids[i - 1u];
    if (!c->box_valid || node_is_invisible(c)) continue;
    if (c->type == WEB_NODE_ELEMENT) {
      if (node_is_stacking_context(c) || c->computed.position != WEB_POS_STATIC) continue;
      if (c->child_frame) {
        if (hit_test_iframe(f, c, x, y, out_frame, out_node, out_lx, out_ly)) return true;
        continue;
      }
      if (!(c->tag && strcmp(c->tag, "svg") == 0)) {
        if (hit_test_flow_children(f, c, x, y, out_frame, out_node, out_lx, out_ly, depth + 1)) return true;
      }
      if (hit_test_self(f, c, x, y, out_frame, out_node, out_lx, out_ly)) return true;
    } else if (c->type != WEB_NODE_TEXT && c->type != WEB_NODE_COMMENT) {
      if (hit_test_flow_children(f, c, x, y, out_frame, out_node, out_lx, out_ly, depth + 1)) return true;
    }
  }
  return false;
}

static bool hit_test_node_sc(web_frame *f, web_node *n, float x, float y,
                             web_frame **out_frame, web_node **out_node, float *out_lx, float *out_ly, int depth)
{
  if (!n || !n->box_valid || depth > 32 || node_is_invisible(n)) return false;

  bool is_root_el = n->type == WEB_NODE_ELEMENT && n->tag &&
                    (strcmp(n->tag, "html") == 0 || strcmp(n->tag, "body") == 0);
  if (n->type == WEB_NODE_ELEMENT && n->computed.overflow_hidden && !is_root_el && !pt_in_box(n->box, x, y)) {
    return false;
  }

  if (n->child_frame) {
    return hit_test_iframe(f, n, x, y, out_frame, out_node, out_lx, out_ly);
  }

  if (!(n->type == WEB_NODE_ELEMENT && n->tag && strcmp(n->tag, "svg") == 0)) {
    web_node *neg[128], *zpos[256], *pos[256];
    size_t nneg = 0, nzpos = 0, npos = 0;
    collect_sc_layers(n, neg, &nneg, 128u, zpos, &nzpos, 256u, pos, &npos, 256u);

    // Exact reverse of CSS 2.1 paint order: positive z -> positioned/auto-z -> normal flow -> negative z -> self
    sort_nodes_by_z(pos, npos);
    for (size_t i = npos; i > 0; i--) {
      if (hit_test_node_sc(f, pos[i - 1u], x, y, out_frame, out_node, out_lx, out_ly, depth + 1)) return true;
    }

    for (size_t i = nzpos; i > 0; i--) {
      web_node *c = zpos[i - 1u];
      if (node_is_stacking_context(c)) {
        if (hit_test_node_sc(f, c, x, y, out_frame, out_node, out_lx, out_ly, depth + 1)) return true;
      } else {
        if (c->child_frame) {
          if (hit_test_iframe(f, c, x, y, out_frame, out_node, out_lx, out_ly)) return true;
        } else {
          if (!(c->tag && strcmp(c->tag, "svg") == 0)) {
            if (hit_test_flow_children(f, c, x, y, out_frame, out_node, out_lx, out_ly, depth + 1)) return true;
          }
          if (hit_test_self(f, c, x, y, out_frame, out_node, out_lx, out_ly)) return true;
        }
      }
    }

    if (hit_test_flow_children(f, n, x, y, out_frame, out_node, out_lx, out_ly, depth + 1)) return true;

    sort_nodes_by_z(neg, nneg);
    for (size_t i = nneg; i > 0; i--) {
      if (hit_test_node_sc(f, neg[i - 1u], x, y, out_frame, out_node, out_lx, out_ly, depth + 1)) return true;
    }
  }

  return hit_test_self(f, n, x, y, out_frame, out_node, out_lx, out_ly);
}

static bool hit_test_node(web_frame *f, web_node *n, float x, float y,
                          web_frame **out_frame, web_node **out_node, float *out_lx, float *out_ly)
{
  return hit_test_node_sc(f, n, x, y, out_frame, out_node, out_lx, out_ly, 0);
}

static web_frame *find_frame_by_id(ccord_web_view *v, uint32_t fid)
{
  if (!v || fid == 0u) return nullptr;
  for (size_t i = 0; i < v->nframes; i++) {
    if (v->frames[i] && v->frames[i]->id == fid) return v->frames[i];
  }
  return nullptr;
}

static web_node *find_node_by_id(web_frame *f, uint32_t nid)
{
  if (!f || nid == 0u) return nullptr;
  for (size_t i = 0; i < f->all_nodes_len; i++) {
    if (f->all_nodes[i] && f->all_nodes[i]->id == nid) return f->all_nodes[i];
  }
  return nullptr;
}

static bool frame_to_local_coords(ccord_web_view *v, web_frame *target, float root_x, float root_y,
                                  float *out_lx, float *out_ly)
{
  if (!v || !target) return false;
  web_frame *chain[WEB_MAX_FRAMES];
  size_t nc = 0;
  for (web_frame *cur = target; cur && nc < WEB_MAX_FRAMES; cur = cur->parent) {
    chain[nc++] = cur;
    if (cur == v->root) break;
  }
  if (nc == 0u || chain[nc - 1u] != v->root) return false;
  float x = root_x;
  float y = root_y + v->root->scroll_y;
  for (size_t i = nc - 1u; i > 0u; i--) {
    web_frame *child = chain[i - 1u];
    web_node *host = child->host_iframe;
    if (!host || !host->box_valid) return false;
    x -= host->box.x;
    y = (y - host->box.y) + child->scroll_y;
  }
  *out_lx = x;
  *out_ly = y;
  return true;
}

static bool js_event_default_prevented(JSContext *ctx, JSValueConst ev)
{
  JSValue dp = JS_GetPropertyStr(ctx, ev, "defaultPrevented");
  bool res = JS_ToBool(ctx, dp) > 0;
  JS_FreeValue(ctx, dp);
  return res;
}

static bool nodes_related(const web_node *a, const web_node *b)
{
  if (!a || !b) return false;
  if (a == b) return true;
  for (const web_node *p = a->parent; p; p = p->parent) {
    if (p == b) return true;
  }
  for (const web_node *p = b->parent; p; p = p->parent) {
    if (p == a) return true;
  }
  return false;
}

bool web_view_dispatch_input(ccord_web_view *v, ccord_rect bounds, const ccord_input_event *ev)
{
  if (!v || !v->root || !ev) return false;
  if (v->rt) JS_UpdateStackTop(v->rt);
  web_frame_ensure_layout(v->root, v->measure_r);

  if (ev->type == CCORD_IN_MOUSE_MOVE || ev->type == CCORD_IN_MOUSE_DOWN || ev->type == CCORD_IN_MOUSE_UP) {
    bool inside = ev->x >= bounds.x && ev->x <= bounds.x + bounds.w &&
                  ev->y >= bounds.y && ev->y <= bounds.y + bounds.h;
    if (!inside && !v->mouse_down) return false;

    float rx = ev->x - bounds.x;
    float ry = ev->y - bounds.y + v->root->scroll_y;
    web_frame *hf = v->root;
    web_node *hn = v->root->body ? v->root->body : v->root->doc;
    float lx = rx, ly = ry;
    (void)hit_test_node(v->root, v->root->doc, rx, ry, &hf, &hn, &lx, &ly);
    if (!hf || !hn) return inside;

    float ox = lx - hn->box.x, oy = ly - hn->box.y;
    int btn = ev->button == 1 ? 0 : (ev->button == 3 ? 2 : 0);

    if (ev->type == CCORD_IN_MOUSE_MOVE) {
      v->last_mx = ev->x; v->last_my = ev->y;
      if (hf->hovered_node != hn) {
        hf->hovered_node = hn;
        hf->style_dirty = true;
        web_mark_dirty(v);
      }
      int btns = v->mouse_down ? 1 : 0;
      JSValue pe = web_js_make_pointer_event(hf->ctx, "pointermove", lx, ly, ox, oy, btn, btns, true);
      web_dom_dispatch_event(hf, hn, pe);
      JS_FreeValue(hf->ctx, pe);
      JSValue me = web_js_make_pointer_event(hf->ctx, "mousemove", lx, ly, ox, oy, btn, btns, false);
      web_dom_dispatch_event(hf, hn, me);
      JS_FreeValue(hf->ctx, me);
      return inside;
    }

    if (ev->type == CCORD_IN_MOUSE_DOWN) {
      const char *hid = web_node_get_attr(hn, "id");
      const char *hcls = web_node_get_attr(hn, "class");
      CCORD_LOG_D("web: mousedown f%u <%s%s%s%s%s> at (%.0f,%.0f) box=(%.0f,%.0f %.0fx%.0f)",
                  hf->id, hn->tag ? hn->tag : "?",
                  hid ? "#" : "", hid ? hid : "",
                  hcls ? "." : "", hcls ? hcls : "",
                  (double)lx, (double)ly,
                  (double)hn->box.x, (double)hn->box.y, (double)hn->box.w, (double)hn->box.h);
      v->mouse_down = true;
      v->captured_frame = hf;
      v->captured_node = hn;
      hf->focused_node = hn;
      JSValue pe = web_js_make_pointer_event(hf->ctx, "pointerdown", lx, ly, ox, oy, btn, 1, true);
      web_dom_dispatch_event(hf, hn, pe);
      JS_FreeValue(hf->ctx, pe);
      JSValue me = web_js_make_pointer_event(hf->ctx, "mousedown", lx, ly, ox, oy, btn, 1, false);
      web_dom_dispatch_event(hf, hn, me);
      JS_FreeValue(hf->ctx, me);
      web_mark_dirty(v);
      return true;
    }

    if (ev->type == CCORD_IN_MOUSE_UP) {
      bool was_down = v->mouse_down;
      v->mouse_down = false;
      JSValue pe = web_js_make_pointer_event(hf->ctx, "pointerup", lx, ly, ox, oy, btn, 0, true);
      web_dom_dispatch_event(hf, hn, pe);
      JS_FreeValue(hf->ctx, pe);
      JSValue me = web_js_make_pointer_event(hf->ctx, "mouseup", lx, ly, ox, oy, btn, 0, false);
      web_dom_dispatch_event(hf, hn, me);
      JS_FreeValue(hf->ctx, me);
      if (was_down && inside) {
        if (hn->tag && strcmp(hn->tag, "input") == 0) {
          const char *tp = web_node_get_attr(hn, "type");
          if (tp && ascii_ieq(tp, "checkbox")) {
            hn->checked = !hn->checked;
            web_dom_dispatch_simple(hf, hn, "change", true);
          }
        }
        JSValue ce = web_js_make_pointer_event(hf->ctx, "click", lx, ly, ox, oy, btn, 0, false);
        web_dom_dispatch_event(hf, hn, ce);
        JS_FreeValue(hf->ctx, ce);
      }
      web_mark_dirty(v);
      return inside || was_down;
    }
  }

  if (ev->type == CCORD_IN_TOUCH_DOWN || ev->type == CCORD_IN_TOUCH_MOVE ||
      ev->type == CCORD_IN_TOUCH_UP || ev->type == CCORD_IN_TOUCH_CANCEL) {
    bool inside = ev->x >= bounds.x && ev->x <= bounds.x + bounds.w &&
                  ev->y >= bounds.y && ev->y <= bounds.y + bounds.h;
    float rx = ev->x - bounds.x;
    float ry = ev->y - bounds.y;
    int pid = (int)(((uint64_t)ev->finger_id & 0x7FFFu) + 2u);

    if (ev->type == CCORD_IN_TOUCH_DOWN) {
      if (v->touch_down) return inside; // track primary finger; consume secondary touches inside bounds
      if (!inside) return false;

      web_frame *hf = v->root;
      web_node *hn = v->root->body ? v->root->body : v->root->doc;
      float lx = rx, ly = ry + v->root->scroll_y;
      (void)hit_test_node(v->root, v->root->doc, rx, ry + v->root->scroll_y, &hf, &hn, &lx, &ly);
      if (!hf || !hn) return inside;

      const char *hid = web_node_get_attr(hn, "id");
      const char *hcls = web_node_get_attr(hn, "class");
      CCORD_LOG_D("web: touchdown f%u <%s%s%s%s%s> at (%.0f,%.0f) box=(%.0f,%.0f %.0fx%.0f)",
                  hf->id, hn->tag ? hn->tag : "?",
                  hid ? "#" : "", hid ? hid : "",
                  hcls ? "." : "", hcls ? hcls : "",
                  (double)lx, (double)ly,
                  (double)hn->box.x, (double)hn->box.y, (double)hn->box.w, (double)hn->box.h);

      v->touch_down = true;
      v->touch_moved = false;
      v->touch_prevented = false;
      v->touch_finger = ev->finger_id;
      v->touch_frame_id = hf->id;
      v->touch_node_id = hn->id;
      v->touch_start_x = ev->x;
      v->touch_start_y = ev->y;
      v->mouse_down = true;
      v->last_mx = ev->x;
      v->last_my = ev->y;
      v->captured_frame = hf;
      v->captured_node = hn;
      hf->focused_node = hn;
      if (hf->hovered_node != hn) {
        hf->hovered_node = hn;
        hf->style_dirty = true;
      }

      uint32_t fid = hf->id;
      float ox = lx - hn->box.x, oy = ly - hn->box.y;
      JSValue pe = web_js_make_pointer_event_ex(hf->ctx, "pointerdown", lx, ly, ox, oy, 0, 1, true, "touch", pid);
      web_dom_dispatch_event(hf, hn, pe);
      if (web_frame_alive(v, fid)) JS_FreeValue(hf->ctx, pe);

      bool start_prev = false;
      if (web_frame_alive(v, fid)) {
        JSValue target_js = web_node_wrap(hf, hn);
        JSValue te = web_js_make_touch_event(hf->ctx, "touchstart", target_js, lx, ly, pid, true);
        web_dom_dispatch_event(hf, hn, te);
        if (web_frame_alive(v, fid)) {
          start_prev = js_event_default_prevented(hf->ctx, te);
          JS_FreeValue(hf->ctx, te);
          JS_FreeValue(hf->ctx, target_js);
        }
      }
      v->touch_prevented = start_prev;

      if (!start_prev && web_frame_alive(v, fid)) {
        JSValue me = web_js_make_pointer_event_ex(hf->ctx, "mousedown", lx, ly, ox, oy, 0, 1, false, "mouse", 1);
        web_dom_dispatch_event(hf, hn, me);
        if (web_frame_alive(v, fid)) JS_FreeValue(hf->ctx, me);
      }
      web_mark_dirty(v);
      return true;
    }

    if (!v->touch_down || ev->finger_id != v->touch_finger) return inside;

    // Resolve captured frame/node safely (implicit touch capture keeps drags locked to the original target).
    web_frame *hf = find_frame_by_id(v, v->touch_frame_id);
    web_node *hn = find_node_by_id(hf, v->touch_node_id);
    float lx = rx, ly = ry + v->root->scroll_y;
    if (!hf || !hn || !frame_to_local_coords(v, hf, rx, ry, &lx, &ly)) {
      hf = v->root;
      hn = v->root->body ? v->root->body : v->root->doc;
      (void)hit_test_node(v->root, v->root->doc, rx, ry + v->root->scroll_y, &hf, &hn, &lx, &ly);
    }
    if (!hf || !hn) {
      if (ev->type == CCORD_IN_TOUCH_UP || ev->type == CCORD_IN_TOUCH_CANCEL) {
        v->touch_down = false;
        v->mouse_down = false;
      }
      return true;
    }

    uint32_t fid = hf->id;
    float ox = lx - hn->box.x, oy = ly - hn->box.y;
    v->last_mx = ev->x;
    v->last_my = ev->y;

    if (ev->type == CCORD_IN_TOUCH_MOVE) {
      float dx = ev->x - v->touch_start_x;
      float dy = ev->y - v->touch_start_y;
      if (dx * dx + dy * dy > 100.0f) v->touch_moved = true;

      JSValue pe = web_js_make_pointer_event_ex(hf->ctx, "pointermove", lx, ly, ox, oy, 0, 1, true, "touch", pid);
      web_dom_dispatch_event(hf, hn, pe);
      if (web_frame_alive(v, fid)) JS_FreeValue(hf->ctx, pe);

      bool move_prev = false;
      if (web_frame_alive(v, fid)) {
        JSValue target_js = web_node_wrap(hf, hn);
        JSValue te = web_js_make_touch_event(hf->ctx, "touchmove", target_js, lx, ly, pid, true);
        web_dom_dispatch_event(hf, hn, te);
        if (web_frame_alive(v, fid)) {
          move_prev = js_event_default_prevented(hf->ctx, te);
          JS_FreeValue(hf->ctx, te);
          JS_FreeValue(hf->ctx, target_js);
        }
      }
      if (!v->touch_prevented && !move_prev && web_frame_alive(v, fid)) {
        JSValue me = web_js_make_pointer_event_ex(hf->ctx, "mousemove", lx, ly, ox, oy, 0, 1, false, "mouse", 1);
        web_dom_dispatch_event(hf, hn, me);
        if (web_frame_alive(v, fid)) JS_FreeValue(hf->ctx, me);
      }
      web_mark_dirty(v);
      return true;
    }

    // CCORD_IN_TOUCH_UP or CCORD_IN_TOUCH_CANCEL
    bool was_moved = v->touch_moved;
    bool start_prev = v->touch_prevented;
    v->touch_down = false;
    v->touch_moved = false;
    v->touch_prevented = false;
    v->mouse_down = false;

    if (ev->type == CCORD_IN_TOUCH_CANCEL) {
      JSValue pe = web_js_make_pointer_event_ex(hf->ctx, "pointercancel", lx, ly, ox, oy, 0, 0, true, "touch", pid);
      web_dom_dispatch_event(hf, hn, pe);
      if (web_frame_alive(v, fid)) JS_FreeValue(hf->ctx, pe);
      if (web_frame_alive(v, fid)) {
        JSValue target_js = web_node_wrap(hf, hn);
        JSValue te = web_js_make_touch_event(hf->ctx, "touchcancel", target_js, lx, ly, pid, false);
        web_dom_dispatch_event(hf, hn, te);
        if (web_frame_alive(v, fid)) {
          JS_FreeValue(hf->ctx, te);
          JS_FreeValue(hf->ctx, target_js);
        }
      }
      web_mark_dirty(v);
      return true;
    }

    JSValue pe = web_js_make_pointer_event_ex(hf->ctx, "pointerup", lx, ly, ox, oy, 0, 0, true, "touch", pid);
    web_dom_dispatch_event(hf, hn, pe);
    if (web_frame_alive(v, fid)) JS_FreeValue(hf->ctx, pe);

    bool end_prev = false;
    if (web_frame_alive(v, fid)) {
      JSValue target_js = web_node_wrap(hf, hn);
      JSValue te = web_js_make_touch_event(hf->ctx, "touchend", target_js, lx, ly, pid, false);
      web_dom_dispatch_event(hf, hn, te);
      if (web_frame_alive(v, fid)) {
        end_prev = js_event_default_prevented(hf->ctx, te);
        JS_FreeValue(hf->ctx, te);
        JS_FreeValue(hf->ctx, target_js);
      }
    }

    if (!start_prev && !end_prev && web_frame_alive(v, fid)) {
      JSValue me = web_js_make_pointer_event_ex(hf->ctx, "mouseup", lx, ly, ox, oy, 0, 0, false, "mouse", 1);
      web_dom_dispatch_event(hf, hn, me);
      if (web_frame_alive(v, fid)) JS_FreeValue(hf->ctx, me);

      if (inside && web_frame_alive(v, fid)) {
        web_frame *cf = hf;
        web_node *cn = hn;
        float clx = lx, cly = ly;
        (void)hit_test_node(v->root, v->root->doc, rx, ry + v->root->scroll_y, &cf, &cn, &clx, &cly);
        bool same_target = (cf == hf && nodes_related(cn, hn));
        if (same_target || !was_moved) {
          web_frame *tf = same_target ? cf : hf;
          web_node *tn = same_target ? cn : hn;
          float tx = same_target ? clx : lx;
          float ty = same_target ? cly : ly;
          uint32_t tfid = tf->id;
          if (tn->tag && strcmp(tn->tag, "input") == 0) {
            const char *tp = web_node_get_attr(tn, "type");
            if (tp && ascii_ieq(tp, "checkbox")) {
              tn->checked = !tn->checked;
              web_dom_dispatch_simple(tf, tn, "change", true);
            }
          }
          if (web_frame_alive(v, tfid)) {
            JSValue ce = web_js_make_pointer_event_ex(tf->ctx, "click", tx, ty,
                                                      tx - tn->box.x, ty - tn->box.y, 0, 0, false, "mouse", 1);
            web_dom_dispatch_event(tf, tn, ce);
            if (web_frame_alive(v, tfid)) JS_FreeValue(tf->ctx, ce);
          }
        }
      }
    }
    web_mark_dirty(v);
    return true;
  }

  if (ev->type == CCORD_IN_WHEEL) {
    bool inside = ev->x >= bounds.x && ev->x <= bounds.x + bounds.w &&
                  ev->y >= bounds.y && ev->y <= bounds.y + bounds.h;
    if (!inside) return false;
    web_frame *hf = v->root;
    web_node *hn = nullptr;
    float lx = 0, ly = 0;
    (void)hit_test_node(v->root, v->root->doc, ev->x - bounds.x, ev->y - bounds.y + v->root->scroll_y, &hf, &hn, &lx, &ly);
    if (!hf) hf = v->root;
    hf->scroll_y -= ev->wheel * 36.0f;
    if (hf->scroll_y < 0.0f) hf->scroll_y = 0.0f;
    web_mark_dirty(v);
    return true;
  }

  if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
    web_frame *tf = v->captured_frame ? v->captured_frame : v->root;
    web_node *fn = tf ? tf->focused_node : nullptr;
    if (fn && fn->tag && (strcmp(fn->tag, "input") == 0 || strcmp(fn->tag, "textarea") == 0)) {
      size_t cur = fn->text ? strlen(fn->text) : 0;
      size_t add = strlen(ev->text);
      char *nt = ccord_realloc(fn->text, cur + add + 1u);
      if (nt) {
        memcpy(nt + cur, ev->text, add + 1u);
        fn->text = nt;
        web_dom_dispatch_simple(tf, fn, "input", true);
        web_mark_dirty(v);
      }
      return true;
    }
  }

  if (ev->type == CCORD_IN_KEY_DOWN) {
    web_frame *tf = v->captured_frame ? v->captured_frame : v->root;
    web_node *fn = (tf && tf->focused_node) ? tf->focused_node : (tf ? tf->body : nullptr);
    if (tf && fn) {
      const char *kname = ev->key == CCORD_KEY_RETURN ? "Enter"
                        : ev->key == CCORD_KEY_BACKSPACE ? "Backspace"
                        : ev->key == CCORD_KEY_ESCAPE ? "Escape"
                        : ev->key == CCORD_KEY_TAB ? "Tab" : "Key";
      if (ev->key == CCORD_KEY_BACKSPACE && fn->text && fn->text[0] &&
          fn->tag && (strcmp(fn->tag, "input") == 0 || strcmp(fn->tag, "textarea") == 0)) {
        fn->text[strlen(fn->text) - 1u] = '\0';
        web_dom_dispatch_simple(tf, fn, "input", true);
        web_mark_dirty(v);
      }
      JSValue ke = web_js_make_key_event(tf->ctx, "keydown", kname, kname, (uint32_t)ev->key);
      web_dom_dispatch_event(tf, fn, ke);
      JS_FreeValue(tf->ctx, ke);
      return ev->key != CCORD_KEY_ESCAPE;
    }
  }
  return false;
}
