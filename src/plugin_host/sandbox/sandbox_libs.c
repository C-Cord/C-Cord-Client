// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "sandbox_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/emoji/emoji.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// ---------------------------------------------------------------- color.* (Class 1 Safe)

static int hex_nib(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool parse_rgba(lua_State *L, int idx, int *r, int *g, int *b, int *a)
{
  *a = 255;
  if (lua_type(L, idx) == LUA_TSTRING) {
    size_t n = 0;
    const char *s = lua_tolstring(L, idx, &n);
    if (!s || n == 0) return false;
    if (s[0] == '#') {
      if (n == 4 || n == 5) {
        int r0 = hex_nib(s[1]), g0 = hex_nib(s[2]), b0 = hex_nib(s[3]);
        int a0 = n == 5 ? hex_nib(s[4]) : 15;
        if (r0 < 0 || g0 < 0 || b0 < 0 || a0 < 0) return false;
        *r = r0 * 17; *g = g0 * 17; *b = b0 * 17; *a = a0 * 17;
        return true;
      }
      if (n == 7 || n == 9) {
        int v[4] = {0, 0, 0, 255};
        size_t cnt = (n - 1u) / 2u;
        for (size_t i = 0; i < cnt; i++) {
          int hi = hex_nib(s[1 + 2 * i]), lo = hex_nib(s[2 + 2 * i]);
          if (hi < 0 || lo < 0) return false;
          v[i] = hi * 16 + lo;
        }
        *r = v[0]; *g = v[1]; *b = v[2]; *a = v[3];
        return true;
      }
      return false;
    }
    int rr = 0, gg = 0, bb = 0;
    double aa = 1.0;
    if (sscanf(s, "rgba(%d,%d,%d,%lf)", &rr, &gg, &bb, &aa) == 4 ||
        sscanf(s, "rgba( %d , %d , %d , %lf )", &rr, &gg, &bb, &aa) == 4) {
      *r = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
      *g = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
      *b = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
      double av = aa <= 1.0 ? aa * 255.0 : aa;
      int ai = (int)llround(av);
      *a = ai < 0 ? 0 : (ai > 255 ? 255 : ai);
      return true;
    }
    if (sscanf(s, "rgb(%d,%d,%d)", &rr, &gg, &bb) == 3 ||
        sscanf(s, "rgb( %d , %d , %d )", &rr, &gg, &bb) == 3) {
      *r = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
      *g = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
      *b = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
      return true;
    }
    return false;
  }
  if (lua_type(L, idx) == LUA_TTABLE) {
    sb_raw_getfield(L, idx, "r"); int rr = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    sb_raw_getfield(L, idx, "g"); int gg = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    sb_raw_getfield(L, idx, "b"); int bb = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    sb_raw_getfield(L, idx, "a"); int aa = lua_isnil(L, -1) ? 255 : (int)lua_tointeger(L, -1); lua_pop(L, 1);
    *r = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
    *g = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
    *b = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
    *a = aa < 0 ? 0 : (aa > 255 ? 255 : aa);
    return true;
  }
  return false;
}

static void push_rgba(lua_State *L, int r, int g, int b, int a)
{
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, r); lua_setfield(L, -2, "r");
  lua_pushinteger(L, g); lua_setfield(L, -2, "g");
  lua_pushinteger(L, b); lua_setfield(L, -2, "b");
  lua_pushinteger(L, a); lua_setfield(L, -2, "a");
}

static void rgb_to_hsl(int r, int g, int b, double *h, double *s, double *l)
{
  double rd = (double)r / 255.0, gd = (double)g / 255.0, bd = (double)b / 255.0;
  double mx = fmax(rd, fmax(gd, bd));
  double mn = fmin(rd, fmin(gd, bd));
  double d = mx - mn;
  *l = (mx + mn) * 0.5;
  if (d < 1e-9) {
    *h = 0.0; *s = 0.0;
    return;
  }
  *s = *l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
  if (mx == rd) *h = fmod((gd - bd) / d + (gd < bd ? 6.0 : 0.0), 6.0) * 60.0;
  else if (mx == gd) *h = ((bd - rd) / d + 2.0) * 60.0;
  else *h = ((rd - gd) / d + 4.0) * 60.0;
}

static double hue2rgb(double p, double q, double t)
{
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

static void hsl_to_rgb(double h, double s, double l, int *r, int *g, int *b)
{
  h = fmod(h, 360.0);
  if (h < 0.0) h += 360.0;
  s = fmax(0.0, fmin(1.0, s));
  l = fmax(0.0, fmin(1.0, l));
  double rd, gd, bd;
  if (s < 1e-9) {
    rd = gd = bd = l;
  } else {
    double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
    double p = 2.0 * l - q;
    double hn = h / 360.0;
    rd = hue2rgb(p, q, hn + 1.0 / 3.0);
    gd = hue2rgb(p, q, hn);
    bd = hue2rgb(p, q, hn - 1.0 / 3.0);
  }
  *r = (int)llround(rd * 255.0);
  *g = (int)llround(gd * 255.0);
  *b = (int)llround(bd * 255.0);
}

static double srgb_lin(int c)
{
  double v = (double)c / 255.0;
  return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double rel_luminance(int r, int g, int b)
{
  return 0.2126 * srgb_lin(r) + 0.7152 * srgb_lin(g) + 0.0722 * srgb_lin(b);
}

static int api_color_parse(lua_State *L)
{
  int r, g, b, a;
  if (!parse_rgba(L, 1, &r, &g, &b, &a)) { lua_pushnil(L); return 1; }
  push_rgba(L, r, g, b, a);
  return 1;
}

static int api_color_to_hex(lua_State *L)
{
  int r, g, b, a;
  if (!parse_rgba(L, 1, &r, &g, &b, &a)) return luaL_error(L, "color.to_hex: invalid color");
  bool inc_alpha = lua_toboolean(L, 2) != 0 || a < 255;
  char buf[16];
  if (inc_alpha) snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
  else snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
  lua_pushstring(L, buf);
  return 1;
}

static int api_color_to_hsl(lua_State *L)
{
  int r, g, b, a;
  if (!parse_rgba(L, 1, &r, &g, &b, &a)) return luaL_error(L, "color.to_hsl: invalid color");
  double h, s, l;
  rgb_to_hsl(r, g, b, &h, &s, &l);
  lua_createtable(L, 0, 4);
  lua_pushnumber(L, h); lua_setfield(L, -2, "h");
  lua_pushnumber(L, s); lua_setfield(L, -2, "s");
  lua_pushnumber(L, l); lua_setfield(L, -2, "l");
  lua_pushinteger(L, a); lua_setfield(L, -2, "a");
  return 1;
}

static int api_color_from_hsl(lua_State *L)
{
  double h = (double)luaL_checknumber(L, 1);
  double s = (double)luaL_checknumber(L, 2);
  double l = (double)luaL_checknumber(L, 3);
  int a = (int)luaL_optinteger(L, 4, 255);
  a = a < 0 ? 0 : (a > 255 ? 255 : a);
  int r, g, b;
  hsl_to_rgb(h, s, l, &r, &g, &b);
  char buf[16];
  if (a < 255) snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
  else snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
  lua_pushstring(L, buf);
  return 1;
}

static int color_shift_l(lua_State *L, double sign)
{
  int r, g, b, a;
  if (!parse_rgba(L, 1, &r, &g, &b, &a)) return luaL_error(L, "color: invalid color");
  double amt = (double)luaL_checknumber(L, 2);
  double h, s, l;
  rgb_to_hsl(r, g, b, &h, &s, &l);
  l = fmax(0.0, fmin(1.0, l + sign * amt));
  hsl_to_rgb(h, s, l, &r, &g, &b);
  char buf[16];
  if (a < 255) snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
  else snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
  lua_pushstring(L, buf);
  return 1;
}

static int api_color_lighten(lua_State *L) { return color_shift_l(L, 1.0); }
static int api_color_darken(lua_State *L) { return color_shift_l(L, -1.0); }

static int api_color_mix(lua_State *L)
{
  int r1, g1, b1, a1, r2, g2, b2, a2;
  if (!parse_rgba(L, 1, &r1, &g1, &b1, &a1) || !parse_rgba(L, 2, &r2, &g2, &b2, &a2)) {
    return luaL_error(L, "color.mix: invalid color");
  }
  double t = fmax(0.0, fmin(1.0, (double)luaL_optnumber(L, 3, 0.5)));
  int r = (int)llround((double)r1 * (1.0 - t) + (double)r2 * t);
  int g = (int)llround((double)g1 * (1.0 - t) + (double)g2 * t);
  int b = (int)llround((double)b1 * (1.0 - t) + (double)b2 * t);
  int a = (int)llround((double)a1 * (1.0 - t) + (double)a2 * t);
  char buf[16];
  if (a < 255) snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
  else snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
  lua_pushstring(L, buf);
  return 1;
}

static int api_color_alpha(lua_State *L)
{
  int r, g, b, a;
  if (!parse_rgba(L, 1, &r, &g, &b, &a)) return luaL_error(L, "color.alpha: invalid color");
  double av = (double)luaL_checknumber(L, 2);
  if (av <= 1.0 && av >= 0.0) av *= 255.0;
  int ai = (int)llround(av);
  ai = ai < 0 ? 0 : (ai > 255 ? 255 : ai);
  char buf[16];
  snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, ai);
  lua_pushstring(L, buf);
  return 1;
}

static int api_color_luminance(lua_State *L)
{
  int r, g, b, a;
  if (!parse_rgba(L, 1, &r, &g, &b, &a)) return luaL_error(L, "color.luminance: invalid color");
  lua_pushnumber(L, rel_luminance(r, g, b));
  return 1;
}

static int api_color_contrast(lua_State *L)
{
  int r1, g1, b1, a1, r2, g2, b2, a2;
  if (!parse_rgba(L, 1, &r1, &g1, &b1, &a1) || !parse_rgba(L, 2, &r2, &g2, &b2, &a2)) {
    return luaL_error(L, "color.contrast: invalid color");
  }
  double l1 = rel_luminance(r1, g1, b1), l2 = rel_luminance(r2, g2, b2);
  double hi = fmax(l1, l2), lo = fmin(l1, l2);
  lua_pushnumber(L, (hi + 0.05) / (lo + 0.05));
  return 1;
}

static const luaL_Reg k_color[] = {
  {"parse", api_color_parse}, {"to_hex", api_color_to_hex}, {"to_hsl", api_color_to_hsl},
  {"from_hsl", api_color_from_hsl}, {"lighten", api_color_lighten}, {"darken", api_color_darken},
  {"mix", api_color_mix}, {"alpha", api_color_alpha}, {"luminance", api_color_luminance},
  {"contrast", api_color_contrast}, {nullptr, nullptr},
};

// ---------------------------------------------------------------- compress.* (Class 1 Safe via zlib)

#define SB_COMPRESS_MAX_OUT (16u * 1024u * 1024u)

static int zlib_compress_mode(lua_State *L, int window_bits)
{
  size_t len = 0;
  const char *in = luaL_checklstring(L, 1, &len);
  int level = (int)luaL_optinteger(L, 2, Z_DEFAULT_COMPRESSION);
  if (level != Z_DEFAULT_COMPRESSION && (level < 0 || level > 9)) level = Z_DEFAULT_COMPRESSION;
  z_stream zs;
  memset(&zs, 0, sizeof zs);
  if (deflateInit2(&zs, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    lua_pushnil(L); lua_pushliteral(L, "compress init failed"); return 2;
  }
  ccord_buf out;
  ccord_buf_init(&out, SB_COMPRESS_MAX_OUT);
  zs.next_in = (Bytef *)(uintptr_t)in;
  zs.avail_in = (uInt)len;
  uint8_t chunk[8192];
  int rc = Z_OK;
  do {
    zs.next_out = chunk;
    zs.avail_out = sizeof chunk;
    rc = deflate(&zs, Z_FINISH);
    size_t got = sizeof chunk - (size_t)zs.avail_out;
    if (got > 0) {
      if (out.len + got > SB_COMPRESS_MAX_OUT || ccord_buf_append(&out, (const char *)chunk, got) != CCORD_OK) {
        deflateEnd(&zs); ccord_buf_free(&out);
        lua_pushnil(L); lua_pushliteral(L, "compress output too large"); return 2;
      }
    }
  } while (rc == Z_OK);
  deflateEnd(&zs);
  if (rc != Z_STREAM_END) {
    ccord_buf_free(&out);
    lua_pushnil(L); lua_pushliteral(L, "compress failed"); return 2;
  }
  lua_pushlstring(L, out.data ? out.data : "", out.len);
  ccord_buf_free(&out);
  return 1;
}

static int zlib_decompress_mode(lua_State *L, int window_bits)
{
  size_t len = 0;
  const char *in = luaL_checklstring(L, 1, &len);
  z_stream zs;
  memset(&zs, 0, sizeof zs);
  if (inflateInit2(&zs, window_bits) != Z_OK) {
    lua_pushnil(L); lua_pushliteral(L, "decompress init failed"); return 2;
  }
  ccord_buf out;
  ccord_buf_init(&out, SB_COMPRESS_MAX_OUT);
  zs.next_in = (Bytef *)(uintptr_t)in;
  zs.avail_in = (uInt)len;
  uint8_t chunk[8192];
  int rc = Z_OK;
  do {
    zs.next_out = chunk;
    zs.avail_out = sizeof chunk;
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc == Z_NEED_DICT || rc == Z_DATA_ERROR || rc == Z_MEM_ERROR || rc == Z_STREAM_ERROR) {
      inflateEnd(&zs); ccord_buf_free(&out);
      lua_pushnil(L); lua_pushliteral(L, "decompress invalid data"); return 2;
    }
    size_t got = sizeof chunk - (size_t)zs.avail_out;
    if (got > 0) {
      if (out.len + got > SB_COMPRESS_MAX_OUT || ccord_buf_append(&out, (const char *)chunk, got) != CCORD_OK) {
        inflateEnd(&zs); ccord_buf_free(&out);
        lua_pushnil(L); lua_pushliteral(L, "decompress output limit exceeded"); return 2;
      }
    }
  } while (rc != Z_STREAM_END && zs.avail_in > 0);
  inflateEnd(&zs);
  if (rc != Z_STREAM_END) {
    ccord_buf_free(&out);
    lua_pushnil(L); lua_pushliteral(L, "truncated compressed stream"); return 2;
  }
  lua_pushlstring(L, out.data ? out.data : "", out.len);
  ccord_buf_free(&out);
  return 1;
}

static int api_compress_deflate(lua_State *L) { return zlib_compress_mode(L, MAX_WBITS); }
static int api_compress_inflate(lua_State *L) { return zlib_decompress_mode(L, MAX_WBITS + 32); }
static int api_compress_gzip(lua_State *L) { return zlib_compress_mode(L, MAX_WBITS + 16); }
static int api_compress_gunzip(lua_State *L) { return zlib_decompress_mode(L, MAX_WBITS + 16); }

static const luaL_Reg k_compress[] = {
  {"deflate", api_compress_deflate}, {"inflate", api_compress_inflate},
  {"gzip", api_compress_gzip}, {"gunzip", api_compress_gunzip}, {nullptr, nullptr},
};

// ---------------------------------------------------------------- semver.* (Class 1 Safe)

typedef struct sv_ver {
  int major, minor, patch;
  char pre[48];
} sv_ver;

static bool sv_parse(const char *s, sv_ver *out)
{
  if (!s) return false;
  if (*s == 'v' || *s == 'V') s++;
  memset(out, 0, sizeof *out);
  char *end = nullptr;
  long maj = strtol(s, &end, 10);
  if (end == s || *end != '.' || maj < 0 || maj > 999999) return false;
  s = end + 1;
  long min = strtol(s, &end, 10);
  if (end == s || *end != '.' || min < 0 || min > 999999) return false;
  s = end + 1;
  long pat = strtol(s, &end, 10);
  if (end == s || pat < 0 || pat > 999999) return false;
  out->major = (int)maj; out->minor = (int)min; out->patch = (int)pat;
  if (*end == '-') {
    const char *p = end + 1;
    size_t n = 0;
    while (p[n] && p[n] != '+' && n + 1 < sizeof out->pre) { out->pre[n] = p[n]; n++; }
    out->pre[n] = '\0';
  } else if (*end != '\0' && *end != '+') {
    return false;
  }
  return true;
}

static int sv_cmp(const sv_ver *a, const sv_ver *b)
{
  if (a->major != b->major) return a->major < b->major ? -1 : 1;
  if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
  if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
  if (a->pre[0] && !b->pre[0]) return -1;
  if (!a->pre[0] && b->pre[0]) return 1;
  int c = strcmp(a->pre, b->pre);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static int api_semver_parse(lua_State *L)
{
  const char *s = luaL_checkstring(L, 1);
  sv_ver v;
  if (!sv_parse(s, &v)) { lua_pushnil(L); return 1; }
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, v.major); lua_setfield(L, -2, "major");
  lua_pushinteger(L, v.minor); lua_setfield(L, -2, "minor");
  lua_pushinteger(L, v.patch); lua_setfield(L, -2, "patch");
  if (v.pre[0]) { lua_pushstring(L, v.pre); lua_setfield(L, -2, "prerelease"); }
  return 1;
}

static int api_semver_compare(lua_State *L)
{
  sv_ver a, b;
  if (!sv_parse(luaL_checkstring(L, 1), &a) || !sv_parse(luaL_checkstring(L, 2), &b)) {
    return luaL_error(L, "semver.compare: invalid semver");
  }
  lua_pushinteger(L, sv_cmp(&a, &b));
  return 1;
}

static int api_semver_satisfies(lua_State *L)
{
  sv_ver v, r;
  if (!sv_parse(luaL_checkstring(L, 1), &v)) { lua_pushboolean(L, 0); return 1; }
  const char *range = luaL_checkstring(L, 2);
  while (*range == ' ') range++;
  bool ok = false;
  if (strncmp(range, ">=", 2) == 0 && sv_parse(range + 2, &r)) ok = sv_cmp(&v, &r) >= 0;
  else if (strncmp(range, "<=", 2) == 0 && sv_parse(range + 2, &r)) ok = sv_cmp(&v, &r) <= 0;
  else if (range[0] == '>' && sv_parse(range + 1, &r)) ok = sv_cmp(&v, &r) > 0;
  else if (range[0] == '<' && sv_parse(range + 1, &r)) ok = sv_cmp(&v, &r) < 0;
  else if (range[0] == '^' && sv_parse(range + 1, &r)) {
    ok = sv_cmp(&v, &r) >= 0 && v.major == r.major && (r.major > 0 || v.minor == r.minor);
  } else if (range[0] == '~' && sv_parse(range + 1, &r)) {
    ok = sv_cmp(&v, &r) >= 0 && v.major == r.major && v.minor == r.minor;
  } else if (range[0] == '=' && sv_parse(range + 1, &r)) {
    ok = sv_cmp(&v, &r) == 0;
  } else if (sv_parse(range, &r)) {
    ok = sv_cmp(&v, &r) == 0;
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

static const luaL_Reg k_semver[] = {
  {"parse", api_semver_parse}, {"compare", api_semver_compare}, {"satisfies", api_semver_satisfies}, {nullptr, nullptr},
};

// ---------------------------------------------------------------- emoji.* (Class 1 Safe via ccord_emoji)

static void push_emoji_entry(lua_State *L, const ccord_emoji_entry *e)
{
  lua_createtable(L, 0, 5);
  lua_pushstring(L, ccord_emoji_utf8(e)); lua_setfield(L, -2, "unicode");
  lua_pushstring(L, ccord_emoji_name(e)); lua_setfield(L, -2, "name");
  lua_pushstring(L, ccord_emoji_cldr_name(e)); lua_setfield(L, -2, "cldr_name");
  lua_pushstring(L, ccord_emoji_category_name(e->category)); lua_setfield(L, -2, "category");
  lua_pushboolean(L, e->has_skin_tones ? 1 : 0); lua_setfield(L, -2, "has_skin_tones");
}

static int api_emoji_lookup(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  if (n >= 2 && s[0] == ':' && s[n - 1] == ':') { s++; n -= 2; }
  const ccord_emoji_entry *e = ccord_emoji_find_shortcode(s, n);
  if (!e) { lua_pushnil(L); return 1; }
  push_emoji_entry(L, e);
  return 1;
}

static int api_emoji_to_shortcode(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  int tone = 0;
  const ccord_emoji_entry *e = ccord_emoji_find_utf8(s, n, &tone);
  if (!e) { lua_pushnil(L); return 1; }
  lua_pushstring(L, ccord_emoji_name(e));
  lua_pushinteger(L, tone);
  return 2;
}

static int api_emoji_search(lua_State *L)
{
  size_t n = 0;
  const char *q = luaL_checklstring(L, 1, &n);
  lua_Integer lim = luaL_optinteger(L, 2, 24);
  if (lim < 1) lim = 1;
  if (lim > 64) lim = 64;
  size_t idxs[64];
  size_t got = ccord_emoji_search(q, n, idxs, (size_t)lim);
  lua_createtable(L, (int)got, 0);
  for (size_t i = 0; i < got; i++) {
    const ccord_emoji_entry *e = ccord_emoji_at(idxs[i]);
    if (e) {
      push_emoji_entry(L, e);
      lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
  }
  return 1;
}

static int api_emoji_replace_shortcodes(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  if (n > SB_MAX_TEXT) return luaL_error(L, "emoji.replace_shortcodes: input too long");
  size_t cap = n * 4u + 64u;
  char *out = ccord_malloc(cap);
  if (!out) return luaL_error(L, "out of memory");
  size_t got = ccord_emoji_replace_shortcodes(s, n, out, cap);
  lua_pushlstring(L, out, got);
  ccord_free(out);
  return 1;
}

static const luaL_Reg k_emoji[] = {
  {"lookup", api_emoji_lookup}, {"to_shortcode", api_emoji_to_shortcode},
  {"search", api_emoji_search}, {"replace_shortcodes", api_emoji_replace_shortcodes}, {nullptr, nullptr},
};

// ---------------------------------------------------------------- markdown.parse (Class 1 Safe)

static void push_md_tok(lua_State *L, int arr_idx, lua_Integer *cnt, const char *type,
                        const char *text, size_t len, const char *extra_key, const char *extra_val, size_t extra_len)
{
  lua_createtable(L, 0, extra_key ? 3 : 2);
  lua_pushstring(L, type); lua_setfield(L, -2, "type");
  lua_pushlstring(L, text, len); lua_setfield(L, -2, "text");
  if (extra_key && extra_val) {
    lua_pushlstring(L, extra_val, extra_len);
    lua_setfield(L, -2, extra_key);
  }
  lua_rawseti(L, arr_idx, ++(*cnt));
}

static size_t find_sub(const char *s, size_t n, size_t from, const char *pat, size_t plen)
{
  if (plen == 0 || from + plen > n) return n;
  for (size_t i = from; i + plen <= n; i++) {
    if (memcmp(s + i, pat, plen) == 0) return i;
  }
  return n;
}

int sb_api_markdown_parse(lua_State *L)
{
  size_t n = 0;
  const char *s = luaL_checklstring(L, 1, &n);
  lua_newtable(L);
  int arr = lua_gettop(L);
  lua_Integer cnt = 0;
  size_t i = 0, text_start = 0;
#define FLUSH_TEXT(end_pos) do { \
  if ((end_pos) > text_start) push_md_tok(L, arr, &cnt, "text", s + text_start, (end_pos) - text_start, nullptr, nullptr, 0); \
} while (0)

  while (i < n) {
    // Fenced code block ```lang\n...\n```
    if (i + 2 < n && s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`') {
      size_t end = find_sub(s, n, i + 3, "```", 3);
      if (end < n) {
        FLUSH_TEXT(i);
        size_t ls = i + 3, le = ls;
        while (le < end && s[le] != '\n' && s[le] != ' ') le++;
        size_t body_s = (le < end && s[le] == '\n') ? le + 1 : ls;
        if (le == ls) body_s = ls;
        push_md_tok(L, arr, &cnt, "codeblock", s + body_s, end - body_s, "lang", s + ls, le > ls ? le - ls : 0);
        i = end + 3; text_start = i; continue;
      }
    }
    // Inline code `...`
    if (s[i] == '`') {
      size_t end = find_sub(s, n, i + 1, "`", 1);
      if (end < n && end > i + 1) {
        FLUSH_TEXT(i);
        push_md_tok(L, arr, &cnt, "code", s + i + 1, end - (i + 1), nullptr, nullptr, 0);
        i = end + 1; text_start = i; continue;
      }
    }
    // Spoiler ||...||
    if (i + 1 < n && s[i] == '|' && s[i + 1] == '|') {
      size_t end = find_sub(s, n, i + 2, "||", 2);
      if (end < n && end > i + 2) {
        FLUSH_TEXT(i);
        push_md_tok(L, arr, &cnt, "spoiler", s + i + 2, end - (i + 2), nullptr, nullptr, 0);
        i = end + 2; text_start = i; continue;
      }
    }
    // Bold **...**
    if (i + 1 < n && s[i] == '*' && s[i + 1] == '*') {
      size_t end = find_sub(s, n, i + 2, "**", 2);
      if (end < n && end > i + 2) {
        FLUSH_TEXT(i);
        push_md_tok(L, arr, &cnt, "bold", s + i + 2, end - (i + 2), nullptr, nullptr, 0);
        i = end + 2; text_start = i; continue;
      }
    }
    // Underline __...__
    if (i + 1 < n && s[i] == '_' && s[i + 1] == '_') {
      size_t end = find_sub(s, n, i + 2, "__", 2);
      if (end < n && end > i + 2) {
        FLUSH_TEXT(i);
        push_md_tok(L, arr, &cnt, "underline", s + i + 2, end - (i + 2), nullptr, nullptr, 0);
        i = end + 2; text_start = i; continue;
      }
    }
    // Strikethrough ~~...~~
    if (i + 1 < n && s[i] == '~' && s[i + 1] == '~') {
      size_t end = find_sub(s, n, i + 2, "~~", 2);
      if (end < n && end > i + 2) {
        FLUSH_TEXT(i);
        push_md_tok(L, arr, &cnt, "strike", s + i + 2, end - (i + 2), nullptr, nullptr, 0);
        i = end + 2; text_start = i; continue;
      }
    }
    // Italic *...*
    if (s[i] == '*') {
      size_t end = find_sub(s, n, i + 1, "*", 1);
      if (end < n && end > i + 1) {
        FLUSH_TEXT(i);
        push_md_tok(L, arr, &cnt, "italic", s + i + 1, end - (i + 1), nullptr, nullptr, 0);
        i = end + 1; text_start = i; continue;
      }
    }
    // Discord mentions <@123>, <#123>, <@&123>
    if (s[i] == '<' && i + 3 < n && (s[i + 1] == '@' || s[i + 1] == '#')) {
      size_t end = find_sub(s, n, i + 2, ">", 1);
      if (end < n && end - i <= 28u) {
        FLUSH_TEXT(i);
        const char *kind = s[i + 1] == '#' ? "channel_mention"
                         : (i + 2 < end && s[i + 2] == '&') ? "role_mention" : "user_mention";
        size_t id_s = i + 2;
        if (id_s < end && (s[id_s] == '!' || s[id_s] == '&')) id_s++;
        push_md_tok(L, arr, &cnt, kind, s + i, end - i + 1, "id", s + id_s, end - id_s);
        i = end + 1; text_start = i; continue;
      }
    }
    // URLs http:// or https://
    if ((i + 8 < n && memcmp(s + i, "https://", 8) == 0) ||
        (i + 7 < n && memcmp(s + i, "http://", 7) == 0)) {
      size_t end = i;
      while (end < n && !isspace((unsigned char)s[end]) && s[end] != '>' && s[end] != ')') end++;
      FLUSH_TEXT(i);
      push_md_tok(L, arr, &cnt, "link", s + i, end - i, "url", s + i, end - i);
      i = end; text_start = i; continue;
    }
    i++;
  }
  FLUSH_TEXT(n);
#undef FLUSH_TEXT
  return 1;
}

// ---------------------------------------------------------------- async.* (Coroutine-friendly async IPC)

static int resume_coro_guarded(ccord_sandbox *sb, lua_State *co, int nargs)
{
  if (sb->insn_per_call > 0) sb->budget = (int64_t)sb->insn_per_call;
  int nres = 0;
  int st = lua_resume(co, sb->L, nargs, &nres);
  if (st == LUA_ERRMEM && sb->limit == CCORD_SB_LIMIT_NONE) sb->limit = CCORD_SB_LIMIT_MEMORY;
  if (st != LUA_OK && st != LUA_YIELD) {
    const char *msg = lua_tostring(co, -1);
    sb_log(sb, CCORD_LOG_WARN, "async", msg ? msg : "coroutine error");
  }
  return st;
}

int sb_async_dispatch(lua_State *L, ccord_sandbox *sb, const char *method, ccord_buf *params_buf, int cb_idx)
{
  if (sb->limit != CCORD_SB_LIMIT_NONE) {
    if (params_buf) ccord_buf_free(params_buf);
    return luaL_error(L, "plugin stopped");
  }
  const char *params_json = params_buf ? ccord_buf_cstr(params_buf) : "{}";
  uint64_t req_id = ++sb->next_async_id;

  // Splice req_id into params_json (which must be a JSON object "{...}")
  ccord_buf b;
  ccord_buf_init(&b, 0);
  (void)ccord_buf_append(&b, "{\"req_id\":", 10);
  char idbuf[32];
  int idn = snprintf(idbuf, sizeof idbuf, "%llu", (unsigned long long)req_id);
  if (idn > 0) (void)ccord_buf_append(&b, idbuf, (size_t)idn);
  if (params_json && params_json[0] == '{') {
    const char *inner = params_json + 1;
    while (*inner == ' ' || *inner == '\t') inner++;
    if (*inner != '}' && *inner != '\0') {
      (void)ccord_buf_append(&b, ",", 1);
      (void)ccord_buf_append_str(&b, inner);
    } else {
      (void)ccord_buf_append(&b, "}", 1);
    }
  } else {
    (void)ccord_buf_append(&b, "}", 1);
  }
  if (params_buf) ccord_buf_free(params_buf);

  bool has_cb = cb_idx > 0 && lua_type(L, cb_idx) == LUA_TFUNCTION;
  bool is_coro = !has_cb && lua_isyieldable(L) && (L != sb->L);

  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_ASYNC);
  lua_createtable(L, 0, 2);
  if (has_cb) {
    lua_pushvalue(L, cb_idx);
    lua_setfield(L, -2, "cb");
  } else if (is_coro) {
    lua_pushthread(L);
    lua_setfield(L, -2, "co");
  }
  lua_rawseti(L, -2, (lua_Integer)req_id);
  lua_pop(L, 1);

  sb_request(sb, method, ccord_buf_cstr(&b));
  ccord_buf_free(&b);

  if (is_coro) {
    return lua_yield(L, 0);
  }
  lua_pushinteger(L, (lua_Integer)req_id);
  return 1;
}

void sb_async_chunks_free_all(ccord_sandbox *sb)
{
  if (!sb) return;
  for (size_t i = 0; i < sb->nasync_chunks; i++) {
    ccord_buf_free(&sb->async_chunks[i].buf);
  }
  ccord_free(sb->async_chunks);
  sb->async_chunks = nullptr;
  sb->nasync_chunks = 0;
  sb->capasync_chunks = 0;
}

static bool async_chunks_pop(ccord_sandbox *sb, uint64_t req_id, sb_async_chunk_buf *out)
{
  if (!sb || !out) return false;
  for (size_t i = 0; i < sb->nasync_chunks; i++) {
    if (sb->async_chunks[i].req_id == req_id) {
      *out = sb->async_chunks[i];
      sb->nasync_chunks--;
      if (i < sb->nasync_chunks) sb->async_chunks[i] = sb->async_chunks[sb->nasync_chunks];
      return true;
    }
  }
  return false;
}

ccord_status ccord_sandbox_async_reply_chunk(ccord_sandbox *sb, uint64_t req_id, uint64_t seq,
                                             const char *b64, size_t b64_len, ccord_error *err)
{
  (void)err;
  if (!sb || req_id == 0 || !b64) return CCORD_ERR_INVALID;
  sb_async_chunk_buf *slot = nullptr;
  for (size_t i = 0; i < sb->nasync_chunks; i++) {
    if (sb->async_chunks[i].req_id == req_id) {
      slot = &sb->async_chunks[i];
      break;
    }
  }
  if (!slot) {
    if (sb->nasync_chunks >= SB_MAX_ASYNC_CHUNKS) return CCORD_OK;
    if (sb->nasync_chunks == sb->capasync_chunks) {
      size_t nc = sb->capasync_chunks ? sb->capasync_chunks * 2u : 4u;
      if (nc > SB_MAX_ASYNC_CHUNKS) nc = SB_MAX_ASYNC_CHUNKS;
      sb_async_chunk_buf *g = ccord_realloc_array(sb->async_chunks, nc, sizeof *g);
      if (!g) return CCORD_ERR_NOMEM;
      sb->async_chunks = g;
      sb->capasync_chunks = nc;
    }
    slot = &sb->async_chunks[sb->nasync_chunks++];
    memset(slot, 0, sizeof *slot);
    slot->req_id = req_id;
    ccord_buf_init(&slot->buf, SB_MAX_HTTP_BODY_BYTES);
  }
  if (slot->failed || seq != slot->next_seq) {
    slot->failed = true;
    return CCORD_OK;
  }
  size_t est = (b64_len / 4u) * 3u;
  if (est > SB_MAX_HTTP_BODY_BYTES || slot->buf.len > SB_MAX_HTTP_BODY_BYTES - est) {
    slot->failed = true;
    ccord_buf_free(&slot->buf);
    return CCORD_OK;
  }
  if (b64_len > 0 && !sb_b64_decode_buf(b64, b64_len, &slot->buf)) {
    slot->failed = true;
    ccord_buf_free(&slot->buf);
    return CCORD_OK;
  }
  slot->next_seq++;
  return CCORD_OK;
}

static bool push_async_result_value(lua_State *L, const ccord_json *result,
                                    const sb_async_chunk_buf *chunk, bool has_chunk,
                                    char *err_out, size_t err_cap)
{
  if (!result) {
    lua_pushboolean(L, 1);
    return true;
  }
  if (ccord_json_type_of(result) == CCORD_JSON_OBJECT) {
    bool is_chunked = ccord_json_bool(ccord_json_get(result, "chunked"), false);
    bool is_binary = ccord_json_bool(ccord_json_get(result, "binary"), false);
    if (is_chunked) {
      if (!has_chunk || chunk->failed) {
        snprintf(err_out, err_cap, "chunked response assembly failed");
        return false;
      }
      size_t n = ccord_json_object_len(result);
      lua_createtable(L, 0, (int)n + 1);
      for (size_t i = 0; i < n; i++) {
        const ccord_json *v = nullptr;
        const char *k = ccord_json_object_key(result, i, &v);
        if (!k || strcmp(k, "chunked") == 0) continue;
        if (!sb_push_json(L, v)) {
          lua_pop(L, 1);
          snprintf(err_out, err_cap, "failed to push response field");
          return false;
        }
        lua_setfield(L, -2, k);
      }
      lua_pushlstring(L, chunk->buf.data ? chunk->buf.data : "", chunk->buf.len);
      lua_setfield(L, -2, "body");
      return true;
    }
    if (is_binary && ccord_json_get(result, "body_b64")) {
      size_t b64l = 0;
      const char *b64 = ccord_json_string(ccord_json_get(result, "body_b64"), &b64l);
      ccord_buf dec;
      ccord_buf_init(&dec, (b64l / 4u) * 3u + 8u);
      if (b64 && b64l > 0 && !sb_b64_decode_buf(b64, b64l, &dec)) {
        ccord_buf_free(&dec);
        snprintf(err_out, err_cap, "failed to decode binary response");
        return false;
      }
      size_t n = ccord_json_object_len(result);
      lua_createtable(L, 0, (int)n);
      for (size_t i = 0; i < n; i++) {
        const ccord_json *v = nullptr;
        const char *k = ccord_json_object_key(result, i, &v);
        if (!k || strcmp(k, "binary") == 0 || strcmp(k, "body_b64") == 0) continue;
        if (!sb_push_json(L, v)) {
          lua_pop(L, 1);
          ccord_buf_free(&dec);
          snprintf(err_out, err_cap, "failed to push response field");
          return false;
        }
        lua_setfield(L, -2, k);
      }
      lua_pushlstring(L, dec.data ? dec.data : "", dec.len);
      lua_setfield(L, -2, "body");
      ccord_buf_free(&dec);
      return true;
    }
  }
  if (!sb_push_json(L, result)) {
    snprintf(err_out, err_cap, "response exceeded stack depth");
    return false;
  }
  return true;
}

void sb_async_resolve_entry(ccord_sandbox *sb, uint64_t req_id, bool ok, const ccord_json *result, const char *err_msg)
{
  sb_async_chunk_buf chunk = {0};
  bool has_chunk = async_chunks_pop(sb, req_id, &chunk);

  lua_State *L = sb->L;
  int top = lua_gettop(L);
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_ASYNC);
  if (lua_rawgeti(L, -1, (lua_Integer)req_id) != LUA_TTABLE) {
    if (has_chunk) ccord_buf_free(&chunk.buf);
    lua_settop(L, top);
    return;
  }
  // Remove entry before invoking callback so re-entrant async calls work cleanly.
  lua_pushnil(L);
  lua_rawseti(L, -3, (lua_Integer)req_id);

  lua_getfield(L, -1, "cb");
  if (lua_isfunction(L, -1)) {
    char dec_err[128] = "";
    if (ok) {
      if (push_async_result_value(L, result, &chunk, has_chunk, dec_err, sizeof dec_err)) {
        lua_pushnil(L);
      } else {
        lua_pushnil(L);
        lua_pushstring(L, dec_err[0] ? dec_err : "response decode failed");
      }
    } else {
      lua_pushnil(L);
      lua_pushstring(L, err_msg && err_msg[0] ? err_msg : "request failed");
    }
    if (has_chunk) { ccord_buf_free(&chunk.buf); has_chunk = false; }
    ccord_error e = {0};
    if (sb_call_guarded(sb, 2, 0, &e) != CCORD_OK) {
      sb_log(sb, CCORD_LOG_WARN, "async.callback", e.message);
    }
    lua_settop(L, top);
    return;
  }
  lua_pop(L, 1);

  lua_getfield(L, -1, "co");
  if (lua_isthread(L, -1)) {
    lua_State *co = lua_tothread(L, -1);
    char dec_err[128] = "";
    if (ok) {
      if (push_async_result_value(co, result, &chunk, has_chunk, dec_err, sizeof dec_err)) {
        lua_pushnil(co);
      } else {
        lua_pushnil(co);
        lua_pushstring(co, dec_err[0] ? dec_err : "response decode failed");
      }
    } else {
      lua_pushnil(co);
      lua_pushstring(co, err_msg && err_msg[0] ? err_msg : "request failed");
    }
    if (has_chunk) { ccord_buf_free(&chunk.buf); has_chunk = false; }
    (void)resume_coro_guarded(sb, co, 2);
  }
  if (has_chunk) ccord_buf_free(&chunk.buf);
  lua_settop(L, top);
}

static int api_async_spawn(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);
  int nargs = lua_gettop(L) - 1;
  lua_State *co = lua_newthread(L);
  sb_install_hook_on_thread(sb, co);
  // Pin thread in SB_KEY_ASYNC until it finishes? Keep ref only while yielded (async entries hold it).
  lua_pushvalue(L, 1);
  lua_xmove(L, co, 1);
  for (int i = 0; i < nargs; i++) {
    lua_pushvalue(L, 2 + i);
    lua_xmove(L, co, 1);
  }
  (void)resume_coro_guarded(sb, co, nargs);
  return 1;
}

static int sleep_resume_cb(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  lua_State *co = lua_tothread(L, lua_upvalueindex(1));
  if (co) (void)resume_coro_guarded(sb, co, 0);
  return 0;
}

static int api_async_sleep(lua_State *L)
{
  ccord_sandbox *sb = sb_of(L);
  lua_Integer ms = luaL_checkinteger(L, 1);
  if (ms < 0) ms = 0;
  if (ms > 3600000) ms = 3600000;
  if (!lua_isyieldable(L) || L == sb->L) {
    return luaL_error(L, "async.sleep must be called inside async.spawn() or a coroutine");
  }
  if (sb->ntimers >= SB_MAX_TIMERS) return luaL_error(L, "async.sleep: timer limit reached");
  if (sb->ntimers == sb->captimers) {
    size_t nc = sb->captimers ? sb->captimers * 2u : 8u;
    sb_timer *g = ccord_realloc_array(sb->timers, nc, sizeof *g);
    if (!g) return luaL_error(L, "out of memory");
    sb->timers = g; sb->captimers = nc;
  }
  lua_getfield(L, LUA_REGISTRYINDEX, SB_KEY_TIMERS);
  lua_pushthread(L);
  lua_pushcclosure(L, sleep_resume_cb, 1);
  int ref = luaL_ref(L, -2);
  lua_pop(L, 1);
  sb->timers[sb->ntimers++] = (sb_timer){
    .id = ++sb->next_timer_id,
    .fire_at_ms = ccord_clock_ms() + (uint64_t)ms,
    .interval_ms = 0,
    .fn_ref = ref,
    .cancelled = false,
  };
  return lua_yield(L, 0);
}

static const luaL_Reg k_async[] = {
  {"spawn", api_async_spawn}, {"sleep", api_async_sleep}, {nullptr, nullptr},
};

void sb_open_libs_v3(lua_State *L)
{
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, SB_KEY_ASYNC);
  sb_reg_ro(L, "color", k_color);
  sb_reg_ro(L, "compress", k_compress);
  sb_reg_ro(L, "semver", k_semver);
  sb_reg_ro(L, "emoji", k_emoji);
  sb_reg_ro(L, "async", k_async);
}
