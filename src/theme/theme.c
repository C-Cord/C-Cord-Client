// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/theme/theme.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/ini.h>

#define THEME_FILE_MAX (64u * 1024u)
#define THEME_FONT_MAX 127u
#define THEME_ID_MAX 47u
#define THEME_NAME_MAX 63u
#define THEME_METRIC_MAX 4096.0
#define THEME_MERGE_MAX 3600000.0

#define CT_NAME(n) #n,
#define CT_COFF(n) offsetof(ccord_theme, c.n),
#define CT_MNAME(n) #n,
#define CT_MOFF(n) offsetof(ccord_theme, m.n),
#define CT_ONE(n) +1

static const char *const color_names[] = {CCORD_THEME_COLORS(CT_NAME)};
static const size_t color_offsets[] = {CCORD_THEME_COLORS(CT_COFF)};
static const char *const metric_names[] = {CCORD_THEME_METRICS(CT_MNAME)};
static const size_t metric_offsets[] = {CCORD_THEME_METRICS(CT_MOFF)};
enum {
  THEME_NCOLORS = 0 CCORD_THEME_COLORS(CT_ONE),
  THEME_NMETRICS = 0 CCORD_THEME_METRICS(CT_ONE),
};
static const char *const font_names[] = {"font_regular", "font_bold"};
static const size_t font_offsets[] = {offsetof(ccord_theme, font_regular), offsetof(ccord_theme, font_bold)};

typedef struct theme_ctx {
  ccord_theme *t;
  bool meta_seen[2];
  bool color_seen[THEME_NCOLORS];
  bool metric_seen[THEME_NMETRICS];
  bool font_seen[2];
} theme_ctx;

static int hexnib(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool ccord_color_parse(const char *s, ccord_color *out)
{
  if (!s || s[0] != '#') return false;
  size_t n = strlen(s);
  if (n != 7 && n != 9) return false;
  uint8_t v[4] = {0, 0, 0, 255};
  for (size_t i = 0; i < (n - 1) / 2; i++) {
    int hi = hexnib(s[1 + 2 * i]), lo = hexnib(s[2 + 2 * i]);
    if (hi < 0 || lo < 0) return false;
    v[i] = (uint8_t)(hi * 16 + lo);
  }
  *out = (ccord_color){.r = v[0], .g = v[1], .b = v[2], .a = v[3]};
  return true;
}

static bool font_path_ok(const char *p)
{
  size_t n = strlen(p);
  if (n == 0 || n > THEME_FONT_MAX || p[0] == '/' || strstr(p, "..") || strchr(p, '\\')) return false;
  bool ttf = n > 4 && strcmp(p + n - 4, ".ttf") == 0;
  bool otf = n > 4 && strcmp(p + n - 4, ".otf") == 0;
  bool ttc = n > 4 && strcmp(p + n - 4, ".ttc") == 0;
  return ttf || otf || ttc;
}

static bool metric_value(const char *s, double max, float *out)
{
  size_t n = strlen(s);
  if (n == 0 || n > 16) return false;
  int dots = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '.') {
      dots++;
    } else if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  if (dots > 1 || s[0] == '.' || s[n - 1] == '.') return false;
  double v = 0;
  if (sscanf(s, "%lf", &v) != 1 || v < 0 || v > max) return false;
  *out = (float)v;
  return true;
}

static bool visit(const char *section, const char *key, const char *value, int line, void *ud, ccord_error *err)
{
  theme_ctx *c = ud;
  ccord_theme *t = c->t;

  if (strcmp(section, "meta") == 0) {
    size_t n = strlen(value);
    if (strcmp(key, "id") == 0) {
      if (n == 0 || n > THEME_ID_MAX) {
        ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: id must be 1..%u bytes", line, THEME_ID_MAX);
        return false;
      }
      memcpy(t->id, value, n + 1);
      c->meta_seen[0] = true;
    } else if (strcmp(key, "name") == 0) {
      if (n == 0 || n > THEME_NAME_MAX) {
        ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: name must be 1..%u bytes", line, THEME_NAME_MAX);
        return false;
      }
      memcpy(t->name, value, n + 1);
      c->meta_seen[1] = true;
    } else {
      ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: unknown key '%s' in [meta]", line, key);
      return false;
    }
    return true;
  }

  if (strcmp(section, "colors") == 0) {
    for (size_t i = 0; i < THEME_NCOLORS; i++) {
      if (strcmp(color_names[i], key) != 0) continue;
      ccord_color col;
      if (!ccord_color_parse(value, &col)) {
        ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: '%s' is not #rrggbb or #rrggbbaa", line, key);
        return false;
      }
      void *dst = (char *)t + color_offsets[i];
      *(ccord_color *)dst = col;
      c->color_seen[i] = true;
      return true;
    }
    ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: unknown colour key '%s'", line, key);
    return false;
  }

  if (strcmp(section, "metrics") == 0) {
    for (size_t i = 0; i < THEME_NMETRICS; i++) {
      if (strcmp(metric_names[i], key) != 0) continue;
      double max = strcmp(key, "message_merge_ms") == 0 ? THEME_MERGE_MAX : THEME_METRIC_MAX;
      float v;
      if (!metric_value(value, max, &v)) {
        ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: '%s' must be a decimal number in range", line, key);
        return false;
      }
      void *dst = (char *)t + metric_offsets[i];
      *(float *)dst = v;
      c->metric_seen[i] = true;
      return true;
    }
    ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: unknown metric key '%s'", line, key);
    return false;
  }

  if (strcmp(section, "fonts") == 0) {
    for (size_t i = 0; i < 2; i++) {
      if (strcmp(font_names[i], key) != 0) continue;
      if (!font_path_ok(value)) {
        ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: '%s' must be a relative .ttf/.otf/.ttc path", line, key);
        return false;
      }
      memcpy((char *)t + font_offsets[i], value, strlen(value) + 1);
      c->font_seen[i] = true;
      return true;
    }
    ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: unknown font key '%s'", line, key);
    return false;
  }

  ccord_error_set(err, CCORD_ERR_PARSE, "theme line %d: unknown section [%s]", line, section);
  return false;
}

ccord_status ccord_theme_parse(ccord_theme *out, const char *text, size_t len, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || (!text && len)) return ccord_error_set(err, CCORD_ERR_INVALID, "theme: null argument");
  memset(out, 0, sizeof *out);
  theme_ctx ctx = {.t = out};
  ccord_status st = ccord_ini_parse(text, len, visit, &ctx, err);
  if (st != CCORD_OK) return st;

  if (!ctx.meta_seen[0]) return ccord_error_set(err, CCORD_ERR_PARSE, "theme: missing [meta] id");
  if (!ctx.meta_seen[1]) return ccord_error_set(err, CCORD_ERR_PARSE, "theme: missing [meta] name");
  for (size_t i = 0; i < THEME_NCOLORS; i++) {
    if (!ctx.color_seen[i]) return ccord_error_set(err, CCORD_ERR_PARSE, "theme: missing [colors] %s", color_names[i]);
  }
  for (size_t i = 0; i < THEME_NMETRICS; i++) {
    if (!ctx.metric_seen[i]) return ccord_error_set(err, CCORD_ERR_PARSE, "theme: missing [metrics] %s", metric_names[i]);
  }
  for (size_t i = 0; i < 2; i++) {
    if (!ctx.font_seen[i]) return ccord_error_set(err, CCORD_ERR_PARSE, "theme: missing [fonts] %s", font_names[i]);
  }
  return CCORD_OK;
}

ccord_status ccord_theme_load(ccord_theme *out, const char *path, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !path) return ccord_error_set(err, CCORD_ERR_INVALID, "theme: null argument");
  FILE *f = fopen(path, "rb");
  if (!f) return ccord_error_set(err, CCORD_ERR_IO, "theme: cannot open file");
  ccord_status st;
  if (fseek(f, 0, SEEK_END) != 0) {
    st = ccord_error_set(err, CCORD_ERR_IO, "theme: cannot read file");
    fclose(f);
    return st;
  }
  long size = ftell(f);
  if (size < 0 || (unsigned long)size > THEME_FILE_MAX) {
    fclose(f);
    return ccord_error_set(err, CCORD_ERR_RANGE, "theme: file is empty or larger than 64 KiB");
  }
  rewind(f);
  char *buf = ccord_malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "theme: out of memory");
  }
  size_t got = fread(buf, 1, (size_t)size, f);
  bool read_ok = got == (size_t)size && !ferror(f);
  fclose(f);
  if (!read_ok) {
    ccord_free(buf);
    return ccord_error_set(err, CCORD_ERR_IO, "theme: short read");
  }
  st = ccord_theme_parse(out, buf, (size_t)size, err);
  ccord_free(buf);
  return st;
}

ccord_status ccord_theme_load_builtin(ccord_theme *out, const char *id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out || !id) return ccord_error_set(err, CCORD_ERR_INVALID, "theme: null argument");
  size_t len = 0;
  const char *text = ccord_embedded_theme_by_id(id, &len);
  if (!text) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "theme: unknown built-in id '%s'", id);
  return ccord_theme_parse(out, text, len, err);
}

// ---- QuickCSS custom-property overrides ----

#define QUICK_CSS_MAX (64u * 1024u)

typedef struct css_span {
  const char *p;
  size_t n;
} css_span;

typedef enum qcss_kind { QCSS_KIND_NONE, QCSS_KIND_COLOR, QCSS_KIND_METRIC } qcss_kind;

typedef struct qcss_target {
  qcss_kind kind;
  size_t index;
} qcss_target;

static char css_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool css_space(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static void span_trim(css_span *s)
{
  while (s->n && css_space(s->p[0])) {
    s->p++;
    s->n--;
  }
  while (s->n && css_space(s->p[s->n - 1])) s->n--;
}

static bool span_is(css_span s, const char *lit)
{
  size_t n = strlen(lit);
  if (s.n != n) return false;
  for (size_t i = 0; i < n; i++) {
    if (css_lower(s.p[i]) != css_lower(lit[i])) return false;
  }
  return true;
}

static bool css_selector_supported(css_span sel)
{
  size_t start = 0;
  for (size_t i = 0; i <= sel.n; i++) {
    if (i != sel.n && sel.p[i] != ',') continue;
    css_span part = {sel.p + start, i - start};
    span_trim(&part);
    if (!span_is(part, ":root") && !span_is(part, "body") && !span_is(part, "*") &&
        !span_is(part, ".theme-dark") && !span_is(part, ".theme-light")) {
      return false;
    }
    start = i + 1;
  }
  return true;
}

static bool css_span_uint(css_span s, long max, long *out)
{
  span_trim(&s);
  if (s.n == 0 || s.n > 6) return false;
  long v = 0;
  for (size_t i = 0; i < s.n; i++) {
    if (s.p[i] < '0' || s.p[i] > '9') return false;
    v = v * 10 + (long)(s.p[i] - '0');
    if (v > max) return false;
  }
  *out = v;
  return true;
}

static bool css_span_alpha(css_span s, uint8_t *out)
{
  span_trim(&s);
  if (s.n == 0 || s.n >= 24) return false;
  bool dot = false;
  for (size_t i = 0; i < s.n; i++) {
    if (s.p[i] == '.') {
      if (dot) return false;
      dot = true;
    } else if (s.p[i] < '0' || s.p[i] > '9') {
      return false;
    }
  }
  if (s.p[0] == '.' || s.p[s.n - 1] == '.') return false;
  char buf[24];
  memcpy(buf, s.p, s.n);
  buf[s.n] = '\0';
  char *end = nullptr;
  double v = strtod(buf, &end);
  if (!end || *end != '\0' || v < 0 || v > 255) return false;
  if (v <= 1.0) v *= 255.0;
  *out = (uint8_t)(v + 0.5);
  return true;
}

static bool css_color_hex(css_span s, ccord_color *out)
{
  size_t digits = s.n - 1;
  uint8_t v[4] = {0, 0, 0, 255};
  if (digits == 3 || digits == 4) {
    for (size_t i = 0; i < digits; i++) {
      int h = hexnib(s.p[1 + i]);
      if (h < 0) return false;
      v[i] = (uint8_t)(h * 17);
    }
  } else if (digits == 6 || digits == 8) {
    for (size_t i = 0; i < digits / 2; i++) {
      int hi = hexnib(s.p[1 + 2 * i]);
      int lo = hexnib(s.p[2 + 2 * i]);
      if (hi < 0 || lo < 0) return false;
      v[i] = (uint8_t)(hi * 16 + lo);
    }
  } else {
    return false;
  }
  *out = (ccord_color){.r = v[0], .g = v[1], .b = v[2], .a = v[3]};
  return true;
}

static bool css_color_rgb(css_span s, ccord_color *out)
{
  size_t open = 3;
  bool alpha_fn = false;
  if (s.n > 3 && css_lower(s.p[3]) == 'a') {
    alpha_fn = true;
    open = 4;
  }
  if (s.n < open + 2 || s.p[open] != '(' || s.p[s.n - 1] != ')') return false;
  css_span inner = {s.p + open + 1, s.n - open - 2};
  css_span parts[4];
  size_t nparts = 0, start = 0;
  for (size_t i = 0; i <= inner.n; i++) {
    if (i != inner.n && inner.p[i] != ',') continue;
    if (nparts == 4) return false;
    parts[nparts++] = (css_span){inner.p + start, i - start};
    start = i + 1;
  }
  if (nparts != (alpha_fn ? 4u : 3u)) return false;
  long r = 0, g = 0, b = 0, a = 255;
  if (!css_span_uint(parts[0], 255, &r) || !css_span_uint(parts[1], 255, &g) ||
      !css_span_uint(parts[2], 255, &b)) {
    return false;
  }
  if (alpha_fn) {
    uint8_t av = 255;
    if (!css_span_alpha(parts[3], &av)) return false;
    a = (long)av;
  }
  *out = (ccord_color){.r = (uint8_t)r, .g = (uint8_t)g, .b = (uint8_t)b, .a = (uint8_t)a};
  return true;
}

static bool css_color_value(css_span s, ccord_color *out)
{
  span_trim(&s);
  if (s.n && s.p[0] == '#') return css_color_hex(s, out);
  if (s.n >= 4 && css_lower(s.p[0]) == 'r' && css_lower(s.p[1]) == 'g' && css_lower(s.p[2]) == 'b' &&
      s.p[3] == '(') {
    return css_color_rgb(s, out);
  }
  if (s.n >= 5 && css_lower(s.p[0]) == 'r' && css_lower(s.p[1]) == 'g' && css_lower(s.p[2]) == 'b' &&
      css_lower(s.p[3]) == 'a' && s.p[4] == '(') {
    return css_color_rgb(s, out);
  }
  return false;
}

static bool css_metric_value(css_span s, double max, float *out)
{
  span_trim(&s);
  if (s.n >= 2 && css_lower(s.p[s.n - 2]) == 'p' && css_lower(s.p[s.n - 1]) == 'x') {
    s.n -= 2;
    span_trim(&s);
  }
  char buf[24];
  if (s.n == 0 || s.n >= sizeof buf) return false;
  memcpy(buf, s.p, s.n);
  buf[s.n] = '\0';
  return metric_value(buf, max, out);
}

// Discord custom properties whose names differ from the C-Cord theme keys.
static const struct {
  const char *css;
  const char *field;
} qcss_discord_colors[] = {
  {"background-primary", "bg_primary"},
  {"background-secondary", "bg_secondary"},
  {"background-secondary-alt", "bg_user_panel"},
  {"background-tertiary", "bg_tertiary"},
  {"background-accent", "bg_item_selected"},
  {"background-floating", "bg_floating"},
  {"channeltextarea-background", "bg_input"},
  {"background-modifier-hover", "bg_message_hover"},
  {"background-modifier-selected", "bg_item_selected"},
  {"background-modifier-accent", "divider"},
  {"background-mentioned", "mention_bg"},
  {"text-normal", "text_normal"},
  {"text-muted", "text_muted"},
  {"text-link", "text_link"},
  {"header-primary", "text_header"},
  {"header-secondary", "text_secondary"},
  {"interactive-normal", "text_muted"},
  {"interactive-hover", "text_interactive_hover"},
  {"interactive-active", "text_interactive_active"},
  {"interactive-muted", "text_placeholder"},
  {"brand-experiment", "brand"},
  {"brand-experiment-560", "brand_hover"},
  {"status-danger", "danger"},
  {"status-positive", "status_online"},
  {"status-warning", "status_idle"},
  {"scrollbar-thin-thumb", "scrollbar_thumb"},
  {"scrollbar-thin-track", "scrollbar_track"},
  {"text-selection", "selection"},
};

static size_t qcss_color_index(const char *name)
{
  for (size_t i = 0; i < THEME_NCOLORS; i++) {
    if (strcmp(color_names[i], name) == 0) return i;
  }
  return THEME_NCOLORS;
}

static qcss_target qcss_lookup(css_span prop)
{
  qcss_target none = {.kind = QCSS_KIND_NONE, .index = 0};
  span_trim(&prop);
  if (prop.n < 3 || prop.p[0] != '-' || prop.p[1] != '-' || prop.n >= 128) return none;

  char name[128];
  size_t n = prop.n - 2;
  for (size_t i = 0; i < n; i++) name[i] = css_lower(prop.p[2 + i]);
  name[n] = '\0';

  for (size_t i = 0; i < sizeof qcss_discord_colors / sizeof qcss_discord_colors[0]; i++) {
    if (strcmp(name, qcss_discord_colors[i].css) != 0) continue;
    size_t idx = qcss_color_index(qcss_discord_colors[i].field);
    if (idx < THEME_NCOLORS) return (qcss_target){.kind = QCSS_KIND_COLOR, .index = idx};
    return none;
  }

  // C-Cord aliases: --ccord-<theme key> and --<theme key>, with '-' or '_' separators.
  char canon[128];
  size_t cn = 0;
  for (size_t i = 0; i < n; i++) canon[cn++] = name[i] == '-' ? '_' : name[i];
  canon[cn] = '\0';
  const char *key = canon;
  if (strncmp(key, "ccord_", 6) == 0) key += 6;
  for (size_t i = 0; i < THEME_NCOLORS; i++) {
    if (strcmp(color_names[i], key) == 0) return (qcss_target){.kind = QCSS_KIND_COLOR, .index = i};
  }
  for (size_t i = 0; i < THEME_NMETRICS; i++) {
    if (strcmp(metric_names[i], key) == 0) return (qcss_target){.kind = QCSS_KIND_METRIC, .index = i};
  }
  return none;
}

static void qcss_strip_important(css_span *value)
{
  static const char suffix[] = "important";
  const size_t sn = sizeof suffix - 1;
  span_trim(value);
  if (value->n < sn + 1) return;
  for (size_t i = 0; i < sn; i++) {
    if (css_lower(value->p[value->n - sn + i]) != suffix[i]) return;
  }
  size_t k = value->n - sn;
  while (k && css_space(value->p[k - 1])) k--;
  if (k == 0 || value->p[k - 1] != '!') return;
  css_span out = {value->p, k - 1};
  span_trim(&out);
  *value = out;
}

static void qcss_apply_declaration(ccord_theme *t, css_span decl, size_t *applied)
{
  span_trim(&decl);
  if (decl.n == 0) return;
  size_t colon = decl.n;
  for (size_t i = 0; i < decl.n; i++) {
    if (decl.p[i] == ':') {
      colon = i;
      break;
    }
  }
  if (colon == decl.n) return;
  css_span prop = {decl.p, colon};
  css_span value = {decl.p + colon + 1, decl.n - colon - 1};
  span_trim(&prop);
  qcss_strip_important(&value);
  qcss_target tgt = qcss_lookup(prop);
  if (tgt.kind == QCSS_KIND_COLOR) {
    ccord_color col;
    if (!css_color_value(value, &col)) return;
    void *dst = (char *)t + color_offsets[tgt.index];
    *(ccord_color *)dst = col;
  } else if (tgt.kind == QCSS_KIND_METRIC) {
    double max = strcmp(metric_names[tgt.index], "message_merge_ms") == 0 ? THEME_MERGE_MAX : THEME_METRIC_MAX;
    float v;
    if (!css_metric_value(value, max, &v)) return;
    void *dst = (char *)t + metric_offsets[tgt.index];
    *(float *)dst = v;
  } else {
    return;
  }
  if (applied) (*applied)++;
}

static void qcss_apply_declarations(ccord_theme *t, const char *text, size_t len, size_t *applied)
{
  size_t start = 0, i = 0;
  while (i < len) {
    if (text[i] == ';') {
      qcss_apply_declaration(t, (css_span){text + start, i - start}, applied);
      i++;
      start = i;
    } else if (text[i] == '{') {
      // Nested rules are unsupported: skip the balanced block rather than parse its contents.
      size_t depth = 1;
      i++;
      while (i < len && depth) {
        if (text[i] == '{') {
          depth++;
        } else if (text[i] == '}') {
          depth--;
        }
        i++;
      }
      start = i;
    } else if (text[i] == '}') {
      i++;
      start = i;
    } else {
      i++;
    }
  }
  qcss_apply_declaration(t, (css_span){text + start, len - start}, applied);
}

static void qcss_strip_comments(char *s, size_t len, size_t *out_len)
{
  size_t r = 0, w = 0;
  bool in_comment = false;
  while (r < len) {
    if (!in_comment) {
      if (r + 1 < len && s[r] == '/' && s[r + 1] == '*') {
        in_comment = true;
        r += 2;
        s[w++] = ' ';
      } else {
        s[w++] = s[r++];
      }
    } else if (r + 1 < len && s[r] == '*' && s[r + 1] == '/') {
      in_comment = false;
      r += 2;
    } else {
      r++;
    }
  }
  s[w] = '\0';
  *out_len = w;
}

static void qcss_run(ccord_theme *t, const char *css, size_t len, size_t *applied)
{
  size_t seg = 0, i = 0;
  while (i < len) {
    if (css[i] == ';') {
      qcss_apply_declaration(t, (css_span){css + seg, i - seg}, applied);
      i++;
      seg = i;
    } else if (css[i] == '{') {
      css_span sel = {css + seg, i - seg};
      size_t depth = 1, j = i + 1;
      while (j < len && depth) {
        if (css[j] == '{') {
          depth++;
        } else if (css[j] == '}') {
          depth--;
        }
        j++;
      }
      size_t body_end = depth ? len : j - 1;
      if (css_selector_supported(sel)) {
        qcss_apply_declarations(t, css + i + 1, body_end - (i + 1), applied);
      }
      i = depth ? len : j;
      seg = i;
    } else if (css[i] == '}') {
      i++;
      seg = i;
    } else {
      i++;
    }
  }
  if (seg < len) qcss_apply_declaration(t, (css_span){css + seg, len - seg}, applied);
}

ccord_status ccord_theme_apply_quick_css(ccord_theme *theme, const char *css, size_t len,
                                        size_t *applied, ccord_error *err)
{
  ccord_error_clear(err);
  if (applied) *applied = 0;
  if (!theme || (!css && len)) return ccord_error_set(err, CCORD_ERR_INVALID, "quickcss: null argument");
  if (len == 0) return CCORD_OK;
  if (len > QUICK_CSS_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "quickcss: text larger than 64 KiB");
  char *buf = ccord_malloc(len + 1);
  if (!buf) return ccord_error_set(err, CCORD_ERR_NOMEM, "quickcss: out of memory");
  memcpy(buf, css, len);
  buf[len] = '\0';
  size_t clean = 0;
  qcss_strip_comments(buf, len, &clean);
  size_t count = 0;
  qcss_run(theme, buf, clean, &count);
  ccord_free(buf);
  if (applied) *applied = count;
  return CCORD_OK;
}
