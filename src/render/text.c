// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#include <SDL3_ttf/SDL_textengine.h>

#include "render_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>

#define MAX_TEXT_BYTES 8192u   // longer strings are cut (callers cap messages at 4000 bytes)

// ---- fonts ----

static bool has_fallback_family(const ccord_renderer *r, const char *fam)
{
  if (!fam || fam[0] == '\0') return false;
  for (int i = 0; i < r->nfallbacks; i++) {
    if (!r->fallback_base[i]) continue;
    const char *existing = TTF_GetFontFamilyName(r->fallback_base[i]);
    if (existing && strcmp(existing, fam) == 0) return true;
  }
  return false;
}

static void text_entries_flush(ccord_renderer *r);

// Attaches `fb` (owned from here on) as the next fallback to both base faces and to every sized font already made.
// A font that gains a fallback gets a new generation, and SDL_ttf then re-lays out every TTF_Text made from it the next time
// it is drawn. That drops what normalize_bitmap_glyphs did to the cached text (a colour emoji's bitmap is cut back to the
// line height there), so the emoji came out at full size over the chat. The cached texts are dropped here instead and
// rebuilt, normalised, on their next use. No caller holds a text_entry across a text_get that can reach this.
static void adopt_fallback(ccord_renderer *r, TTF_Font *fb, const char *path)
{
  int idx = r->nfallbacks++;
  r->fallback_base[idx] = fb;
  snprintf(r->fallback_path[idx], sizeof r->fallback_path[idx], "%s", path);
  for (int b = 0; b < 2; b++) {
    if (r->base[b]) (void)TTF_AddFallbackFont(r->base[b], fb);
  }
  for (int i = 0; i < r->nfonts; i++) {
    font_entry *fe = &r->fonts[i];
    if (!fe->font || fe->nfallbacks >= CCORD_MAX_FALLBACKS) continue;
    TTF_Font *copy = TTF_CopyFont(fb);
    if (!copy) continue;
    TTF_SetFontSize(copy, (float)fe->px);
    TTF_SetFontHinting(copy, TTF_HINTING_LIGHT);
    if (TTF_AddFallbackFont(fe->font, copy)) {
      fe->fallbacks[fe->nfallbacks++] = copy;
    } else {
      TTF_CloseFont(copy);
    }
  }
  text_entries_flush(r);
}

static bool try_bundled(ccord_renderer *r, const char *path)
{
  if (r->nfallbacks >= CCORD_MAX_FALLBACKS) return false;
  TTF_Font *fb = TTF_OpenFont(path, 16.0f);
  if (!fb) return false;
  const char *fam = TTF_GetFontFamilyName(fb);
  if (has_fallback_family(r, fam)) {
    TTF_CloseFont(fb);
    return false;
  }
  adopt_fallback(r, fb, path);
  r->nbundled = r->nfallbacks;
  return true;
}

// Only the bundled OFL subsets are opened here, so startup touches no system font. Code points they and the base
// face do not cover are looked up on the host the first time text needs them (ensure_coverage).
void fallback_fonts_init(ccord_renderer *r)
{
  if (!r || r->fallbacks_probed) return;
  r->fallbacks_probed = true;
  static const char *const k_bundled_rel[] = {
    "fonts/NotoColorEmoji-Subset.ttf",
    "fonts/NotoSansCJK-Subset.ttf",
  };
  char path[1024];
  for (size_t i = 0; i < sizeof k_bundled_rel / sizeof k_bundled_rel[0]; i++) {
    ccord_error err;
    bool loaded = false;
    if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), k_bundled_rel[i], &err) == CCORD_OK) {
      loaded = try_bundled(r, path);
    }
#if defined(__ANDROID__)
    // On Android, SDL_IOFromFile reads directly from APK assets/ via AAssetManager.
    if (!loaded) {
      SDL_IOStream *io = SDL_IOFromFile(k_bundled_rel[i], "rb");
      if (io) {
        TTF_Font *fb = TTF_OpenFontIO(io, true, 16.0f);
        if (fb) {
          const char *fam = TTF_GetFontFamilyName(fb);
          if (has_fallback_family(r, fam)) {
            TTF_CloseFont(fb);
          } else {
            adopt_fallback(r, fb, k_bundled_rel[i]);
            r->nbundled = r->nfallbacks;
            loaded = true;
          }
        }
      }
    }
#endif
    (void)loaded;
  }
}

// ---- code point classes ----

// Characters HarfBuzz already hides (zero advance, no ink) even when no font has them; they also steer shaping (ZWJ
// in emoji, ZWNJ in Persian, variation selectors), so they are never cut and never searched for.
static bool cp_default_ignorable(uint32_t cp)
{
  return cp == 0x00AD || cp == 0x034F || cp == 0x061C || (cp >= 0x115F && cp <= 0x1160) || (cp >= 0x17B4 && cp <= 0x17B5) ||
         (cp >= 0x180B && cp <= 0x180F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x206F) || cp == 0x3164 || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 ||
         (cp >= 0xFFF0 && cp <= 0xFFF8) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) || (cp >= 0x1D173 && cp <= 0x1D17A) ||
         (cp >= 0xE0000 && cp <= 0xE0FFF);
}

// Controls that no font draws and that have no width in a chat message: C0 (except \n and \t), DEL, C1, and the line
// and paragraph separators. Cut out before shaping, otherwise the base face draws its .notdef box for each.
static bool cp_hidden(uint32_t cp)
{
  return (cp < 0x20 && cp != '\n' && cp != '\t') || (cp >= 0x7F && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029 ||
         (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
}

static bool text_has_hidden(const char *s, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    uint8_t b = (uint8_t)s[i];
    if (b < 0x20 && b != '\n') return true;   // includes \t: it becomes a space
    if (b == 0x7F) return true;
    if (b == 0xC2 && i + 1 < len && (uint8_t)s[i + 1] >= 0x80 && (uint8_t)s[i + 1] <= 0x9F) return true;
    if (b == 0xE2 && i + 2 < len && (uint8_t)s[i + 1] == 0x80 && ((uint8_t)s[i + 2] == 0xA8 || (uint8_t)s[i + 2] == 0xA9)) return true;
  }
  return false;
}

// Builds the text that is shaped: hidden characters removed, a tab turned into one space (same byte length, so the
// offsets of everything else stay put). o2s/s2o translate byte offsets both ways. false: nothing to change or no memory.
static bool sanitize_text(const char *s, size_t len, char **out, size_t *out_len, uint32_t **o2s, uint32_t **s2o)
{
  if (len == 0 || len > UINT32_MAX / 2 || !text_has_hidden(s, len)) return false;
  char *o = ccord_malloc(len + 1);
  uint32_t *m1 = ccord_malloc((len + 1) * sizeof *m1);
  uint32_t *m2 = ccord_malloc((len + 1) * sizeof *m2);
  if (!o || !m1 || !m2) {
    ccord_free(o);
    ccord_free(m1);
    ccord_free(m2);
    return false;
  }
  size_t i = 0, j = 0;
  while (i < len) {
    char32_t cp = 0;
    int n = ccord_utf8_decode(s + i, len - i, &cp);
    if (n <= 0) {
      n = 1;
      cp = 0x80000000u;   // stray byte: kept as it is
    }
    if (cp == '\t') {
      o[j] = ' ';
      m1[i] = (uint32_t)j;
      m2[j++] = (uint32_t)i;
    } else if (cp != 0x80000000u && cp_hidden(cp)) {
      for (int k = 0; k < n; k++) m1[i + (size_t)k] = (uint32_t)j;
    } else {
      for (int k = 0; k < n; k++) {
        o[j] = s[i + (size_t)k];
        m1[i + (size_t)k] = (uint32_t)j;
        m2[j++] = (uint32_t)(i + (size_t)k);
      }
    }
    i += (size_t)n;
  }
  o[j] = '\0';
  m1[len] = (uint32_t)j;
  m2[j] = (uint32_t)len;
  *out = o;
  *out_len = j;
  *o2s = m1;
  *s2o = m2;
  return true;
}

// ---- run-time font lookup (first miss only) ----

static bool font_file_ext(const char *name)
{
  const char *dot = strrchr(name, '.');
  if (!dot) return false;
  return strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 || strcasecmp(dot, ".ttc") == 0 || strcasecmp(dot, ".otc") == 0;
}

static bool already_adopted(const ccord_renderer *r, const char *path)
{
  for (int i = 0; i < r->nfallbacks; i++) {
    if (strcmp(r->fallback_path[i], path) == 0) return true;
  }
  return false;
}

// Opens `path` (face `face`) and keeps it as a fallback when it really has `cp`.
static bool try_system_font(ccord_renderer *r, const char *path, int face, uint32_t cp)
{
  if (!font_file_ext(path) || strlen(path) >= CCORD_FALLBACK_PATH_MAX) return false;
  char id[CCORD_FALLBACK_PATH_MAX];
  snprintf(id, sizeof id, "%s", path);
  if (face > 0 && strlen(path) + 12 < sizeof id) snprintf(id, sizeof id, "%s#%d", path, face);
  if (already_adopted(r, id)) return false;
  SDL_PropertiesID props = SDL_CreateProperties();
  if (!props) return false;
  SDL_SetStringProperty(props, TTF_PROP_FONT_CREATE_FILENAME_STRING, path);
  SDL_SetFloatProperty(props, TTF_PROP_FONT_CREATE_SIZE_FLOAT, 16.0f);
  SDL_SetNumberProperty(props, TTF_PROP_FONT_CREATE_FACE_NUMBER, face);
  TTF_Font *fb = TTF_OpenFontWithProperties(props);
  SDL_DestroyProperties(props);
  if (!fb) return false;
  if (!TTF_FontHasGlyph(fb, cp)) {
    TTF_CloseFont(fb);
    return false;
  }
  adopt_fallback(r, fb, id);
  CCORD_LOG_D("render: font fallback for U+%04X: %s", (unsigned)cp, id);
  return true;
}

#if CCORD_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>

// Asks fontconfig for fonts covering `cp`, nearest to Noto Sans first, and keeps the first one that opens and has it.
static bool lookup_system_font(ccord_renderer *r, uint32_t cp)
{
  static FcConfig *cfg;
  static bool tried;
  if (!tried) {
    tried = true;
    cfg = FcInitLoadConfigAndFonts();
  }
  if (!cfg) return false;
  FcPattern *pat = FcPatternCreate();
  FcCharSet *cs = FcCharSetCreate();
  if (!pat || !cs) {
    if (pat) FcPatternDestroy(pat);
    if (cs) FcCharSetDestroy(cs);
    return false;
  }
  FcCharSetAddChar(cs, cp);
  FcPatternAddCharSet(pat, FC_CHARSET, cs);
  FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"Noto Sans");
  FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
  FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
  FcConfigSubstitute(cfg, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult res;
  FcFontSet *fs = FcFontSort(cfg, pat, FcFalse, nullptr, &res);
  bool ok = false;
  for (int i = 0; fs && i < fs->nfont && !ok && i < 64; i++) {
    FcCharSet *have = nullptr;
    FcChar8 *file = nullptr;
    int idx = 0;
    if (FcPatternGetCharSet(fs->fonts[i], FC_CHARSET, 0, &have) != FcResultMatch || !FcCharSetHasChar(have, cp)) continue;
    if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) != FcResultMatch) continue;
    (void)FcPatternGetInteger(fs->fonts[i], FC_INDEX, 0, &idx);
    ok = try_system_font(r, (const char *)file, idx & 0xFFFF, cp);
  }
  if (fs) FcFontSetDestroy(fs);
  FcCharSetDestroy(cs);
  FcPatternDestroy(pat);
  return ok;
}
#else
// Without fontconfig: scan the standard font directories once and probe the files, plain Noto Sans-like ones first.
static char **g_font_files;
static int g_nfont_files;
static bool g_font_scanned;

static int font_file_rank(const char *path)
{
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  int rank = 0;
  if (strstr(base, "Noto")) rank -= 2;
  if (strstr(base, "Sans")) rank -= 2;
  if (strstr(base, "DejaVu")) rank -= 1;
  static const char *const k_dull[] = {"Bold", "Italic", "Oblique", "Mono", "Serif", "Black", "Light", "Thin", "Condensed", "Display", "UI"};
  for (size_t i = 0; i < sizeof k_dull / sizeof k_dull[0]; i++) {
    if (strstr(base, k_dull[i])) rank += 3;
  }
  return rank;
}

static int font_file_cmp(const void *a, const void *b)
{
  const char *pa = *(char *const *)a, *pb = *(char *const *)b;
  int ra = font_file_rank(pa), rb = font_file_rank(pb);
  return ra != rb ? ra - rb : strcmp(pa, pb);
}

static void scan_font_dir(const char *dir, int depth)
{
  ccord_dir *d = ccord_dir_open(dir);
  if (!d) return;
  char name[512];
  bool is_dir = false;
  while (ccord_dir_next(d, name, sizeof name, &is_dir) && g_nfont_files < 8192) {
    if (name[0] == '.') continue;
    char full[1024];
    if (snprintf(full, sizeof full, "%s/%s", dir, name) >= (int)sizeof full) continue;
    if (is_dir) {
      if (depth < 6) scan_font_dir(full, depth + 1);
    } else if (font_file_ext(name)) {
      char **grown = ccord_realloc(g_font_files, (size_t)(g_nfont_files + 1) * sizeof *g_font_files);
      char *copy = grown ? ccord_strndup(full, strlen(full)) : nullptr;
      if (!copy) break;
      g_font_files = grown;
      g_font_files[g_nfont_files++] = copy;
    }
  }
  ccord_dir_close(d);
}

static bool lookup_system_font(ccord_renderer *r, uint32_t cp)
{
  if (!g_font_scanned) {
    g_font_scanned = true;
    char user[1024];
#if defined(_WIN32)
    const char *windir = getenv("WINDIR");
    if (windir && windir[0] && snprintf(user, sizeof user, "%s/Fonts", windir) < (int)sizeof user) scan_font_dir(user, 0);
    else scan_font_dir("C:/Windows/Fonts", 0);
    const char *la = getenv("LOCALAPPDATA");
    if (la && la[0] && snprintf(user, sizeof user, "%s/Microsoft/Windows/Fonts", la) < (int)sizeof user) scan_font_dir(user, 0);
#elif defined(__APPLE__)
    scan_font_dir("/System/Library/Fonts", 0);
    scan_font_dir("/Library/Fonts", 0);
    const char *home = getenv("HOME");
    if (home && home[0] && snprintf(user, sizeof user, "%s/Library/Fonts", home) < (int)sizeof user) scan_font_dir(user, 0);
#elif defined(__ANDROID__)
    (void)user;
    scan_font_dir("/system/fonts", 0);
    scan_font_dir("/product/fonts", 0);
    scan_font_dir("/system_ext/fonts", 0);
#else
    scan_font_dir("/usr/share/fonts", 0);
    scan_font_dir("/usr/local/share/fonts", 0);
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] && snprintf(user, sizeof user, "%s/fonts", xdg) < (int)sizeof user) scan_font_dir(user, 0);
#endif
    if (g_nfont_files > 1) qsort(g_font_files, (size_t)g_nfont_files, sizeof *g_font_files, font_file_cmp);
  }
  for (int i = 0; i < g_nfont_files; i++) {
    if (try_system_font(r, g_font_files[i], 0, cp)) return true;
  }
  return false;
}
#endif

static bool miss_known(const ccord_renderer *r, uint32_t cp)
{
  for (int i = 0; i < r->nmiss; i++) {
    if (r->miss_cp[i] == cp) return true;
  }
  return false;
}

static void miss_add(ccord_renderer *r, uint32_t cp)
{
  r->miss_cp[r->miss_next] = cp;
  r->miss_next = (r->miss_next + 1) % CCORD_MISS_CACHE_SIZE;
  if (r->nmiss < CCORD_MISS_CACHE_SIZE) r->nmiss++;
}

// Before `s` is shaped: for each code point neither the base face nor an attached fallback has, look for an installed
// font that does and attach it. Nothing is opened for text the bundled fonts already cover.
static void ensure_coverage(ccord_renderer *r, TTF_Font *font, const char *s, size_t len)
{
  int budget = 16;   // distinct lookups per text; a long run of unknown characters must not stall one frame
  size_t i = 0;
  while (i < len && budget > 0) {
    uint8_t b = (uint8_t)s[i];
    if (b < 0x80) {
      i++;
      continue;
    }
    char32_t cp = 0;
    int n = ccord_utf8_decode(s + i, len - i, &cp);
    if (n <= 0) {
      i++;
      continue;
    }
    i += (size_t)n;
    if (cp_default_ignorable(cp) || cp_hidden(cp) || TTF_FontHasGlyph(font, cp) || miss_known(r, cp)) continue;
    // Lazily load bundled emoji/CJK subsets on the first code point NotoSans doesn't cover so cold start
    // draws its first frame immediately without parsing ~16 MB of fallback font tables upfront.
    if (!r->fallbacks_probed) {
      fallback_fonts_init(r);
      if (TTF_FontHasGlyph(font, cp)) continue;
    }
    if (r->glyph_fixes_off) continue;
    budget--;
    if (r->nfallbacks >= CCORD_MAX_FALLBACKS || !lookup_system_font(r, cp)) miss_add(r, cp);
  }
}

static int font_px(const ccord_renderer *r, const ccord_text_style *st)
{
  int px = (int)lroundf(st->size * r->scale);
  return px < 4 ? 4 : (px > 512 ? 512 : px);
}

static TTF_Font *font_get(ccord_renderer *r, ccord_font_style style, int px)
{
  for (int i = 0; i < r->nfonts; i++) {
    if (r->fonts[i].style == style && r->fonts[i].px == px) return r->fonts[i].font;
  }
  if (r->nfonts == CCORD_FONT_CACHE_SIZE) {
    CCORD_LOG_W("render: font cache full, flushing");
    text_cache_flush(r);
  }
  TTF_Font *f = TTF_CopyFont(r->base[style == CCORD_FONT_BOLD ? 1 : 0]);
  if (!f) {
    CCORD_LOG_E("render: cannot copy font: %s", SDL_GetError());
    return nullptr;
  }
  TTF_SetFontSize(f, (float)px);
  TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
  int skip = (int)lroundf((float)px * r->line_factor);
  TTF_SetFontLineSkip(f, skip);
  font_entry fe = {.font = f, .style = style, .px = px};
  const int nfb = r->glyph_fixes_off ? r->nbundled : r->nfallbacks;
  for (int k = 0; k < nfb && fe.nfallbacks < CCORD_MAX_FALLBACKS; k++) {
    TTF_Font *fb = TTF_CopyFont(r->fallback_base[k]);
    if (!fb) continue;
    TTF_SetFontSize(fb, (float)px);
    TTF_SetFontHinting(fb, TTF_HINTING_LIGHT);
    if (TTF_AddFallbackFont(f, fb)) {
      fe.fallbacks[fe.nfallbacks++] = fb;
    } else {
      TTF_CloseFont(fb);
    }
  }
  r->fonts[r->nfonts++] = fe;
  return f;
}

// ---- text cache: fixed-size LRU, hash buckets chained through `next` ----

void text_cache_init(ccord_renderer *r)
{
  for (int i = 0; i < CCORD_TEXT_CACHE_SIZE; i++) r->buckets[i] = -1;
  r->ntexts = 0;
}

static void entry_free(text_entry *e)
{
  if (e->text) TTF_DestroyText(e->text);
  ccord_free(e->str);
  ccord_free(e->shaped);
  ccord_free(e->o2s);
  ccord_free(e->s2o);
  memset(e, 0, sizeof *e);
}

// Drops every cached text and keeps the fonts.
static void text_entries_flush(ccord_renderer *r)
{
  for (int i = 0; i < r->ntexts; i++) entry_free(&r->texts[i]);
  r->ntexts = 0;
  for (int i = 0; i < CCORD_TEXT_CACHE_SIZE; i++) r->buckets[i] = -1;
}

void text_cache_flush(ccord_renderer *r)
{
  text_entries_flush(r);
  for (int i = 0; i < r->nfonts; i++) {
    if (r->fonts[i].font) {
      TTF_ClearFallbackFonts(r->fonts[i].font);
      TTF_CloseFont(r->fonts[i].font);
    }
    for (int k = 0; k < r->fonts[i].nfallbacks; k++) {
      if (r->fonts[i].fallbacks[k]) TTF_CloseFont(r->fonts[i].fallbacks[k]);
    }
    memset(&r->fonts[i], 0, sizeof r->fonts[i]);
  }
  r->nfonts = 0;
}

static uint32_t hash_key(const TTF_Font *font, int wrap_px, const char *s, size_t len)
{
  uint32_t h = 2166136261u;
  uintptr_t fp = (uintptr_t)font;
  for (size_t i = 0; i < sizeof fp; i++) {
    h ^= (uint32_t)((fp >> (8 * i)) & 0xffu);
    h *= 16777619u;
  }
  h ^= (uint32_t)wrap_px;
  h *= 16777619u;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

static void bucket_unlink(ccord_renderer *r, int idx)
{
  int *link = &r->buckets[r->texts[idx].hash % CCORD_TEXT_CACHE_SIZE];
  while (*link != -1 && *link != idx) link = &r->texts[*link].next;
  if (*link == idx) *link = r->texts[idx].next;
}

static void normalize_bitmap_glyphs(TTF_Text *t, TTF_Font *font, int wrap_px, int *io_w, int *io_h)
{
  if (!t || !t->internal || !font) return;
  int fh = TTF_GetFontHeight(font);
  int skip = TTF_GetFontLineSkip(font);
  if (fh <= 0) return;
  if (skip <= 0) skip = fh;
  TTF_TextData *d = t->internal;
  int oversized = 0;
  for (int i = 0; i < d->num_ops; i++) {
    if (d->ops[i].cmd == TTF_DRAW_COMMAND_COPY && d->ops[i].copy.dst.h > fh + 2) {
      oversized++;
    }
  }
  if (oversized == 0) return;
  if (wrap_px > 0 && t->num_lines > 1) {
    TTF_SetTextWrapWidth(t, wrap_px + oversized * 120);
    TTF_GetTextSize(t, io_w, io_h);
    d = t->internal;
    if (!d) return;
  }
  if (d->engine && d->engine_text && d->engine->DestroyText) {
    d->engine->DestroyText(d->engine->userdata, t);
    d->engine_text = nullptr;
  }
  int cur_line = -1;
  int shift_x = 0;
  int max_x = 0;
  int max_line = 0;
  for (int i = 0; i < d->num_ops; i++) {
    TTF_DrawOperation *op = &d->ops[i];
    if (op->cmd != TTF_DRAW_COMMAND_COPY) continue;
    int line = op->copy.dst.y / skip;
    if (line < 0) line = 0;
    if (line != cur_line) {
      cur_line = line;
      shift_x = 0;
    }
    op->copy.dst.x -= shift_x;
    if (op->copy.dst.x < 0) op->copy.dst.x = 0;
    if (op->copy.dst.h > fh + 2) {
      int old_w = op->copy.dst.w;
      int new_h = fh;
      int new_w = op->copy.src.h > 0 ? (op->copy.src.w * new_h + op->copy.src.h / 2) / op->copy.src.h : new_h;
      op->copy.dst.w = new_w;
      op->copy.dst.h = new_h;
      op->copy.dst.y = line * skip + (skip - new_h) / 2;
      shift_x += (old_w - new_w);
    }
    if (op->copy.dst.x + op->copy.dst.w > max_x) max_x = op->copy.dst.x + op->copy.dst.w;
    if (line > max_line) max_line = line;
  }
  cur_line = -1;
  shift_x = 0;
  for (int i = 0; i < d->num_clusters; i++) {
    TTF_SubString *cl = &d->clusters[i];
    if (cl->line_index != cur_line) {
      cur_line = cl->line_index;
      shift_x = 0;
    }
    cl->rect.x -= shift_x;
    if (cl->rect.x < 0) cl->rect.x = 0;
    if (cl->rect.w > fh * 2) {
      int old_w = cl->rect.w;
      cl->rect.w = fh;
      shift_x += (old_w - fh);
    }
    if (cl->rect.x + cl->rect.w > max_x) max_x = cl->rect.x + cl->rect.w;
    if (cl->line_index > max_line) max_line = cl->line_index;
  }
  int lines = t->num_lines > max_line + 1 ? t->num_lines : max_line + 1;
  d->w = max_x;
  d->h = lines <= 1 ? fh : (lines - 1) * skip + fh;
  d->needs_engine_update = true;
  *io_w = d->w;
  *io_h = d->h;
}

static text_entry *text_get(ccord_renderer *r, const char *s, size_t len, const ccord_text_style *st, float wrap_width)
{
  if (!s) {
    s = "";
    len = 0;
  }
  if (len > MAX_TEXT_BYTES) {
    len = MAX_TEXT_BYTES;
    while (len > 0 && !ccord_utf8_valid(s, len)) len--;   // never split a code point
  }
  int px = font_px(r, st);
  TTF_Font *font = font_get(r, st->font, px);
  if (!font) return nullptr;
  int wrap_px = wrap_width > 0.0f ? (int)lroundf(wrap_width * r->scale) : 0;
  if (wrap_width > 0.0f && wrap_px < 1) wrap_px = 1;
  uint32_t h = hash_key(font, wrap_px, s, len);
  r->tick++;
  for (int i = r->buckets[h % CCORD_TEXT_CACHE_SIZE]; i != -1; i = r->texts[i].next) {
    text_entry *e = &r->texts[i];
    if (e->hash == h && e->font == font && e->wrap_px == wrap_px && e->len == len && memcmp(e->str, s, len) == 0) {
      e->tick = r->tick;
      return e;
    }
  }
  r->cache_misses++;
  char *copy = ccord_strndup(s, len);
  if (!copy) return nullptr;
  char *shaped = nullptr;
  size_t slen = len;
  uint32_t *o2s = nullptr, *s2o = nullptr;
  if (!r->glyph_fixes_off) (void)sanitize_text(copy, len, &shaped, &slen, &o2s, &s2o);   // shaped stays null if nothing to cut
  const char *src = shaped ? shaped : copy;
  ensure_coverage(r, font, src, slen);
  TTF_Text *t = TTF_CreateText(r->engine, font, src, slen);
  if (!t) {
    CCORD_LOG_W("render: TTF_CreateText failed: %s", SDL_GetError());
    ccord_free(copy);
    ccord_free(shaped);
    ccord_free(o2s);
    ccord_free(s2o);
    return nullptr;
  }
  if (wrap_px > 0) TTF_SetTextWrapWidth(t, wrap_px);
  int slot;
  if (r->ntexts < CCORD_TEXT_CACHE_SIZE) {
    slot = r->ntexts++;
  } else {
    slot = 0;   // evict the least recently used entry
    for (int i = 1; i < r->ntexts; i++) {
      if (r->texts[i].tick < r->texts[slot].tick) slot = i;
    }
    bucket_unlink(r, slot);
    entry_free(&r->texts[slot]);
  }
  text_entry *e = &r->texts[slot];
  *e = (text_entry){.text = t, .font = font, .str = copy, .len = len, .shaped = shaped, .slen = slen, .o2s = o2s, .s2o = s2o,
                    .wrap_px = wrap_px, .hash = h, .tick = r->tick};
  TTF_GetTextSize(t, &e->w, &e->h);
  normalize_bitmap_glyphs(t, font, wrap_px, &e->w, &e->h);
  int fh = TTF_GetFontHeight(font);
  if (slen == 0) {
    e->h = fh;
  } else if (wrap_px == 0 && memchr(copy, '\n', len) == nullptr && e->h > fh * 2) {
    e->h = fh;
  }
  int *head = &r->buckets[h % CCORD_TEXT_CACHE_SIZE];
  e->next = *head;
  *head = slot;
  return e;
}

uint64_t ccord_text_cache_misses(const ccord_renderer *r)
{
  return r ? r->cache_misses : 0;
}

int ccord_render_fallback_font_count(const ccord_renderer *r)
{
  if (!r) return 0;
  union { const ccord_renderer *c; ccord_renderer *v; } u = {.c = r};
  fallback_fonts_init(u.v);
  return r->nfallbacks;
}

const char *ccord_render_fallback_font_family(const ccord_renderer *r, int index)
{
  if (!r) return nullptr;
  union { const ccord_renderer *c; ccord_renderer *v; } u = {.c = r};
  fallback_fonts_init(u.v);
  if (index < 0 || index >= r->nfallbacks) return nullptr;
  return TTF_GetFontFamilyName(r->fallback_base[index]);
}

// ---- glyph probe (test hooks) ----

void ccord_render_set_glyph_fixes(ccord_renderer *r, bool enabled)
{
  if (!r || r->glyph_fixes_off == !enabled) return;
  r->glyph_fixes_off = !enabled;
  text_cache_flush(r);   // sized fonts and shaped text were made under the other setting
}

int ccord_render_system_font_count(const ccord_renderer *r)
{
  return r ? r->nfallbacks - r->nbundled : 0;
}

static int text_draw_ops(const text_entry *e)
{
  return (e && e->text && e->text->internal) ? e->text->internal->num_ops : 0;
}

ccord_glyph_kind ccord_text_glyph_kind(ccord_renderer *r, const ccord_text_style *st, uint32_t cp)
{
  char s[16] = "a";
  size_t n = ccord_utf8_encode(cp, s + 1);
  if (n == 0) return CCORD_GLYPH_NOTDEF;
  s[1 + n] = 'a';
  text_entry *e = text_get(r, s, n + 2, st, 0.0f);
  if (!e) return CCORD_GLYPH_NOTDEF;
  const int w = e->w, ops = text_draw_ops(e);
  TTF_Font *font = e->font;
  const bool shaped = e->shaped != nullptr;
  text_entry *base = text_get(r, "aa", 2, st, 0.0f);
  if (!base) return CCORD_GLYPH_NOTDEF;
  if (w == base->w && ops <= text_draw_ops(base)) return CCORD_GLYPH_NONE;   // no width and no ink
  return (shaped || TTF_FontHasGlyph(font, cp)) ? CCORD_GLYPH_REAL : CCORD_GLYPH_NOTDEF;
}

// ---- public API ----

ccord_vec2 ccord_text_measure(ccord_renderer *r, const char *utf8, size_t len, const ccord_text_style *st, float wrap_width)
{
  text_entry *e = text_get(r, utf8, len, st, wrap_width);
  if (!e) return (ccord_vec2){0.0f, st->size * r->line_factor};
  return (ccord_vec2){(float)e->w / r->scale, (float)e->h / r->scale};
}

void ccord_text_draw(ccord_renderer *r, const char *utf8, size_t len, float x, float y, const ccord_text_style *st, float wrap_width)
{
  if (!utf8 || len == 0 || st->color.a == 0) return;
  text_entry *e = text_get(r, utf8, len, st, wrap_width);
  if (!e) return;
  uint8_t a = ccord_fx_alpha(r, st->color.a);
  if (a == 0) return;
  TTF_SetTextColor(e->text, st->color.r, st->color.g, st->color.b, a);
  TTF_DrawRendererText(e->text, roundf((x + r->ox) * r->scale), roundf((y + r->oy) * r->scale));
}

float ccord_text_draw_ellipsis(ccord_renderer *r, const char *utf8, size_t len, float x, float y, float max_w, const ccord_text_style *st)
{
  if (!utf8 || len == 0 || max_w <= 0.0f) return 0.0f;
  ccord_vec2 full = ccord_text_measure(r, utf8, len, st, 0.0f);
  if (full.x <= max_w) {
    ccord_text_draw(r, utf8, len, x, y, st, 0.0f);
    return full.x;
  }
  static const char dots[] = "\xE2\x80\xA6";   // U+2026
  ccord_vec2 dw = ccord_text_measure(r, dots, 3, st, 0.0f);
  TTF_Font *font = font_get(r, st->font, font_px(r, st));
  if (!font) return 0.0f;
  int avail = (int)floorf((max_w - dw.x) * r->scale);
  size_t fit = 0;
  if (avail > 0) {
    int mw = 0;
    if (!TTF_MeasureString(font, utf8, len, avail, &mw, &fit)) fit = 0;
  }
  while (fit > 0 && fit < len && !ccord_utf8_valid(utf8, fit)) fit--;
  char buf[320];
  if (fit > sizeof buf - 4) fit = sizeof buf - 4;
  while (fit > 0 && !ccord_utf8_valid(utf8, fit)) fit--;
  // Trailing spaces before the ellipsis look sloppy.
  while (fit > 0 && utf8[fit - 1] == ' ') fit--;
  memcpy(buf, utf8, fit);
  memcpy(buf + fit, dots, 3);
  size_t total = fit + 3;
  ccord_text_draw(r, buf, total, x, y, st, 0.0f);
  return ccord_text_measure(r, buf, total, st, 0.0f).x;
}

// SDL3_ttf collapses trailing whitespace on a line into a single zero-width TTF_SubString; measure the prefix
// delta on that line so caret and hit-test advance across trailing spaces instead of sticking before them.
static int trailing_ws_advance(const text_entry *e, const char *su, const TTF_SubString *sub, size_t target_s)
{
  if (!e || !e->font || !su || !sub || sub->offset < 0) return 0;
  size_t soff = (size_t)sub->offset;
  if (target_s <= soff || target_s > e->slen) return 0;
  TTF_SubString ls;
  size_t lstart = soff;
  if (TTF_GetTextSubStringForLine(e->text, sub->line_index, &ls) && ls.offset >= 0 && (size_t)ls.offset <= soff) {
    lstart = (size_t)ls.offset;
  }
  int w0 = 0, w1 = 0;
  if (soff > lstart) (void)TTF_GetStringSize(e->font, su + lstart, soff - lstart, &w0, nullptr);
  (void)TTF_GetStringSize(e->font, su + lstart, target_s - lstart, &w1, nullptr);
  return w1 > w0 ? w1 - w0 : 0;
}

size_t ccord_text_hit_test(ccord_renderer *r, const char *utf8, size_t len, const ccord_text_style *st, float wrap_width, float px, float py)
{
  if (!utf8 || len == 0) return 0;
  text_entry *e = text_get(r, utf8, len, st, wrap_width);
  if (!e) return len;
  if (e->len < len) len = e->len;
  TTF_SubString sub;
  int ix = (int)lroundf(px * r->scale), iy = (int)lroundf(py * r->scale);
  if (!TTF_GetTextSubStringForPoint(e->text, ix, iy, &sub)) return len;
  size_t soff = (size_t)sub.offset;
  if (soff >= e->slen) return len;
  const char *su = e->shaped ? e->shaped : e->str;
  size_t off = e->s2o ? e->s2o[soff] : soff;
  if (off >= len) return len;
  if (su[soff] == '\n' || utf8[off] == '\n') return off;   // right of the last glyph of a line
  if (sub.rect.w == 0 && sub.length > 0) {
    size_t send = soff + (size_t)sub.length;
    if (send > e->slen) send = e->slen;
    size_t end_ws = soff;
    while (end_ws < send && su[end_ws] != '\n') end_ws++;
    size_t best = soff;
    int prev_x = sub.rect.x;
    size_t cur = soff;
    while (cur < end_ws) {
      size_t next = ccord_utf8_next(su, end_ws, cur);
      int next_x = sub.rect.x + trailing_ws_advance(e, su, &sub, next);
      if (ix <= (prev_x + next_x) / 2) break;
      best = next;
      prev_x = next_x;
      cur = next;
    }
    off = e->s2o ? e->s2o[best] : best;
    return off > len ? len : off;
  }
  if (ix > sub.rect.x + sub.rect.w / 2) {
    size_t send = soff + (size_t)sub.length;
    if (send > e->slen) send = e->slen;
    off = e->s2o ? e->s2o[send] : send;
  }
  return off > len ? len : off;
}

ccord_rect ccord_text_caret_rect(ccord_renderer *r, const char *utf8, size_t len, const ccord_text_style *st, float wrap_width, size_t pos)
{
  float lh = st->size * r->line_factor;
  if (!utf8 || len == 0 || pos == 0) {
    text_entry *e0 = (utf8 && len) ? text_get(r, utf8, len, st, wrap_width) : nullptr;
    float h0 = e0 ? (float)TTF_GetFontHeight(e0->font) / r->scale : lh;
    return (ccord_rect){0.0f, 0.0f, 1.0f, h0};
  }
  text_entry *e = text_get(r, utf8, len, st, wrap_width);
  if (!e) return (ccord_rect){0.0f, 0.0f, 1.0f, lh};
  float fh = (float)TTF_GetFontHeight(e->font) / r->scale;
  if (pos > e->len) pos = e->len;
  // Offsets into the text that was shaped (controls cut out); identical to `pos` when nothing was cut.
  const char *su = e->shaped ? e->shaped : e->str;
  const size_t sl = e->slen;
  pos = e->o2s ? e->o2s[pos] : pos;
  if (sl == 0) return (ccord_rect){0.0f, 0.0f, 1.0f, fh};
  if (pos == sl && su[pos - 1] == '\n') {   // caret after a trailing newline sits at the start of the next line
    TTF_SubString nl;
    if (TTF_GetTextSubString(e->text, (int)pos - 1, &nl)) {
      return (ccord_rect){0.0f, ((float)nl.rect.y + (float)TTF_GetFontLineSkip(e->font)) / r->scale, 1.0f, fh};
    }
  }
  TTF_SubString sub;
  if (pos == sl) {
    // End of text: right edge of the last glyph.
    if (TTF_GetTextSubString(e->text, (int)pos - 1, &sub)) {
      int x_px = sub.rect.x + sub.rect.w;
      if (sub.rect.w == 0) x_px = sub.rect.x + trailing_ws_advance(e, su, &sub, pos);
      return (ccord_rect){(float)x_px / r->scale, (float)sub.rect.y / r->scale, 1.0f, fh};
    }
  } else if (TTF_GetTextSubString(e->text, (int)pos, &sub)) {
    int x_px = sub.rect.x;
    if (sub.rect.w == 0 && (size_t)sub.offset < pos) {
      x_px = sub.rect.x + trailing_ws_advance(e, su, &sub, pos);
    }
    return (ccord_rect){(float)x_px / r->scale, (float)sub.rect.y / r->scale, 1.0f, fh};
  }
  return (ccord_rect){(float)e->w / r->scale, 0.0f, 1.0f, fh};
}
