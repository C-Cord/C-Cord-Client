// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Lightweight, zero-dependency syntax highlighter for fenced Markdown code blocks.
// Pure: source line + language tag -> contiguous token runs (never strips or mutates bytes).
#include <ctype.h>
#include <string.h>

#include "ui_internal.h"

static bool eq_ci(const char *a, size_t alen, const char *lit)
{
  size_t blen = strlen(lit);
  if (alen != blen) return false;
  for (size_t i = 0; i < alen; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)lit[i])) return false;
  }
  return true;
}

static bool in_word_list(const char *w, size_t len, const char *const *list, bool ci)
{
  for (size_t i = 0; list[i]; i++) {
    if (ci) {
      if (eq_ci(w, len, list[i])) return true;
    } else {
      if (strlen(list[i]) == len && memcmp(w, list[i], len) == 0) return true;
    }
  }
  return false;
}

ccord_hl_lang ccord_md_hl_detect_lang(const char *tag, size_t len, char *out_label, size_t label_cap)
{
  while (len && (*tag == ' ' || *tag == '\t')) { tag++; len--; }
  while (len && (tag[len - 1] == ' ' || tag[len - 1] == '\t' || tag[len - 1] == '\r')) len--;
  // Stop at whitespace if extra fence info follows the language identifier.
  for (size_t i = 0; i < len; i++) {
    if (tag[i] == ' ' || tag[i] == '\t') { len = i; break; }
  }
  if (out_label && label_cap > 0) {
    size_t copy = len < label_cap - 1 ? len : label_cap - 1;
    for (size_t i = 0; i < copy; i++) out_label[i] = (char)tolower((unsigned char)tag[i]);
    out_label[copy] = '\0';
  }
  if (len == 0) return CCORD_HL_NONE;
  if (eq_ci(tag, len, "c") || eq_ci(tag, len, "h") || eq_ci(tag, len, "cpp") || eq_ci(tag, len, "c++") ||
      eq_ci(tag, len, "cc") || eq_ci(tag, len, "cxx") || eq_ci(tag, len, "hpp") || eq_ci(tag, len, "objc") ||
      eq_ci(tag, len, "java") || eq_ci(tag, len, "cs") || eq_ci(tag, len, "csharp") || eq_ci(tag, len, "kt") ||
      eq_ci(tag, len, "kotlin") || eq_ci(tag, len, "swift") || eq_ci(tag, len, "zig")) {
    return CCORD_HL_C;
  }
  if (eq_ci(tag, len, "rs") || eq_ci(tag, len, "rust")) return CCORD_HL_RUST;
  if (eq_ci(tag, len, "py") || eq_ci(tag, len, "python")) return CCORD_HL_PYTHON;
  if (eq_ci(tag, len, "js") || eq_ci(tag, len, "jsx") || eq_ci(tag, len, "ts") || eq_ci(tag, len, "tsx") ||
      eq_ci(tag, len, "javascript") || eq_ci(tag, len, "typescript")) {
    return CCORD_HL_JS;
  }
  if (eq_ci(tag, len, "go") || eq_ci(tag, len, "golang")) return CCORD_HL_GO;
  if (eq_ci(tag, len, "sh") || eq_ci(tag, len, "bash") || eq_ci(tag, len, "zsh") || eq_ci(tag, len, "shell") ||
      eq_ci(tag, len, "fish")) {
    return CCORD_HL_SHELL;
  }
  if (eq_ci(tag, len, "lua")) return CCORD_HL_LUA;
  if (eq_ci(tag, len, "json") || eq_ci(tag, len, "jsonc")) return CCORD_HL_JSON;
  if (eq_ci(tag, len, "sql")) return CCORD_HL_SQL;
  if (eq_ci(tag, len, "html") || eq_ci(tag, len, "xml") || eq_ci(tag, len, "css") || eq_ci(tag, len, "scss")) {
    return CCORD_HL_MARKUP;
  }
  if (eq_ci(tag, len, "yaml") || eq_ci(tag, len, "yml") || eq_ci(tag, len, "toml") || eq_ci(tag, len, "ini") ||
      eq_ci(tag, len, "cfg") || eq_ci(tag, len, "conf") || eq_ci(tag, len, "cmake")) {
    return CCORD_HL_CONFIG;
  }
  if (eq_ci(tag, len, "diff") || eq_ci(tag, len, "patch")) return CCORD_HL_DIFF;
  return CCORD_HL_NONE;
}

static bool is_ident_start(unsigned char c) { return isalpha(c) || c == '_'; }
static bool is_ident_cont(unsigned char c) { return isalnum(c) || c == '_'; }

static void emit_run(ccord_hl_run *runs, size_t max_runs, size_t *nr, size_t off, size_t len, ccord_hl_kind kind)
{
  if (len == 0) return;
  if (*nr > 0 && runs[*nr - 1].kind == kind && runs[*nr - 1].off + runs[*nr - 1].len == off) {
    runs[*nr - 1].len += len;
    return;
  }
  if (*nr < max_runs) {
    runs[(*nr)++] = (ccord_hl_run){.off = off, .len = len, .kind = kind};
  } else if (*nr > 0) {
    runs[*nr - 1].len = (off + len) - runs[*nr - 1].off;
  }
}

static bool is_keyword(ccord_hl_lang lang, const char *w, size_t len)
{
  static const char *const k_c[] = {
    "auto", "break", "case", "class", "const", "constexpr", "continue", "default", "do", "else", "enum", "extern",
    "false", "final", "for", "goto", "if", "import", "inline", "namespace", "new", "nullptr", "NULL", "override",
    "private", "protected", "public", "return", "sizeof", "static", "static_assert", "struct", "switch", "template",
    "this", "throw", "true", "try", "catch", "typedef", "typeof", "union", "using", "virtual", "volatile", "while",
    nullptr,
  };
  static const char *const k_rust[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern", "false", "fn",
    "for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self",
    "static", "struct", "super", "trait", "true", "type", "unsafe", "use", "where", "while", nullptr,
  };
  static const char *const k_py[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except",
    "False", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None", "nonlocal", "not",
    "or", "pass", "raise", "return", "True", "try", "while", "with", "yield", "match", "case", nullptr,
  };
  static const char *const k_js[] = {
    "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do",
    "else", "enum", "export", "extends", "false", "finally", "for", "from", "function", "if", "implements", "import",
    "in", "instanceof", "interface", "let", "new", "null", "of", "package", "private", "protected", "public", "readonly",
    "return", "static", "super", "switch", "this", "throw", "true", "try", "type", "typeof", "undefined", "var", "void",
    "while", "yield", nullptr,
  };
  static const char *const k_go[] = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "false", "for", "func",
    "go", "goto", "if", "import", "interface", "map", "nil", "package", "range", "return", "select", "struct", "switch",
    "true", "type", "var", nullptr,
  };
  static const char *const k_sh[] = {
    "case", "do", "done", "elif", "else", "esac", "export", "fi", "for", "function", "if", "in", "local", "readonly",
    "return", "select", "set", "shift", "then", "true", "false", "unset", "until", "while", nullptr,
  };
  static const char *const k_lua[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil",
    "not", "or", "repeat", "return", "then", "true", "until", "while", nullptr,
  };
  static const char *const k_json[] = {"true", "false", "null", nullptr};
  static const char *const k_sql[] = {
    "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "INDEX",
    "DROP", "ALTER", "JOIN", "LEFT", "RIGHT", "INNER", "OUTER", "ON", "AND", "OR", "NOT", "NULL", "AS", "IN", "IS",
    "ORDER", "BY", "GROUP", "HAVING", "LIMIT", "OFFSET", "UNION", "ALL", "DISTINCT", "CASE", "WHEN", "THEN", "ELSE",
    "END", "TRUE", "FALSE", "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "DEFAULT", "BEGIN", "COMMIT", "ROLLBACK", nullptr,
  };
  switch (lang) {
    case CCORD_HL_C: return in_word_list(w, len, k_c, false);
    case CCORD_HL_RUST: return in_word_list(w, len, k_rust, false);
    case CCORD_HL_PYTHON: return in_word_list(w, len, k_py, false);
    case CCORD_HL_JS: return in_word_list(w, len, k_js, false);
    case CCORD_HL_GO: return in_word_list(w, len, k_go, false);
    case CCORD_HL_SHELL: return in_word_list(w, len, k_sh, false);
    case CCORD_HL_LUA: return in_word_list(w, len, k_lua, false);
    case CCORD_HL_JSON: return in_word_list(w, len, k_json, false);
    case CCORD_HL_SQL: return in_word_list(w, len, k_sql, true);
    case CCORD_HL_CONFIG: return in_word_list(w, len, k_json, true);
    default: return false;
  }
}

static bool is_type_name(ccord_hl_lang lang, const char *w, size_t len)
{
  static const char *const k_types[] = {
    "void", "bool", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "size_t", "ssize_t",
    "ptrdiff_t", "intptr_t", "uintptr_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
    "uint64_t", "string", "String", "Vec", "Option", "Result", "Box", "Arc", "Rc", "u8", "u16", "u32", "u64", "usize",
    "i8", "i16", "i32", "i64", "isize", "f32", "f64", "str", "byte", "rune", "error", "any", "number", "boolean",
    "never", "unknown", "dict", "list", "tuple", "set", "bytes", nullptr,
  };
  if (lang == CCORD_HL_JSON || lang == CCORD_HL_SHELL || lang == CCORD_HL_CONFIG || lang == CCORD_HL_DIFF) return false;
  return in_word_list(w, len, k_types, false);
}

static bool uses_c_comments(ccord_hl_lang lang)
{
  return lang == CCORD_HL_C || lang == CCORD_HL_RUST || lang == CCORD_HL_JS || lang == CCORD_HL_GO ||
         lang == CCORD_HL_JSON || lang == CCORD_HL_MARKUP;
}

static bool uses_hash_comments(ccord_hl_lang lang)
{
  return lang == CCORD_HL_PYTHON || lang == CCORD_HL_SHELL || lang == CCORD_HL_CONFIG;
}

static bool uses_dash_comments(ccord_hl_lang lang)
{
  return lang == CCORD_HL_LUA || lang == CCORD_HL_SQL;
}

size_t ccord_md_hl_tokenize(ccord_hl_lang lang, const char *line, size_t len, ccord_hl_run *runs, size_t max_runs,
                            ccord_hl_state *st)
{
  if (!line || len == 0 || !runs || max_runs == 0) return 0;
  size_t nr = 0;
  if (lang == CCORD_HL_NONE) {
    emit_run(runs, max_runs, &nr, 0, len, CCORD_HL_PLAIN);
    return nr;
  }
  if (lang == CCORD_HL_DIFF) {
    ccord_hl_kind k = CCORD_HL_PLAIN;
    if (len >= 2 && line[0] == '@' && line[1] == '@') k = CCORD_HL_PREPROC;
    else if (line[0] == '+' && !(len >= 3 && line[1] == '+' && line[2] == '+')) k = CCORD_HL_DIFF_ADD;
    else if (line[0] == '-' && !(len >= 3 && line[1] == '-' && line[2] == '-')) k = CCORD_HL_DIFF_DEL;
    else if ((len >= 3 && (memcmp(line, "+++", 3) == 0 || memcmp(line, "---", 3) == 0)) ||
             (len >= 5 && memcmp(line, "diff ", 5) == 0)) {
      k = CCORD_HL_KEYWORD;
    }
    emit_run(runs, max_runs, &nr, 0, len, k);
    return nr;
  }

  size_t i = 0;
  if (st && st->in_block_comment) {
    size_t start = 0;
    while (i + 1 < len) {
      if (line[i] == '*' && line[i + 1] == '/') {
        i += 2;
        st->in_block_comment = false;
        break;
      }
      i++;
    }
    if (st->in_block_comment) i = len;
    emit_run(runs, max_runs, &nr, start, i - start, CCORD_HL_COMMENT);
  }

  // Leading preprocessor directive in C/C++ (#include, #define, ...) or decorator in Python/Rust (@foo).
  size_t ws = i;
  while (ws < len && (line[ws] == ' ' || line[ws] == '\t')) ws++;
  if (lang == CCORD_HL_C && ws < len && line[ws] == '#') {
    if (ws > i) emit_run(runs, max_runs, &nr, i, ws - i, CCORD_HL_PLAIN);
    size_t k = ws + 1;
    while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
    while (k < len && is_ident_cont((unsigned char)line[k])) k++;
    emit_run(runs, max_runs, &nr, ws, k - ws, CCORD_HL_PREPROC);
    i = k;
  }

  while (i < len) {
    unsigned char ch = (unsigned char)line[i];
    // Comments
    if (uses_c_comments(lang) && i + 1 < len && line[i] == '/' && line[i + 1] == '/') {
      emit_run(runs, max_runs, &nr, i, len - i, CCORD_HL_COMMENT);
      break;
    }
    if (uses_c_comments(lang) && i + 1 < len && line[i] == '/' && line[i + 1] == '*') {
      size_t start = i;
      i += 2;
      bool closed = false;
      while (i + 1 < len) {
        if (line[i] == '*' && line[i + 1] == '/') { i += 2; closed = true; break; }
        i++;
      }
      if (!closed) {
        i = len;
        if (st) st->in_block_comment = true;
      }
      emit_run(runs, max_runs, &nr, start, i - start, CCORD_HL_COMMENT);
      continue;
    }
    if (uses_hash_comments(lang) && ch == '#') {
      emit_run(runs, max_runs, &nr, i, len - i, CCORD_HL_COMMENT);
      break;
    }
    if (uses_dash_comments(lang) && i + 1 < len && line[i] == '-' && line[i + 1] == '-') {
      emit_run(runs, max_runs, &nr, i, len - i, CCORD_HL_COMMENT);
      break;
    }
    // Strings
    if (ch == '"' || ch == '\'' || (ch == '`' && (lang == CCORD_HL_JS || lang == CCORD_HL_GO || lang == CCORD_HL_SHELL))) {
      size_t start = i++;
      while (i < len) {
        if (line[i] == '\\' && i + 1 < len) { i += 2; continue; }
        if ((unsigned char)line[i] == ch) { i++; break; }
        i++;
      }
      // JSON object keys ("key":) highlight as keyword/property for quick scannability.
      ccord_hl_kind sk = CCORD_HL_STRING;
      if (lang == CCORD_HL_JSON && ch == '"') {
        size_t p = i;
        while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
        if (p < len && line[p] == ':') sk = CCORD_HL_FUNC;
      }
      emit_run(runs, max_runs, &nr, start, i - start, sk);
      continue;
    }
    // Markup tags (<div ...>)
    if (lang == CCORD_HL_MARKUP && ch == '<') {
      size_t start = i++;
      if (i < len && (line[i] == '/' || line[i] == '!')) i++;
      while (i < len && (is_ident_cont((unsigned char)line[i]) || line[i] == '-' || line[i] == ':')) i++;
      emit_run(runs, max_runs, &nr, start, i - start, CCORD_HL_KEYWORD);
      continue;
    }
    // Numbers
    if (isdigit(ch) && (i == 0 || !is_ident_cont((unsigned char)line[i - 1]))) {
      size_t start = i++;
      if (ch == '0' && i < len && (line[i] == 'x' || line[i] == 'X' || line[i] == 'b' || line[i] == 'B')) {
        i++;
        while (i < len && (isxdigit((unsigned char)line[i]) || line[i] == '_')) i++;
      } else {
        while (i < len && (isdigit((unsigned char)line[i]) || line[i] == '.' || line[i] == '_' ||
                           line[i] == 'e' || line[i] == 'E')) {
          i++;
        }
      }
      while (i < len && isalpha((unsigned char)line[i])) i++;   // suffixes: u, ul, f, i32
      emit_run(runs, max_runs, &nr, start, i - start, CCORD_HL_NUMBER);
      continue;
    }
    // Shell variables ($FOO / ${BAR}) or decorators (@name)
    if ((lang == CCORD_HL_SHELL && ch == '$') || ((lang == CCORD_HL_PYTHON || lang == CCORD_HL_C) && ch == '@')) {
      size_t start = i++;
      if (i < len && line[i] == '{') {
        while (i < len && line[i] != '}') i++;
        if (i < len) i++;
      } else {
        while (i < len && is_ident_cont((unsigned char)line[i])) i++;
      }
      emit_run(runs, max_runs, &nr, start, i - start, CCORD_HL_PREPROC);
      continue;
    }
    // Identifiers / keywords / types / function calls
    if (is_ident_start(ch)) {
      size_t start = i++;
      while (i < len && is_ident_cont((unsigned char)line[i])) i++;
      size_t wlen = i - start;
      ccord_hl_kind kind = CCORD_HL_PLAIN;
      if (is_keyword(lang, line + start, wlen)) {
        kind = CCORD_HL_KEYWORD;
      } else if (is_type_name(lang, line + start, wlen)) {
        kind = CCORD_HL_TYPE;
      } else if (lang == CCORD_HL_CONFIG) {
        size_t p = i;
        while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
        if (p < len && (line[p] == ':' || line[p] == '=')) kind = CCORD_HL_FUNC;
      } else {
        size_t p = i;
        while (p < len && (line[p] == ' ' || line[p] == '\t')) p++;
        if (p < len && line[p] == '(') kind = CCORD_HL_FUNC;
      }
      emit_run(runs, max_runs, &nr, start, wlen, kind);
      continue;
    }
    emit_run(runs, max_runs, &nr, i, 1, CCORD_HL_PLAIN);
    i++;
  }
  return nr;
}

static ccord_color hl_rgb(uint32_t hex)
{
  return (ccord_color){(uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex, 255};
}

ccord_color ccord_md_hl_color(ccord_hl_kind kind, ccord_color bg, ccord_color fallback)
{
  // Pick dark- or light-background palette based on code-block background luminance.
  int lum = (int)bg.r * 299 + (int)bg.g * 587 + (int)bg.b * 114;
  bool light = lum > 140000;
  switch (kind) {
    case CCORD_HL_KEYWORD: return hl_rgb(light ? 0xA626A4u : 0xC678DDu);
    case CCORD_HL_TYPE:    return hl_rgb(light ? 0xC18401u : 0xE5C07Bu);
    case CCORD_HL_STRING:  return hl_rgb(light ? 0x50A14Fu : 0x98C379u);
    case CCORD_HL_NUMBER:  return hl_rgb(light ? 0x986801u : 0xD19A66u);
    case CCORD_HL_COMMENT: return hl_rgb(light ? 0x7A828Eu : 0x7F848Eu);
    case CCORD_HL_PREPROC: return hl_rgb(light ? 0x0184BCu : 0x56B6C2u);
    case CCORD_HL_FUNC:    return hl_rgb(light ? 0x4078F2u : 0x61AFEFu);
    case CCORD_HL_DIFF_ADD: return hl_rgb(light ? 0x2E8B57u : 0x57F287u);
    case CCORD_HL_DIFF_DEL: return hl_rgb(light ? 0xD73A49u : 0xED4245u);
    default: return fallback;
  }
}
