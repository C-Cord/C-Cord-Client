// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Pure-C23 Lottie JSON vector animation rasterizer (zero external dependencies beyond ccord_core + libm).
#include "lottie.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>

#define LOT_MAX_INPUT ((size_t)4u * 1024u * 1024u)
#define LOT_MAX_DIM 160            // matches Discord's sticker CDN display resolution (?size=160)
#define LOT_MAX_FRAMES 120u
#define LOT_TARGET_FPS 20.0f
#define LOT_MAX_DEPTH 12
#define LOT_MAX_PARENT_DEPTH 16
#define LOT_MAX_PTS 32768u
#define LOT_MAX_EDGES 65536u
#define LOT_MAX_SUBPATHS 1024u
#define LOT_PI 3.14159265358979323846f
#define LOT_DEG2RAD (LOT_PI / 180.0f)

// ---- sniffing ----

static bool mem_contains(const uint8_t *buf, size_t len, const char *needle)
{
  size_t nlen = strlen(needle);
  if (nlen == 0 || len < nlen) return false;
  for (size_t i = 0; i + nlen <= len; i++) {
    if (buf[i] == (uint8_t)needle[0] && memcmp(buf + i, needle, nlen) == 0) return true;
  }
  return false;
}

bool ccord_lottie_sniff(const uint8_t *data, size_t len)
{
  if (!data || len < 24 || len > LOT_MAX_INPUT) return false;
  size_t i = 0;
  while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
  if (i >= len || data[i] != '{') return false;
  // Fast header/tail scan: standard Lottie payloads place "fr", "ip", "op", "layers" near top-level boundaries.
  size_t head_n = len - i < 4096u ? len - i : 4096u;
  const uint8_t *head = data + i;
  size_t tail_off = len > 1024u ? len - 1024u : 0u;
  const uint8_t *tail = data + tail_off;
  size_t tail_n = len - tail_off;
  bool has_fr = mem_contains(head, head_n, "\"fr\"") || mem_contains(tail, tail_n, "\"fr\"");
  bool has_ip = mem_contains(head, head_n, "\"ip\"") || mem_contains(tail, tail_n, "\"ip\"");
  bool has_op = mem_contains(head, head_n, "\"op\"") || mem_contains(tail, tail_n, "\"op\"");
  bool has_ly = mem_contains(head, head_n, "\"layers\"") || mem_contains(tail, tail_n, "\"layers\"");
  if (!(has_fr && has_ip && has_op)) return false;
  if (has_ly) return true;
  // Large assets array can push "layers" into the middle/tail of the JSON.
  return mem_contains(data, len, "\"layers\"");
}

// ---- math / 2D affine ----

typedef struct lot_vec2 {
  float x, y;
} lot_vec2;

typedef struct lot_mat {
  float a, b, c, d, tx, ty;   // x' = a*x + c*y + tx, y' = b*x + d*y + ty
} lot_mat;

static lot_mat mat_id(void) { return (lot_mat){1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}; }

static lot_mat mat_mul(lot_mat m, lot_mat n)
{
  return (lot_mat){
    m.a * n.a + m.c * n.b,
    m.b * n.a + m.d * n.b,
    m.a * n.c + m.c * n.d,
    m.b * n.c + m.d * n.d,
    m.a * n.tx + m.c * n.ty + m.tx,
    m.b * n.tx + m.d * n.ty + m.ty,
  };
}

static lot_vec2 mat_pt(lot_mat m, float x, float y)
{
  return (lot_vec2){m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty};
}

static float mat_avg_scale(lot_mat m)
{
  float s = sqrtf((m.a * m.a + m.b * m.b + m.c * m.c + m.d * m.d) * 0.5f);
  return isfinite(s) ? s : 1.0f;
}

static float clampf_lot(float v, float lo, float hi)
{
  if (!isfinite(v)) return lo;
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---- JSON helpers & keyframe animation ----

static float j_num(const ccord_json *j, float def)
{
  if (!j) return def;
  if (ccord_json_type_of(j) == CCORD_JSON_NUMBER) {
    float v = (float)ccord_json_number(j, (double)def);
    return isfinite(v) ? v : def;
  }
  if (ccord_json_type_of(j) == CCORD_JSON_ARRAY && ccord_json_array_len(j) > 0) {
    float v = (float)ccord_json_number(ccord_json_array_at(j, 0), (double)def);
    return isfinite(v) ? v : def;
  }
  return def;
}

static float j_get_num(const ccord_json *obj, const char *key, float def)
{
  return obj ? j_num(ccord_json_get(obj, key), def) : def;
}

static const char *j_get_str(const ccord_json *obj, const char *key)
{
  if (!obj) return nullptr;
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_string(v, nullptr) : nullptr;
}

static bool j_is_animated(const ccord_json *prop)
{
  if (!prop || ccord_json_type_of(prop) != CCORD_JSON_OBJECT) return false;
  const ccord_json *a = ccord_json_get(prop, "a");
  const ccord_json *k = ccord_json_get(prop, "k");
  if (!k || ccord_json_type_of(k) != CCORD_JSON_ARRAY || ccord_json_array_len(k) == 0) return false;
  if (a && j_num(a, 0.0f) >= 0.5f) return true;
  const ccord_json *first = ccord_json_array_at(k, 0);
  return first && ccord_json_type_of(first) == CCORD_JSON_OBJECT && ccord_json_get(first, "t") != nullptr;
}

static float j_axis(const ccord_json *obj, const char *key, size_t comp, float def)
{
  if (!obj) return def;
  const ccord_json *v = ccord_json_get(obj, key);
  if (!v) return def;
  if (ccord_json_type_of(v) == CCORD_JSON_NUMBER) return j_num(v, def);
  if (ccord_json_type_of(v) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(v);
    if (n == 0) return def;
    return j_num(ccord_json_array_at(v, comp < n ? comp : n - 1u), def);
  }
  return def;
}

// Evaluates cubic Bezier timing curve (0,0)-(ox,oy)-(ix,iy)-(1,1) at normalized time u in [0,1].
static float cubic_ease(float u, float ox, float oy, float ix, float iy)
{
  if (u <= 0.0f) return 0.0f;
  if (u >= 1.0f) return 1.0f;
  ox = clampf_lot(ox, 0.0f, 1.0f);
  ix = clampf_lot(ix, 0.0f, 1.0f);
  if (fabsf(ox - oy) < 1e-4f && fabsf(ix - iy) < 1e-4f) return u;
  float lo = 0.0f, hi = 1.0f, t = u;
  for (int iter = 0; iter < 8; iter++) {
    float mt = 1.0f - t;
    float x = (3.0f * mt * mt * ox + 3.0f * mt * t * ix + t * t) * t;
    float dx = 3.0f * mt * mt * ox + 6.0f * mt * t * (ix - ox) + 3.0f * t * t * (1.0f - ix);
    float diff = x - u;
    if (fabsf(diff) < 1e-5f) break;
    if (diff < 0.0f) lo = t;
    else hi = t;
    if (fabsf(dx) > 1e-5f) {
      float nt = t - diff / dx;
      t = (nt > lo && nt < hi) ? nt : (lo + hi) * 0.5f;
    } else {
      t = (lo + hi) * 0.5f;
    }
  }
  float mt = 1.0f - t;
  return (3.0f * mt * mt * oy + 3.0f * mt * t * iy + t * t) * t;
}

static void j_read_vec(const ccord_json *v, float *out, size_t n, const float *def)
{
  for (size_t i = 0; i < n; i++) out[i] = def ? def[i] : 0.0f;
  if (!v) return;
  if (ccord_json_type_of(v) == CCORD_JSON_NUMBER) {
    if (n > 0) out[0] = j_num(v, out[0]);
    return;
  }
  if (ccord_json_type_of(v) == CCORD_JSON_ARRAY) {
    size_t len = ccord_json_array_len(v);
    for (size_t i = 0; i < n && i < len; i++) {
      out[i] = j_num(ccord_json_array_at(v, i), out[i]);
    }
  }
}

static void eval_prop_vec(const ccord_json *prop, float frame, float *out, size_t n, const float *def)
{
  for (size_t i = 0; i < n; i++) out[i] = def ? def[i] : 0.0f;
  if (!prop || ccord_json_type_of(prop) != CCORD_JSON_OBJECT) return;
  // Split 2D position: {"s": true, "x": {...}, "y": {...}}
  if (n >= 2 && ccord_json_bool(ccord_json_get(prop, "s"), false)) {
    eval_prop_vec(ccord_json_get(prop, "x"), frame, &out[0], 1, def ? &def[0] : nullptr);
    eval_prop_vec(ccord_json_get(prop, "y"), frame, &out[1], 1, def ? &def[1] : nullptr);
    return;
  }
  const ccord_json *k = ccord_json_get(prop, "k");
  if (!k) return;
  if (!j_is_animated(prop)) {
    j_read_vec(k, out, n, def);
    return;
  }
  size_t nk = ccord_json_array_len(k);
  if (nk == 0) return;
  const ccord_json *kf0 = ccord_json_array_at(k, 0);
  if (frame <= j_get_num(kf0, "t", 0.0f)) {
    j_read_vec(ccord_json_get(kf0, "s"), out, n, def);
    return;
  }
  for (size_t i = 0; i + 1u < nk; i++) {
    const ccord_json *ka = ccord_json_array_at(k, i);
    const ccord_json *kb = ccord_json_array_at(k, i + 1u);
    float ta = j_get_num(ka, "t", 0.0f);
    float tb = j_get_num(kb, "t", ta);
    if (frame < tb || i + 2u == nk) {
      if (frame >= tb) {
        const ccord_json *sv = ccord_json_get(kb, "s");
        if (sv) j_read_vec(sv, out, n, def);
        else {
          const ccord_json *ev = ccord_json_get(ka, "e");
          j_read_vec(ev ? ev : ccord_json_get(ka, "s"), out, n, def);
        }
        return;
      }
      if (j_get_num(ka, "h", 0.0f) >= 0.5f || tb <= ta) {
        j_read_vec(ccord_json_get(ka, "s"), out, n, def);
        return;
      }
      float s[4] = {0}, e[4] = {0};
      j_read_vec(ccord_json_get(ka, "s"), s, n, def);
      const ccord_json *ev = ccord_json_get(ka, "e");
      j_read_vec(ev ? ev : ccord_json_get(kb, "s"), e, n, def);
      float u = clampf_lot((frame - ta) / (tb - ta), 0.0f, 1.0f);
      const ccord_json *oo = ccord_json_get(ka, "o");
      const ccord_json *ii = ccord_json_get(ka, "i");
      const ccord_json *to = ccord_json_get(ka, "to");
      const ccord_json *ti = ccord_json_get(ka, "ti");
      if (n >= 2 && to && ti && ccord_json_type_of(to) == CCORD_JSON_ARRAY &&
          ccord_json_type_of(ti) == CCORD_JSON_ARRAY) {
        float tv[2] = {0}, iv[2] = {0};
        j_read_vec(to, tv, 2, nullptr);
        j_read_vec(ti, iv, 2, nullptr);
        if (fabsf(tv[0]) + fabsf(tv[1]) + fabsf(iv[0]) + fabsf(iv[1]) > 1e-4f) {
          float ox = j_axis(oo, "x", 0, 0.0f), oy = j_axis(oo, "y", 0, 0.0f);
          float ix = j_axis(ii, "x", 0, 1.0f), iy = j_axis(ii, "y", 0, 1.0f);
          float t = cubic_ease(u, ox, oy, ix, iy);
          float mt = 1.0f - t;
          float p1x = s[0] + tv[0], p1y = s[1] + tv[1];
          float p2x = e[0] + iv[0], p2y = e[1] + iv[1];
          out[0] = mt * mt * mt * s[0] + 3.0f * mt * mt * t * p1x + 3.0f * mt * t * t * p2x + t * t * t * e[0];
          out[1] = mt * mt * mt * s[1] + 3.0f * mt * mt * t * p1y + 3.0f * mt * t * t * p2y + t * t * t * e[1];
          for (size_t c = 2; c < n; c++) out[c] = s[c] + (e[c] - s[c]) * t;
          return;
        }
      }
      for (size_t c = 0; c < n; c++) {
        float ox = j_axis(oo, "x", c, 0.0f), oy = j_axis(oo, "y", c, 0.0f);
        float ix = j_axis(ii, "x", c, 1.0f), iy = j_axis(ii, "y", c, 1.0f);
        float t = cubic_ease(u, ox, oy, ix, iy);
        out[c] = s[c] + (e[c] - s[c]) * t;
      }
      return;
    }
  }
  const ccord_json *last = ccord_json_array_at(k, nk - 1u);
  const ccord_json *sv = ccord_json_get(last, "s");
  if (sv) j_read_vec(sv, out, n, def);
}

static float eval_prop_scalar(const ccord_json *prop, float frame, float def)
{
  float v = def;
  eval_prop_vec(prop, frame, &v, 1, &def);
  return v;
}

static void eval_transform(const ccord_json *ks, float frame, lot_mat *out_m, float *out_op)
{
  *out_m = mat_id();
  if (out_op) *out_op = 1.0f;
  if (!ks || ccord_json_type_of(ks) != CCORD_JSON_OBJECT) return;
  float def0[2] = {0.0f, 0.0f};
  float def100[2] = {100.0f, 100.0f};
  float a[2], p[2], s[2];
  eval_prop_vec(ccord_json_get(ks, "a"), frame, a, 2, def0);
  eval_prop_vec(ccord_json_get(ks, "p"), frame, p, 2, def0);
  eval_prop_vec(ccord_json_get(ks, "s"), frame, s, 2, def100);
  float r_deg = eval_prop_scalar(ccord_json_get(ks, "r"), frame, 0.0f);
  float sk_deg = eval_prop_scalar(ccord_json_get(ks, "sk"), frame, 0.0f);
  float sa_deg = eval_prop_scalar(ccord_json_get(ks, "sa"), frame, 0.0f);
  if (out_op) *out_op = clampf_lot(eval_prop_scalar(ccord_json_get(ks, "o"), frame, 100.0f) * 0.01f, 0.0f, 1.0f);

  lot_mat m = {1.0f, 0.0f, 0.0f, 1.0f, p[0], p[1]};
  if (fabsf(r_deg) > 1e-4f) {
    float rad = r_deg * LOT_DEG2RAD;
    float cs = cosf(rad), sn = sinf(rad);
    m = mat_mul(m, (lot_mat){cs, sn, -sn, cs, 0.0f, 0.0f});
  }
  if (fabsf(sk_deg) > 1e-4f) {
    float sa = sa_deg * LOT_DEG2RAD;
    float cs = cosf(sa), sn = sinf(sa);
    float sk = clampf_lot(sk_deg, -85.0f, 85.0f) * LOT_DEG2RAD;
    lot_mat r1 = {cs, -sn, sn, cs, 0.0f, 0.0f};
    lot_mat sh = {1.0f, 0.0f, -tanf(sk), 1.0f, 0.0f, 0.0f};
    lot_mat r2 = {cs, sn, -sn, cs, 0.0f, 0.0f};
    m = mat_mul(m, mat_mul(r1, mat_mul(sh, r2)));
  }
  m = mat_mul(m, (lot_mat){s[0] * 0.01f, 0.0f, 0.0f, s[1] * 0.01f, 0.0f, 0.0f});
  m = mat_mul(m, (lot_mat){1.0f, 0.0f, 0.0f, 1.0f, -a[0], -a[1]});
  *out_m = m;
}

// ---- polyline / edge / rasterizer buffers ----

typedef struct lot_subpath {
  size_t start;
  size_t count;
  bool closed;
} lot_subpath;

typedef struct lot_edge {
  float x0, y0, x1, y1;   // always y0 < y1
  int8_t dir;
} lot_edge;

typedef struct lot_cross {
  float x;
  int8_t dir;
} lot_cross;

typedef struct lot_grad_stop {
  float pos;
  float r, g, b, a;
} lot_grad_stop;

typedef struct lot_paint {
  bool is_grad;
  int grad_type;   // 1 = linear, 2 = radial
  lot_vec2 gs, ge;
  float r, g, b, a;
  lot_grad_stop stops[32];
  size_t nstops;
  int fill_rule;   // 1 = nonzero, 2 = even-odd
} lot_paint;

typedef struct lot_trim {
  bool active;
  float s, e, o;
  int mode;   // 1 = simultaneous, 2 = individually
} lot_trim;

typedef struct lot_paths {
  lot_vec2 *pts;
  size_t npts, cap_pts;
  lot_subpath *subs;
  size_t nsubs, cap_subs;
} lot_paths;

struct ccord_lottie_stream {
  ccord_arena arena;
  const ccord_json *root;
  float src_w, src_h;
  float ip, op, fr;
  int out_w, out_h;
  uint32_t frame_count;
  uint32_t cur_idx;
  uint32_t dur_ms;
  uint32_t total_ms;
  uint8_t *premul;     // out_w * out_h * 4 premultiplied working canvas
  uint8_t *out_rgba;   // out_w * out_h * 4 straight-alpha output frame
  // Reusable rasterization scratch buffers (never malloc per shape)
  lot_edge *edges;
  size_t nedges, cap_edges;
  lot_cross *cross;
  size_t cap_cross;
  uint16_t *cov;       // out_w entries (0..256)
};

static void paths_free(lot_paths *p)
{
  ccord_free(p->pts);
  ccord_free(p->subs);
  memset(p, 0, sizeof *p);
}

static bool paths_push_pt(lot_paths *p, float x, float y)
{
  if (p->npts >= LOT_MAX_PTS) return false;
  if (p->npts == p->cap_pts) {
    size_t nc = p->cap_pts ? p->cap_pts * 2u : 128u;
    if (nc > LOT_MAX_PTS) nc = LOT_MAX_PTS;
    lot_vec2 *np = ccord_realloc_array(p->pts, nc, sizeof *np);
    if (!np) return false;
    p->pts = np;
    p->cap_pts = nc;
  }
  p->pts[p->npts++] = (lot_vec2){x, y};
  return true;
}

static bool paths_begin(lot_paths *p, bool closed, size_t *out_idx)
{
  if (p->nsubs >= LOT_MAX_SUBPATHS) return false;
  if (p->nsubs == p->cap_subs) {
    size_t nc = p->cap_subs ? p->cap_subs * 2u : 16u;
    if (nc > LOT_MAX_SUBPATHS) nc = LOT_MAX_SUBPATHS;
    lot_subpath *ns = ccord_realloc_array(p->subs, nc, sizeof *ns);
    if (!ns) return false;
    p->subs = ns;
    p->cap_subs = nc;
  }
  *out_idx = p->nsubs;
  p->subs[p->nsubs++] = (lot_subpath){.start = p->npts, .count = 0, .closed = closed};
  return true;
}

static void paths_end(lot_paths *p, size_t idx)
{
  if (idx < p->nsubs) p->subs[idx].count = p->npts - p->subs[idx].start;
}

static void flatten_cubic(lot_paths *p, lot_vec2 p0, lot_vec2 p1, lot_vec2 p2, lot_vec2 p3, int depth)
{
  // Distance of control points from chord p0->p3 in screen pixels.
  float dx = p3.x - p0.x, dy = p3.y - p0.y;
  float d2 = dx * dx + dy * dy;
  float err1, err2;
  if (d2 > 1e-6f) {
    float c1 = (p1.x - p3.x) * dy - (p1.y - p3.y) * dx;
    float c2 = (p2.x - p3.x) * dy - (p2.y - p3.y) * dx;
    err1 = (c1 * c1) / d2;
    err2 = (c2 * c2) / d2;
  } else {
    err1 = (p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y);
    err2 = (p2.x - p0.x) * (p2.x - p0.x) + (p2.y - p0.y) * (p2.y - p0.y);
  }
  if (depth >= 6 || (err1 <= 0.14f && err2 <= 0.14f)) {
    (void)paths_push_pt(p, p3.x, p3.y);
    return;
  }
  lot_vec2 p01 = {(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
  lot_vec2 p12 = {(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
  lot_vec2 p23 = {(p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f};
  lot_vec2 p012 = {(p01.x + p12.x) * 0.5f, (p01.y + p12.y) * 0.5f};
  lot_vec2 p123 = {(p12.x + p23.x) * 0.5f, (p12.y + p23.y) * 0.5f};
  lot_vec2 mid = {(p012.x + p123.x) * 0.5f, (p012.y + p123.y) * 0.5f};
  flatten_cubic(p, p0, p01, p012, mid, depth + 1);
  flatten_cubic(p, mid, p123, p23, p3, depth + 1);
}

// ---- edge table & analytical scanline rasterizer ----

static void edges_push(ccord_lottie_stream *s, float x0, float y0, float x1, float y1)
{
  if (s->nedges >= LOT_MAX_EDGES || fabsf(y1 - y0) < 1e-5f) return;
  if (s->nedges == s->cap_edges) {
    size_t nc = s->cap_edges ? s->cap_edges * 2u : 512u;
    if (nc > LOT_MAX_EDGES) nc = LOT_MAX_EDGES;
    lot_edge *ne = ccord_realloc_array(s->edges, nc, sizeof *ne);
    if (!ne) return;
    s->edges = ne;
    s->cap_edges = nc;
  }
  if (y0 < y1) s->edges[s->nedges++] = (lot_edge){x0, y0, x1, y1, 1};
  else s->edges[s->nedges++] = (lot_edge){x1, y1, x0, y0, -1};
}

static void add_poly_edges(ccord_lottie_stream *s, const lot_vec2 *pts, size_t n)
{
  if (n < 3) return;
  for (size_t i = 0; i < n; i++) {
    lot_vec2 a = pts[i], b = pts[(i + 1u) % n];
    edges_push(s, a.x, a.y, b.x, b.y);
  }
}

// Emits a polygon with consistent clockwise orientation (interior winding -1) so overlapping stroke
// quads, joins and round caps always union under nonzero winding and never cancel each other out.
static void add_poly_edges_cw(ccord_lottie_stream *s, const lot_vec2 *pts, size_t n)
{
  if (n < 3) return;
  float area2 = 0.0f;
  for (size_t i = 0; i < n; i++) {
    lot_vec2 a = pts[i], b = pts[(i + 1u) % n];
    area2 += (b.x - a.x) * (b.y + a.y);
  }
  if (fabsf(area2) < 1e-6f) return;
  if (area2 > 0.0f) {
    for (size_t i = n; i > 0; i--) {
      lot_vec2 a = pts[i - 1u];
      lot_vec2 b = pts[i > 1u ? i - 2u : n - 1u];
      edges_push(s, a.x, a.y, b.x, b.y);
    }
  } else {
    add_poly_edges(s, pts, n);
  }
}

static void add_circle_edges(ccord_lottie_stream *s, float cx, float cy, float rad)
{
  if (rad < 0.25f) return;
  enum { SEGS = 12 };
  lot_vec2 ring[SEGS];
  for (int i = 0; i < SEGS; i++) {
    float a = (float)i * (2.0f * LOT_PI / (float)SEGS);
    ring[i] = (lot_vec2){cx + cosf(a) * rad, cy + sinf(a) * rad};
  }
  add_poly_edges_cw(s, ring, SEGS);
}

static void sample_gradient(const lot_paint *pt, float px, float py, float *r, float *g, float *b, float *a)
{
  if (pt->nstops == 0) {
    *r = pt->r;
    *g = pt->g;
    *b = pt->b;
    *a = pt->a;
    return;
  }
  float dx = pt->ge.x - pt->gs.x, dy = pt->ge.y - pt->gs.y;
  float len2 = dx * dx + dy * dy;
  float t = 0.0f;
  if (len2 > 1e-6f) {
    if (pt->grad_type == 2) {
      float rx = px - pt->gs.x, ry = py - pt->gs.y;
      t = sqrtf((rx * rx + ry * ry) / len2);
    } else {
      t = ((px - pt->gs.x) * dx + (py - pt->gs.y) * dy) / len2;
    }
  }
  t = clampf_lot(t, 0.0f, 1.0f);
  if (t <= pt->stops[0].pos) {
    *r = pt->stops[0].r;
    *g = pt->stops[0].g;
    *b = pt->stops[0].b;
    *a = pt->stops[0].a * pt->a;
    return;
  }
  for (size_t i = 0; i + 1u < pt->nstops; i++) {
    const lot_grad_stop *sa = &pt->stops[i];
    const lot_grad_stop *sb = &pt->stops[i + 1u];
    if (t <= sb->pos) {
      float span = sb->pos - sa->pos;
      float f = span > 1e-5f ? (t - sa->pos) / span : 0.0f;
      *r = sa->r + (sb->r - sa->r) * f;
      *g = sa->g + (sb->g - sa->g) * f;
      *b = sa->b + (sb->b - sa->b) * f;
      *a = (sa->a + (sb->a - sa->a) * f) * pt->a;
      return;
    }
  }
  const lot_grad_stop *last = &pt->stops[pt->nstops - 1u];
  *r = last->r;
  *g = last->g;
  *b = last->b;
  *a = last->a * pt->a;
}

static inline void blend_premul_pixel(uint8_t *dst, uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa)
{
  if (pa == 0) return;
  if (pa == 255) {
    dst[0] = pr;
    dst[1] = pg;
    dst[2] = pb;
    dst[3] = 255;
    return;
  }
  uint32_t inv = 255u - pa;
  dst[0] = (uint8_t)(pr + ((uint32_t)dst[0] * inv + 127u) / 255u);
  dst[1] = (uint8_t)(pg + ((uint32_t)dst[1] * inv + 127u) / 255u);
  dst[2] = (uint8_t)(pb + ((uint32_t)dst[2] * inv + 127u) / 255u);
  dst[3] = (uint8_t)(pa + ((uint32_t)dst[3] * inv + 127u) / 255u);
}

// Rasterizes s->edges[0..s->nedges) into premultiplied RGBA `dst` (or 8-bit alpha `alpha_dst` if non-null).
static void rasterize_edges(ccord_lottie_stream *s, uint8_t *dst, uint8_t *alpha_dst, const lot_paint *pt)
{
  if (s->nedges == 0) return;
  if (!alpha_dst && pt->a <= 1e-4f) return;
  int w = s->out_w, h = s->out_h;
  float min_y = (float)h, max_y = 0.0f;
  for (size_t i = 0; i < s->nedges; i++) {
    if (s->edges[i].y0 < min_y) min_y = s->edges[i].y0;
    if (s->edges[i].y1 > max_y) max_y = s->edges[i].y1;
  }
  int y0 = (int)floorf(min_y);
  int y1 = (int)ceilf(max_y);
  if (y0 < 0) y0 = 0;
  if (y1 > h) y1 = h;
  if (y0 >= y1) return;

  if (s->nedges > s->cap_cross) {
    size_t nc = s->nedges < 256u ? 256u : s->nedges;
    lot_cross *ncr = ccord_realloc_array(s->cross, nc, sizeof *ncr);
    if (!ncr) return;
    s->cross = ncr;
    s->cap_cross = nc;
  }

  const bool even_odd = pt->fill_rule == 2;
  const float solid_a = clampf_lot(pt->a, 0.0f, 1.0f);
  const float solid_r = clampf_lot(pt->r, 0.0f, 1.0f);
  const float solid_g = clampf_lot(pt->g, 0.0f, 1.0f);
  const float solid_b = clampf_lot(pt->b, 0.0f, 1.0f);

  for (int py = y0; py < y1; py++) {
    memset(s->cov, 0, (size_t)w * sizeof *s->cov);
    int row_x0 = w, row_x1 = 0;
    for (int sub = 0; sub < 4; sub++) {
      float sy = (float)py + ((float)sub + 0.5f) * 0.25f;
      size_t ncr = 0;
      for (size_t i = 0; i < s->nedges; i++) {
        const lot_edge *e = &s->edges[i];
        if (sy >= e->y0 && sy < e->y1) {
          float t = (sy - e->y0) / (e->y1 - e->y0);
          s->cross[ncr++] = (lot_cross){e->x0 + (e->x1 - e->x0) * t, e->dir};
        }
      }
      if (ncr < 2) continue;
      // Insertion sort crossings by x (tiny ncr per scanline).
      for (size_t i = 1; i < ncr; i++) {
        lot_cross key = s->cross[i];
        size_t j = i;
        while (j > 0 && s->cross[j - 1u].x > key.x) {
          s->cross[j] = s->cross[j - 1u];
          j--;
        }
        s->cross[j] = key;
      }
      int wind = 0;
      for (size_t i = 0; i + 1u < ncr; i++) {
        wind += s->cross[i].dir;
        bool inside = even_odd ? ((wind & 1) != 0) : (wind != 0);
        if (!inside) continue;
        float xa = s->cross[i].x, xb = s->cross[i + 1u].x;
        if (xb <= 0.0f || xa >= (float)w || xb <= xa) continue;
        if (xa < 0.0f) xa = 0.0f;
        if (xb > (float)w) xb = (float)w;
        int ix0 = (int)floorf(xa);
        int ix1 = (int)floorf(xb);
        if (ix0 < row_x0) row_x0 = ix0;
        if (ix1 + 1 > row_x1) row_x1 = ix1 + 1;
        if (ix0 == ix1) {
          if (ix0 < w) s->cov[ix0] += (uint16_t)((xb - xa) * 64.0f + 0.5f);
        } else {
          if (ix0 < w) s->cov[ix0] += (uint16_t)((1.0f - (xa - (float)ix0)) * 64.0f + 0.5f);
          for (int x = ix0 + 1; x < ix1 && x < w; x++) s->cov[x] += 64u;
          if (ix1 < w) s->cov[ix1] += (uint16_t)((xb - (float)ix1) * 64.0f + 0.5f);
        }
      }
    }
    if (row_x0 >= row_x1) continue;
    if (row_x1 > w) row_x1 = w;
    if (alpha_dst) {
      uint8_t *arow = alpha_dst + (size_t)py * (size_t)w;
      for (int x = row_x0; x < row_x1; x++) {
        uint16_t c = s->cov[x];
        if (c == 0) continue;
        if (c > 256u) c = 256u;
        uint32_t a = (uint32_t)(c * solid_a * (255.0f / 256.0f) + 0.5f);
        if (a > 255u) a = 255u;
        arow[x] = (uint8_t)a;
      }
    } else if (!pt->is_grad) {
      uint8_t *drow = dst + (size_t)py * (size_t)w * 4u;
      for (int x = row_x0; x < row_x1; x++) {
        uint16_t c = s->cov[x];
        if (c == 0) continue;
        if (c > 256u) c = 256u;
        float fa = ((float)c / 256.0f) * solid_a;
        uint8_t pa = (uint8_t)(fa * 255.0f + 0.5f);
        uint8_t pr = (uint8_t)(solid_r * fa * 255.0f + 0.5f);
        uint8_t pg = (uint8_t)(solid_g * fa * 255.0f + 0.5f);
        uint8_t pb = (uint8_t)(solid_b * fa * 255.0f + 0.5f);
        blend_premul_pixel(drow + (size_t)x * 4u, pr, pg, pb, pa);
      }
    } else {
      uint8_t *drow = dst + (size_t)py * (size_t)w * 4u;
      for (int x = row_x0; x < row_x1; x++) {
        uint16_t c = s->cov[x];
        if (c == 0) continue;
        if (c > 256u) c = 256u;
        float gr, gg, gb, ga;
        sample_gradient(pt, (float)x + 0.5f, (float)py + 0.5f, &gr, &gg, &gb, &ga);
        float fa = ((float)c / 256.0f) * clampf_lot(ga, 0.0f, 1.0f);
        uint8_t pa = (uint8_t)(fa * 255.0f + 0.5f);
        uint8_t pr = (uint8_t)(clampf_lot(gr, 0.0f, 1.0f) * fa * 255.0f + 0.5f);
        uint8_t pg = (uint8_t)(clampf_lot(gg, 0.0f, 1.0f) * fa * 255.0f + 0.5f);
        uint8_t pb = (uint8_t)(clampf_lot(gb, 0.0f, 1.0f) * fa * 255.0f + 0.5f);
        blend_premul_pixel(drow + (size_t)x * 4u, pr, pg, pb, pa);
      }
    }
  }
}

// ---- Bezier shape extraction & geometric primitives ----

static const ccord_json *unwrap_shape_val(const ccord_json *v)
{
  if (!v) return nullptr;
  if (ccord_json_type_of(v) == CCORD_JSON_ARRAY && ccord_json_array_len(v) > 0) {
    return ccord_json_array_at(v, 0);
  }
  return v;
}

static void add_bezier_interp(lot_paths *out, const ccord_json *sa, const ccord_json *sb, float t, lot_mat m)
{
  if (!sa || ccord_json_type_of(sa) != CCORD_JSON_OBJECT) return;
  const ccord_json *va = ccord_json_get(sa, "v");
  const ccord_json *ia = ccord_json_get(sa, "i");
  const ccord_json *oa = ccord_json_get(sa, "o");
  if (!va || ccord_json_type_of(va) != CCORD_JSON_ARRAY) return;
  size_t n = ccord_json_array_len(va);
  if (n == 0) return;
  bool closed = ccord_json_bool(ccord_json_get(sa, "c"), false);

  const ccord_json *vb = sb ? ccord_json_get(sb, "v") : nullptr;
  const ccord_json *ib = sb ? ccord_json_get(sb, "i") : nullptr;
  const ccord_json *ob = sb ? ccord_json_get(sb, "o") : nullptr;
  bool morph = vb && ccord_json_type_of(vb) == CCORD_JSON_ARRAY && ccord_json_array_len(vb) == n && t > 1e-4f;

  size_t sub_idx = 0;
  if (!paths_begin(out, closed, &sub_idx)) return;

  float p0v[2], p0o[2], p1v[2], p1i[2];
  float b0v[2], b0o[2], b1v[2], b1i[2];
  j_read_vec(ccord_json_array_at(va, 0), p0v, 2, nullptr);
  if (morph) {
    j_read_vec(ccord_json_array_at(vb, 0), b0v, 2, nullptr);
    p0v[0] += (b0v[0] - p0v[0]) * t;
    p0v[1] += (b0v[1] - p0v[1]) * t;
  }
  lot_vec2 first_pt = mat_pt(m, p0v[0], p0v[1]);
  (void)paths_push_pt(out, first_pt.x, first_pt.y);

  size_t segs = closed ? n : (n > 0 ? n - 1u : 0u);
  for (size_t k = 0; k < segs; k++) {
    size_t nxt = (k + 1u) % n;
    j_read_vec(ccord_json_array_at(va, k), p0v, 2, nullptr);
    j_read_vec(oa && k < ccord_json_array_len(oa) ? ccord_json_array_at(oa, k) : nullptr, p0o, 2, nullptr);
    j_read_vec(ccord_json_array_at(va, nxt), p1v, 2, nullptr);
    j_read_vec(ia && nxt < ccord_json_array_len(ia) ? ccord_json_array_at(ia, nxt) : nullptr, p1i, 2, nullptr);
    if (morph) {
      j_read_vec(ccord_json_array_at(vb, k), b0v, 2, nullptr);
      j_read_vec(ob && k < ccord_json_array_len(ob) ? ccord_json_array_at(ob, k) : nullptr, b0o, 2, nullptr);
      j_read_vec(ccord_json_array_at(vb, nxt), b1v, 2, nullptr);
      j_read_vec(ib && nxt < ccord_json_array_len(ib) ? ccord_json_array_at(ib, nxt) : nullptr, b1i, 2, nullptr);
      for (int c = 0; c < 2; c++) {
        p0v[c] += (b0v[c] - p0v[c]) * t;
        p0o[c] += (b0o[c] - p0o[c]) * t;
        p1v[c] += (b1v[c] - p1v[c]) * t;
        p1i[c] += (b1i[c] - p1i[c]) * t;
      }
    }
    lot_vec2 q0 = mat_pt(m, p0v[0], p0v[1]);
    lot_vec2 q3 = mat_pt(m, p1v[0], p1v[1]);
    if (fabsf(p0o[0]) + fabsf(p0o[1]) + fabsf(p1i[0]) + fabsf(p1i[1]) < 1e-4f) {
      (void)paths_push_pt(out, q3.x, q3.y);
    } else {
      lot_vec2 q1 = mat_pt(m, p0v[0] + p0o[0], p0v[1] + p0o[1]);
      lot_vec2 q2 = mat_pt(m, p1v[0] + p1i[0], p1v[1] + p1i[1]);
      flatten_cubic(out, q0, q1, q2, q3, 0);
    }
  }
  paths_end(out, sub_idx);
}

static void add_bezier_prop(lot_paths *out, const ccord_json *ks, float frame, lot_mat m)
{
  if (!ks || ccord_json_type_of(ks) != CCORD_JSON_OBJECT) return;
  const ccord_json *k = ccord_json_get(ks, "k");
  if (!k) return;
  if (!j_is_animated(ks)) {
    add_bezier_interp(out, unwrap_shape_val(k), nullptr, 0.0f, m);
    return;
  }
  size_t nk = ccord_json_array_len(k);
  if (nk == 0) return;
  const ccord_json *kf0 = ccord_json_array_at(k, 0);
  if (frame <= j_get_num(kf0, "t", 0.0f)) {
    add_bezier_interp(out, unwrap_shape_val(ccord_json_get(kf0, "s")), nullptr, 0.0f, m);
    return;
  }
  for (size_t i = 0; i + 1u < nk; i++) {
    const ccord_json *ka = ccord_json_array_at(k, i);
    const ccord_json *kb = ccord_json_array_at(k, i + 1u);
    float ta = j_get_num(ka, "t", 0.0f);
    float tb = j_get_num(kb, "t", ta);
    if (frame < tb || i + 2u == nk) {
      if (frame >= tb) {
        const ccord_json *sv = unwrap_shape_val(ccord_json_get(kb, "s"));
        if (!sv) sv = unwrap_shape_val(ccord_json_get(ka, "e"));
        if (!sv) sv = unwrap_shape_val(ccord_json_get(ka, "s"));
        add_bezier_interp(out, sv, nullptr, 0.0f, m);
        return;
      }
      const ccord_json *sa = unwrap_shape_val(ccord_json_get(ka, "s"));
      if (j_get_num(ka, "h", 0.0f) >= 0.5f || tb <= ta) {
        add_bezier_interp(out, sa, nullptr, 0.0f, m);
        return;
      }
      const ccord_json *sb = unwrap_shape_val(ccord_json_get(ka, "e"));
      if (!sb) sb = unwrap_shape_val(ccord_json_get(kb, "s"));
      float u = clampf_lot((frame - ta) / (tb - ta), 0.0f, 1.0f);
      const ccord_json *oo = ccord_json_get(ka, "o");
      const ccord_json *ii = ccord_json_get(ka, "i");
      float t = cubic_ease(u, j_axis(oo, "x", 0, 0.0f), j_axis(oo, "y", 0, 0.0f),
                           j_axis(ii, "x", 0, 1.0f), j_axis(ii, "y", 0, 1.0f));
      add_bezier_interp(out, sa, sb, t, m);
      return;
    }
  }
  const ccord_json *last = ccord_json_array_at(k, nk - 1u);
  add_bezier_interp(out, unwrap_shape_val(ccord_json_get(last, "s")), nullptr, 0.0f, m);
}

static void add_ellipse(lot_paths *out, const ccord_json *sh, float frame, lot_mat m)
{
  float p[2] = {0}, sz[2] = {0};
  eval_prop_vec(ccord_json_get(sh, "p"), frame, p, 2, nullptr);
  eval_prop_vec(ccord_json_get(sh, "s"), frame, sz, 2, nullptr);
  float rx = sz[0] * 0.5f, ry = sz[1] * 0.5f;
  if (fabsf(rx) < 1e-4f || fabsf(ry) < 1e-4f) return;
  bool ccw = j_get_num(sh, "d", 1.0f) == 3.0f;
  size_t sub_idx = 0;
  if (!paths_begin(out, true, &sub_idx)) return;
  enum { SEGS = 32 };
  for (int i = 0; i <= SEGS; i++) {
    float a = (float)i * (2.0f * LOT_PI / (float)SEGS) * (ccw ? -1.0f : 1.0f) - LOT_PI * 0.5f;
    lot_vec2 pt = mat_pt(m, p[0] + cosf(a) * rx, p[1] + sinf(a) * ry);
    (void)paths_push_pt(out, pt.x, pt.y);
  }
  paths_end(out, sub_idx);
}

static void add_rect(lot_paths *out, const ccord_json *sh, float frame, lot_mat m)
{
  float p[2] = {0}, sz[2] = {0};
  eval_prop_vec(ccord_json_get(sh, "p"), frame, p, 2, nullptr);
  eval_prop_vec(ccord_json_get(sh, "s"), frame, sz, 2, nullptr);
  float rad = eval_prop_scalar(ccord_json_get(sh, "r"), frame, 0.0f);
  float hw = sz[0] * 0.5f, hh = sz[1] * 0.5f;
  if (hw <= 0.0f || hh <= 0.0f) return;
  rad = clampf_lot(rad, 0.0f, hw < hh ? hw : hh);
  size_t sub_idx = 0;
  if (!paths_begin(out, true, &sub_idx)) return;
  float x0 = p[0] - hw, y0 = p[1] - hh, x1 = p[0] + hw, y1 = p[1] + hh;
  if (rad < 0.25f) {
    lot_vec2 cs[5] = {mat_pt(m, x1, y0), mat_pt(m, x1, y1), mat_pt(m, x0, y1), mat_pt(m, x0, y0), mat_pt(m, x1, y0)};
    for (int i = 0; i < 5; i++) (void)paths_push_pt(out, cs[i].x, cs[i].y);
  } else {
    float cx[4] = {x1 - rad, x1 - rad, x0 + rad, x0 + rad};
    float cy[4] = {y0 + rad, y1 - rad, y1 - rad, y0 + rad};
    float a0[4] = {-LOT_PI * 0.5f, 0.0f, LOT_PI * 0.5f, LOT_PI};
    for (int c = 0; c < 4; c++) {
      for (int s = 0; s <= 4; s++) {
        float ang = a0[c] + (float)s * (LOT_PI * 0.5f / 4.0f);
        lot_vec2 pt = mat_pt(m, cx[c] + cosf(ang) * rad, cy[c] + sinf(ang) * rad);
        (void)paths_push_pt(out, pt.x, pt.y);
      }
    }
  }
  paths_end(out, sub_idx);
}

static void add_polystar(lot_paths *out, const ccord_json *sh, float frame, lot_mat m)
{
  int sy = (int)j_get_num(sh, "sy", 1.0f);   // 1 = star, 2 = polygon
  int npts = (int)floorf(eval_prop_scalar(ccord_json_get(sh, "pt"), frame, 5.0f));
  if (npts < 3 || npts > 64) return;
  float p[2] = {0};
  eval_prop_vec(ccord_json_get(sh, "p"), frame, p, 2, nullptr);
  float rot = (eval_prop_scalar(ccord_json_get(sh, "r"), frame, 0.0f) - 90.0f) * LOT_DEG2RAD;
  float orad = eval_prop_scalar(ccord_json_get(sh, "or"), frame, 50.0f);
  float irad = eval_prop_scalar(ccord_json_get(sh, "ir"), frame, orad * 0.5f);
  size_t sub_idx = 0;
  if (!paths_begin(out, true, &sub_idx)) return;
  int total = sy == 1 ? npts * 2 : npts;
  for (int i = 0; i <= total; i++) {
    float a = rot + (float)i * (2.0f * LOT_PI / (float)total);
    float rad = (sy == 1 && (i & 1)) ? irad : orad;
    lot_vec2 pt = mat_pt(m, p[0] + cosf(a) * rad, p[1] + sinf(a) * rad);
    (void)paths_push_pt(out, pt.x, pt.y);
  }
  paths_end(out, sub_idx);
}

// ---- trim paths ----

static float subpath_length(const lot_paths *p, const lot_subpath *sub)
{
  if (sub->count < 2) return 0.0f;
  float total = 0.0f;
  for (size_t i = 0; i + 1u < sub->count; i++) {
    lot_vec2 a = p->pts[sub->start + i], b = p->pts[sub->start + i + 1u];
    total += hypotf(b.x - a.x, b.y - a.y);
  }
  return total;
}

static void emit_trimmed_slice(lot_paths *dst, const lot_paths *src, const lot_subpath *sub, float d0, float d1)
{
  if (d1 <= d0 + 1e-4f || sub->count < 2) return;
  size_t idx = 0;
  if (!paths_begin(dst, false, &idx)) return;
  float acc = 0.0f;
  bool started = false;
  for (size_t i = 0; i + 1u < sub->count; i++) {
    lot_vec2 a = src->pts[sub->start + i], b = src->pts[sub->start + i + 1u];
    float seg = hypotf(b.x - a.x, b.y - a.y);
    if (seg < 1e-5f) continue;
    float s0 = acc, s1 = acc + seg;
    acc = s1;
    if (s1 < d0 || s0 > d1) continue;
    float u0 = d0 > s0 ? (d0 - s0) / seg : 0.0f;
    float u1 = d1 < s1 ? (d1 - s0) / seg : 1.0f;
    if (!started) {
      (void)paths_push_pt(dst, a.x + (b.x - a.x) * u0, a.y + (b.y - a.y) * u0);
      started = true;
    }
    (void)paths_push_pt(dst, a.x + (b.x - a.x) * u1, a.y + (b.y - a.y) * u1);
  }
  paths_end(dst, idx);
}

static void apply_trim_paths(const lot_paths *src, const lot_trim *tr, lot_paths *dst)
{
  float s = tr->s * 0.01f, e = tr->e * 0.01f;
  if (s > e) {
    float tmp = s;
    s = e;
    e = tmp;
  }
  float span = e - s;
  if (span <= 1e-5f) return;
  if (span >= 1.0f - 1e-5f) {
    for (size_t i = 0; i < src->nsubs; i++) {
      size_t idx = 0;
      if (!paths_begin(dst, src->subs[i].closed, &idx)) break;
      for (size_t k = 0; k < src->subs[i].count; k++) {
        lot_vec2 pt = src->pts[src->subs[i].start + k];
        (void)paths_push_pt(dst, pt.x, pt.y);
      }
      paths_end(dst, idx);
    }
    return;
  }
  float off = fmodf(tr->o / 360.0f, 1.0f);
  if (off < 0.0f) off += 1.0f;
  float u0 = fmodf(s + off, 1.0f);
  if (u0 < 0.0f) u0 += 1.0f;
  float u1 = u0 + span;

  if (tr->mode == 2) {
    float total = 0.0f;
    for (size_t i = 0; i < src->nsubs; i++) total += subpath_length(src, &src->subs[i]);
    if (total <= 1e-4f) return;
    float ranges[2][2];
    int nr = 1;
    if (u1 <= 1.0f) {
      ranges[0][0] = u0 * total;
      ranges[0][1] = u1 * total;
    } else {
      ranges[0][0] = u0 * total;
      ranges[0][1] = total;
      ranges[1][0] = 0.0f;
      ranges[1][1] = (u1 - 1.0f) * total;
      nr = 2;
    }
    for (int ri = 0; ri < nr; ri++) {
      float acc = 0.0f;
      for (size_t i = 0; i < src->nsubs; i++) {
        float sl = subpath_length(src, &src->subs[i]);
        float lo = ranges[ri][0] - acc, hi = ranges[ri][1] - acc;
        if (hi > 0.0f && lo < sl) {
          emit_trimmed_slice(dst, src, &src->subs[i], lo > 0.0f ? lo : 0.0f, hi < sl ? hi : sl);
        }
        acc += sl;
      }
    }
  } else {
    for (size_t i = 0; i < src->nsubs; i++) {
      float sl = subpath_length(src, &src->subs[i]);
      if (sl <= 1e-4f) continue;
      if (u1 <= 1.0f) {
        emit_trimmed_slice(dst, src, &src->subs[i], u0 * sl, u1 * sl);
      } else {
        emit_trimmed_slice(dst, src, &src->subs[i], u0 * sl, sl);
        emit_trimmed_slice(dst, src, &src->subs[i], 0.0f, (u1 - 1.0f) * sl);
      }
    }
  }
}

// ---- stroke expansion (nonzero winding union of oriented quads + caps + joins) ----

static void emit_stroke_subpath(ccord_lottie_stream *s, const lot_vec2 *pts, size_t n, bool closed,
                                float hw, int lc, int lj, float ml)
{
  if (n < 2 || hw <= 0.05f) return;
  // Filter zero-length consecutive points into a clean segment list.
  lot_vec2 prev_n = {0, 0};
  lot_vec2 first_n = {0, 0};
  bool have_prev = false;
  size_t seg_count = closed ? n : n - 1u;
  size_t actual_segs = 0;
  for (size_t i = 0; i < seg_count; i++) {
    lot_vec2 a = pts[i];
    lot_vec2 b = pts[(i + 1u) % n];
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = hypotf(dx, dy);
    if (len < 1e-4f) continue;
    float ux = dx / len, uy = dy / len;
    float nx = -uy * hw, ny = ux * hw;

    // Extend start/end for square caps on open paths.
    lot_vec2 ea = a, eb = b;
    if (!closed && lc == 3) {
      if (actual_segs == 0) {
        ea.x -= ux * hw;
        ea.y -= uy * hw;
      }
      if (i + 1u == seg_count) {
        eb.x += ux * hw;
        eb.y += uy * hw;
      }
    }
    lot_vec2 quad[4] = {
      {ea.x + nx, ea.y + ny},
      {eb.x + nx, eb.y + ny},
      {eb.x - nx, eb.y - ny},
      {ea.x - nx, ea.y - ny},
    };
    add_poly_edges_cw(s, quad, 4);

    if (!closed && lc == 2) {
      if (actual_segs == 0) add_circle_edges(s, a.x, a.y, hw);
      if (i + 1u == seg_count) add_circle_edges(s, b.x, b.y, hw);
    }

    if (have_prev) {
      if (lj == 2) {
        add_circle_edges(s, a.x, a.y, hw);
      } else {
        // Outer join side determined by cross product of previous and current normals.
        float cross = prev_n.x * ny - prev_n.y * nx;
        float sign = cross >= 0.0f ? 1.0f : -1.0f;
        lot_vec2 j1 = {a.x + prev_n.x * sign, a.y + prev_n.y * sign};
        lot_vec2 j2 = {a.x + nx * sign, a.y + ny * sign};
        bool did_miter = false;
        if (lj == 1) {
          float mx = prev_n.x + nx, my = prev_n.y + ny;
          float mlen2 = mx * mx + my * my;
          if (mlen2 > 1e-4f) {
            float k = (2.0f * hw * hw) / mlen2;
            float mdist = sqrtf(k * k * mlen2);
            if (mdist <= ml * hw) {
              lot_vec2 tip = {a.x + mx * sign * k, a.y + my * sign * k};
              lot_vec2 mpoly[4] = {a, j1, tip, j2};
              add_poly_edges_cw(s, mpoly, 4);
              did_miter = true;
            }
          }
        }
        if (!did_miter) {
          lot_vec2 btri[3] = {a, j1, j2};
          add_poly_edges_cw(s, btri, 3);
        }
      }
    } else {
      first_n = (lot_vec2){nx, ny};
    }
    prev_n = (lot_vec2){nx, ny};
    have_prev = true;
    actual_segs++;
  }
  // Closing join on closed paths.
  if (closed && actual_segs >= 2) {
    lot_vec2 a = pts[0];
    if (lj == 2) {
      add_circle_edges(s, a.x, a.y, hw);
    } else {
      float cross = prev_n.x * first_n.y - prev_n.y * first_n.x;
      float sign = cross >= 0.0f ? 1.0f : -1.0f;
      lot_vec2 j1 = {a.x + prev_n.x * sign, a.y + prev_n.y * sign};
      lot_vec2 j2 = {a.x + first_n.x * sign, a.y + first_n.y * sign};
      lot_vec2 btri[3] = {a, j1, j2};
      add_poly_edges_cw(s, btri, 3);
    }
  }
}

// ---- paint parsing ----

static void parse_color(const ccord_json *prop, float frame, float *r, float *g, float *b)
{
  float c[3] = {0, 0, 0};
  eval_prop_vec(prop, frame, c, 3, nullptr);
  if (c[0] > 1.001f || c[1] > 1.001f || c[2] > 1.001f) {
    c[0] /= 255.0f;
    c[1] /= 255.0f;
    c[2] /= 255.0f;
  }
  *r = clampf_lot(c[0], 0.0f, 1.0f);
  *g = clampf_lot(c[1], 0.0f, 1.0f);
  *b = clampf_lot(c[2], 0.0f, 1.0f);
}

static void parse_gradient(const ccord_json *item, float frame, lot_mat m, float opacity, lot_paint *out)
{
  memset(out, 0, sizeof *out);
  out->is_grad = true;
  out->grad_type = (int)j_get_num(item, "t", 1.0f);
  out->a = clampf_lot(eval_prop_scalar(ccord_json_get(item, "o"), frame, 100.0f) * 0.01f * opacity, 0.0f, 1.0f);
  out->fill_rule = (int)j_get_num(item, "r", 1.0f);
  float sp[2] = {0}, ep[2] = {0};
  eval_prop_vec(ccord_json_get(item, "s"), frame, sp, 2, nullptr);
  eval_prop_vec(ccord_json_get(item, "e"), frame, ep, 2, nullptr);
  out->gs = mat_pt(m, sp[0], sp[1]);
  out->ge = mat_pt(m, ep[0], ep[1]);

  const ccord_json *g = ccord_json_get(item, "g");
  if (!g) return;
  int pcount = (int)j_get_num(g, "p", 0.0f);
  if (pcount <= 0) return;
  if (pcount > 32) pcount = 32;
  const ccord_json *gk = ccord_json_get(g, "k");
  if (!gk) return;
  float vals[32 * 6];
  size_t need = (size_t)pcount * 6u;
  // Determine actual array length in k (static or first keyframe).
  const ccord_json *raw_k = ccord_json_get(gk, "k");
  size_t raw_len = 0;
  if (raw_k && ccord_json_type_of(raw_k) == CCORD_JSON_ARRAY) {
    if (j_is_animated(gk) && ccord_json_array_len(raw_k) > 0) {
      const ccord_json *s0 = ccord_json_get(ccord_json_array_at(raw_k, 0), "s");
      if (s0 && ccord_json_type_of(s0) == CCORD_JSON_ARRAY) raw_len = ccord_json_array_len(s0);
    } else {
      raw_len = ccord_json_array_len(raw_k);
    }
  }
  if (raw_len > need) raw_len = need;
  if (raw_len < (size_t)pcount * 4u) return;
  eval_prop_vec(gk, frame, vals, raw_len, nullptr);
  for (int i = 0; i < pcount; i++) {
    out->stops[i] = (lot_grad_stop){
      .pos = clampf_lot(vals[i * 4 + 0], 0.0f, 1.0f),
      .r = clampf_lot(vals[i * 4 + 1], 0.0f, 1.0f),
      .g = clampf_lot(vals[i * 4 + 2], 0.0f, 1.0f),
      .b = clampf_lot(vals[i * 4 + 3], 0.0f, 1.0f),
      .a = 1.0f,
    };
  }
  out->nstops = (size_t)pcount;
  // Optional alpha stops follow color stops as pairs (pos, alpha).
  size_t a_off = (size_t)pcount * 4u;
  if (raw_len >= a_off + 2u) {
    size_t na = (raw_len - a_off) / 2u;
    for (int i = 0; i < pcount; i++) {
      float pos = out->stops[i].pos;
      float alpha = clampf_lot(vals[a_off + 1u], 0.0f, 1.0f);
      for (size_t j = 0; j + 1u < na; j++) {
        float pa = vals[a_off + j * 2u], aa = vals[a_off + j * 2u + 1u];
        float pb = vals[a_off + (j + 1u) * 2u], ab = vals[a_off + (j + 1u) * 2u + 1u];
        if (pos <= pa) {
          alpha = aa;
          break;
        }
        if (pos <= pb) {
          float f = pb > pa ? (pos - pa) / (pb - pa) : 0.0f;
          alpha = aa + (ab - aa) * f;
          break;
        }
        alpha = ab;
      }
      out->stops[i].a = clampf_lot(alpha, 0.0f, 1.0f);
    }
  }
}

// ---- shape group & layer rendering ----

static void collect_geometry(const ccord_json *items, size_t limit, float frame, lot_mat m, lot_paths *out, int depth)
{
  if (!items || ccord_json_type_of(items) != CCORD_JSON_ARRAY || depth > LOT_MAX_DEPTH) return;
  size_t n = ccord_json_array_len(items);
  if (limit < n) n = limit;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *it = ccord_json_array_at(items, i);
    if (!it || ccord_json_bool(ccord_json_get(it, "hd"), false)) continue;
    const char *ty = j_get_str(it, "ty");
    if (!ty) continue;
    if (strcmp(ty, "sh") == 0) {
      add_bezier_prop(out, ccord_json_get(it, "ks"), frame, m);
    } else if (strcmp(ty, "el") == 0) {
      add_ellipse(out, it, frame, m);
    } else if (strcmp(ty, "rc") == 0) {
      add_rect(out, it, frame, m);
    } else if (strcmp(ty, "sr") == 0) {
      add_polystar(out, it, frame, m);
    } else if (strcmp(ty, "gr") == 0) {
      const ccord_json *sub = ccord_json_get(it, "it");
      if (!sub || ccord_json_type_of(sub) != CCORD_JSON_ARRAY) continue;
      lot_mat gm = m;
      size_t sn = ccord_json_array_len(sub);
      for (size_t k = sn; k > 0; k--) {
        const ccord_json *si = ccord_json_array_at(sub, k - 1u);
        const char *sty = j_get_str(si, "ty");
        if (sty && strcmp(sty, "tr") == 0) {
          lot_mat trm;
          eval_transform(si, frame, &trm, nullptr);
          gm = mat_mul(m, trm);
          break;
        }
      }
      collect_geometry(sub, sn, frame, gm, out, depth + 1);
    }
  }
}

static void render_shape_items(ccord_lottie_stream *s, uint8_t *dst, const ccord_json *items,
                               float frame, lot_mat parent_m, float parent_op, lot_trim inherited_trim, int depth)
{
  if (!items || ccord_json_type_of(items) != CCORD_JSON_ARRAY || depth > LOT_MAX_DEPTH) return;
  size_t n = ccord_json_array_len(items);
  if (n == 0) return;

  // Apply this group's transform `tr` (if present).
  lot_mat m = parent_m;
  float op = parent_op;
  lot_trim trim = inherited_trim;
  const ccord_json *repeater = nullptr;
  for (size_t i = n; i > 0; i--) {
    const ccord_json *it = ccord_json_array_at(items, i - 1u);
    if (!it || ccord_json_bool(ccord_json_get(it, "hd"), false)) continue;
    const char *ty = j_get_str(it, "ty");
    if (!ty) continue;
    if (strcmp(ty, "tr") == 0) {
      lot_mat trm;
      float trop = 1.0f;
      eval_transform(it, frame, &trm, &trop);
      m = mat_mul(parent_m, trm);
      op = parent_op * trop;
    } else if (strcmp(ty, "tm") == 0) {
      trim.active = true;
      trim.s = eval_prop_scalar(ccord_json_get(it, "s"), frame, 0.0f);
      trim.e = eval_prop_scalar(ccord_json_get(it, "e"), frame, 100.0f);
      trim.o = eval_prop_scalar(ccord_json_get(it, "o"), frame, 0.0f);
      trim.mode = (int)j_get_num(it, "m", 1.0f);
    } else if (strcmp(ty, "rp") == 0) {
      repeater = it;
    }
  }
  if (op <= 1e-4f) return;

  // Repeater: invoke the rest of this group N times with cumulative transform & interpolated opacity.
  int copies = 1;
  float rp_off = 0.0f, rp_so = 1.0f, rp_eo = 1.0f;
  const ccord_json *rp_tr = nullptr;
  if (repeater) {
    copies = (int)floorf(eval_prop_scalar(ccord_json_get(repeater, "c"), frame, 1.0f));
    if (copies < 0) copies = 0;
    if (copies > 32) copies = 32;
    rp_off = eval_prop_scalar(ccord_json_get(repeater, "o"), frame, 0.0f);
    rp_tr = ccord_json_get(repeater, "tr");
    if (rp_tr) {
      rp_so = clampf_lot(eval_prop_scalar(ccord_json_get(rp_tr, "so"), frame, 100.0f) * 0.01f, 0.0f, 1.0f);
      rp_eo = clampf_lot(eval_prop_scalar(ccord_json_get(rp_tr, "eo"), frame, 100.0f) * 0.01f, 0.0f, 1.0f);
    }
  }

  for (int ci = copies - 1; ci >= 0; ci--) {
    lot_mat cm = m;
    float cop = op;
    if (repeater && rp_tr) {
      float idx_f = (float)ci + rp_off;
      float def0[2] = {0, 0}, def100[2] = {100, 100};
      float a[2], p[2], sc[2];
      eval_prop_vec(ccord_json_get(rp_tr, "a"), frame, a, 2, def0);
      eval_prop_vec(ccord_json_get(rp_tr, "p"), frame, p, 2, def0);
      eval_prop_vec(ccord_json_get(rp_tr, "s"), frame, sc, 2, def100);
      float rdeg = eval_prop_scalar(ccord_json_get(rp_tr, "r"), frame, 0.0f);
      lot_mat rm = {1.0f, 0.0f, 0.0f, 1.0f, p[0] * idx_f, p[1] * idx_f};
      float rad = rdeg * idx_f * LOT_DEG2RAD;
      if (fabsf(rad) > 1e-4f) {
        float cs = cosf(rad), sn = sinf(rad);
        rm = mat_mul(rm, (lot_mat){cs, sn, -sn, cs, 0.0f, 0.0f});
      }
      float sx = powf(sc[0] * 0.01f, idx_f), sy = powf(sc[1] * 0.01f, idx_f);
      rm = mat_mul(rm, (lot_mat){sx, 0.0f, 0.0f, sy, 0.0f, 0.0f});
      rm = mat_mul(rm, (lot_mat){1.0f, 0.0f, 0.0f, 1.0f, -a[0], -a[1]});
      cm = mat_mul(m, rm);
      float frac = copies > 1 ? (float)ci / (float)(copies - 1) : 0.0f;
      cop = op * (rp_so + (rp_eo - rp_so) * frac);
    }

    // Draw items back-to-front (index n-1 down to 0).
    for (size_t i = n; i > 0; i--) {
      size_t idx = i - 1u;
      const ccord_json *it = ccord_json_array_at(items, idx);
      if (!it || ccord_json_bool(ccord_json_get(it, "hd"), false)) continue;
      const char *ty = j_get_str(it, "ty");
      if (!ty) continue;
      if (strcmp(ty, "gr") == 0) {
        render_shape_items(s, dst, ccord_json_get(it, "it"), frame, cm, cop, trim, depth + 1);
      } else if (strcmp(ty, "fl") == 0 || strcmp(ty, "gf") == 0 || strcmp(ty, "st") == 0 || strcmp(ty, "gs") == 0) {
        bool is_stroke = ty[0] == 's' || ty[1] == 's';
        lot_paint pt = {0};
        if (ty[0] == 'g') {
          parse_gradient(it, frame, cm, cop, &pt);
        } else {
          parse_color(ccord_json_get(it, "c"), frame, &pt.r, &pt.g, &pt.b);
          pt.a = clampf_lot(eval_prop_scalar(ccord_json_get(it, "o"), frame, 100.0f) * 0.01f * cop, 0.0f, 1.0f);
          pt.fill_rule = (int)j_get_num(it, "r", 1.0f);
        }
        if (pt.a <= 1e-4f) continue;

        lot_paths raw = {0}, trimmed = {0};
        collect_geometry(items, idx, frame, cm, &raw, depth + 1);
        const lot_paths *use = &raw;
        if (trim.active && raw.nsubs > 0) {
          apply_trim_paths(&raw, &trim, &trimmed);
          use = &trimmed;
        }
        s->nedges = 0;
        if (is_stroke) {
          float sw = eval_prop_scalar(ccord_json_get(it, "w"), frame, 1.0f) * mat_avg_scale(cm);
          int lc = (int)j_get_num(it, "lc", 1.0f);
          int lj = (int)j_get_num(it, "lj", 1.0f);
          float ml = j_get_num(it, "ml", 4.0f);
          if (ml < 1.0f) ml = 4.0f;
          pt.fill_rule = 1;   // strokes union cleanly under nonzero winding
          for (size_t si = 0; si < use->nsubs; si++) {
            emit_stroke_subpath(s, use->pts + use->subs[si].start, use->subs[si].count,
                                use->subs[si].closed, sw * 0.5f, lc, lj, ml);
          }
        } else {
          for (size_t si = 0; si < use->nsubs; si++) {
            add_poly_edges(s, use->pts + use->subs[si].start, use->subs[si].count);
          }
        }
        rasterize_edges(s, dst, nullptr, &pt);
        paths_free(&trimmed);
        paths_free(&raw);
      }
    }
  }
}

static const ccord_json *find_layer_by_ind(const ccord_json *layers, int64_t ind)
{
  size_t n = ccord_json_array_len(layers);
  for (size_t i = 0; i < n; i++) {
    const ccord_json *l = ccord_json_array_at(layers, i);
    int64_t lid = 0;
    if (l && ccord_json_int64(ccord_json_get(l, "ind"), &lid) && lid == ind) return l;
  }
  return nullptr;
}

static lot_mat layer_world_matrix(const ccord_json *layers, const ccord_json *layer, float frame, lot_mat comp_m,
                                  float *out_op, int depth)
{
  lot_mat local;
  eval_transform(ccord_json_get(layer, "ks"), frame, &local, out_op);
  if (depth < LOT_MAX_PARENT_DEPTH) {
    int64_t pid = 0;
    if (ccord_json_int64(ccord_json_get(layer, "parent"), &pid)) {
      const ccord_json *parent = find_layer_by_ind(layers, pid);
      if (parent && parent != layer) {
        lot_mat pm = layer_world_matrix(layers, parent, frame, comp_m, nullptr, depth + 1);
        return mat_mul(pm, local);
      }
    }
  }
  return mat_mul(comp_m, local);
}

static const ccord_json *find_precomp_layers(const ccord_json *root, const char *ref_id)
{
  if (!ref_id || !ref_id[0]) return nullptr;
  const ccord_json *assets = ccord_json_get(root, "assets");
  if (!assets || ccord_json_type_of(assets) != CCORD_JSON_ARRAY) return nullptr;
  for (size_t i = 0, n = ccord_json_array_len(assets); i < n; i++) {
    const ccord_json *a = ccord_json_array_at(assets, i);
    const char *id = j_get_str(a, "id");
    if (id && strcmp(id, ref_id) == 0) return ccord_json_get(a, "layers");
  }
  return nullptr;
}

static bool parse_hex_color(const char *hex, float *r, float *g, float *b)
{
  if (!hex) return false;
  if (*hex == '#') hex++;
  if (strlen(hex) < 6) return false;
  char *end = nullptr;
  unsigned long v = strtoul(hex, &end, 16);
  if (!end || end < hex + 6) return false;
  *r = (float)((v >> 16) & 0xFFu) / 255.0f;
  *g = (float)((v >> 8) & 0xFFu) / 255.0f;
  *b = (float)(v & 0xFFu) / 255.0f;
  return true;
}

static void render_layers(ccord_lottie_stream *s, uint8_t *dst, const ccord_json *layers,
                          float frame, lot_mat comp_m, float comp_op, int depth);

static void composite_premul(uint8_t *dst, const uint8_t *src, size_t npixels, const uint8_t *mask, bool inv_mask)
{
  for (size_t i = 0; i < npixels; i++) {
    uint8_t sa = src[i * 4u + 3u];
    if (sa == 0) continue;
    uint8_t sr = src[i * 4u + 0u];
    uint8_t sg = src[i * 4u + 1u];
    uint8_t sb = src[i * 4u + 2u];
    if (mask) {
      uint8_t m = inv_mask ? (uint8_t)(255u - mask[i]) : mask[i];
      if (m == 0) continue;
      if (m < 255u) {
        sr = (uint8_t)(((uint32_t)sr * m + 127u) / 255u);
        sg = (uint8_t)(((uint32_t)sg * m + 127u) / 255u);
        sb = (uint8_t)(((uint32_t)sb * m + 127u) / 255u);
        sa = (uint8_t)(((uint32_t)sa * m + 127u) / 255u);
      }
    }
    blend_premul_pixel(dst + i * 4u, sr, sg, sb, sa);
  }
}

static void render_single_layer(ccord_lottie_stream *s, uint8_t *dst, const ccord_json *layers,
                                const ccord_json *layer, float frame, lot_mat comp_m, float comp_op, int depth)
{
  float ip = j_get_num(layer, "ip", 0.0f);
  float op_f = j_get_num(layer, "op", s->op);
  if (frame < ip || frame >= op_f) return;

  int ty = (int)j_get_num(layer, "ty", -1.0f);
  if (ty != 0 && ty != 1 && ty != 4) return;   // null (3) and non-vector layers don't paint

  float lop = 1.0f;
  lot_mat wm = layer_world_matrix(layers, layer, frame, comp_m, &lop, 0);
  float total_op = comp_op * lop;
  if (total_op <= 1e-4f) return;

  const ccord_json *masks = ccord_json_get(layer, "masksProperties");
  bool has_masks = masks && ccord_json_type_of(masks) == CCORD_JSON_ARRAY && ccord_json_array_len(masks) > 0;
  size_t npix = (size_t)s->out_w * (size_t)s->out_h;
  uint8_t *target = dst;
  uint8_t *layer_buf = nullptr;
  if (has_masks) {
    layer_buf = ccord_calloc(npix * 4u, 1);
    if (!layer_buf) return;
    target = layer_buf;
  }

  if (ty == 4) {
    lot_trim no_trim = {0};
    render_shape_items(s, target, ccord_json_get(layer, "shapes"), frame, wm, total_op, no_trim, depth + 1);
  } else if (ty == 0) {
    const char *ref_id = j_get_str(layer, "refId");
    const ccord_json *sub_layers = find_precomp_layers(s->root, ref_id);
    if (sub_layers) {
      float st = j_get_num(layer, "st", 0.0f);
      float sr = j_get_num(layer, "sr", 1.0f);
      if (fabsf(sr) < 1e-4f) sr = 1.0f;
      const ccord_json *tm = ccord_json_get(layer, "tm");
      float sub_frame = tm ? eval_prop_scalar(tm, frame, 0.0f) * s->fr : (frame - st) / sr;
      render_layers(s, target, sub_layers, sub_frame, wm, total_op, depth + 1);
    }
  } else if (ty == 1) {
    float sw = j_get_num(layer, "sw", 0.0f), sh = j_get_num(layer, "sh", 0.0f);
    lot_paint pt = {.a = total_op, .fill_rule = 1};
    if (sw > 0.0f && sh > 0.0f && parse_hex_color(j_get_str(layer, "sc"), &pt.r, &pt.g, &pt.b)) {
      lot_vec2 q[4] = {mat_pt(wm, 0, 0), mat_pt(wm, sw, 0), mat_pt(wm, sw, sh), mat_pt(wm, 0, sh)};
      s->nedges = 0;
      add_poly_edges(s, q, 4);
      rasterize_edges(s, target, nullptr, &pt);
    }
  }

  if (has_masks) {
    uint8_t *mask_acc = ccord_calloc(npix, 1);
    uint8_t *mask_tmp = ccord_malloc(npix);
    bool any_mask = false;
    if (mask_acc && mask_tmp) {
      size_t nm = ccord_json_array_len(masks);
      for (size_t mi = 0; mi < nm; mi++) {
        const ccord_json *mk = ccord_json_array_at(masks, mi);
        if (!mk) continue;
        const char *mode = j_get_str(mk, "mode");
        if (mode && strcmp(mode, "n") == 0) continue;
        bool inv = ccord_json_bool(ccord_json_get(mk, "inv"), false);
        float mop = clampf_lot(eval_prop_scalar(ccord_json_get(mk, "o"), frame, 100.0f) * 0.01f, 0.0f, 1.0f);
        lot_paths mp = {0};
        add_bezier_prop(&mp, ccord_json_get(mk, "pt"), frame, wm);
        memset(mask_tmp, 0, npix);
        s->nedges = 0;
        for (size_t si = 0; si < mp.nsubs; si++) add_poly_edges(s, mp.pts + mp.subs[si].start, mp.subs[si].count);
        lot_paint mpt = {.a = mop, .fill_rule = 1};
        rasterize_edges(s, nullptr, mask_tmp, &mpt);
        paths_free(&mp);
        if (inv) {
          for (size_t p = 0; p < npix; p++) mask_tmp[p] = (uint8_t)(255u - mask_tmp[p]);
        }
        if (!any_mask) {
          memcpy(mask_acc, mask_tmp, npix);
          any_mask = true;
        } else if (mode && strcmp(mode, "s") == 0) {
          for (size_t p = 0; p < npix; p++) mask_acc[p] = mask_acc[p] > mask_tmp[p] ? (uint8_t)(mask_acc[p] - mask_tmp[p]) : 0u;
        } else if (mode && strcmp(mode, "i") == 0) {
          for (size_t p = 0; p < npix; p++) mask_acc[p] = (uint8_t)(((uint32_t)mask_acc[p] * mask_tmp[p] + 127u) / 255u);
        } else {
          for (size_t p = 0; p < npix; p++) {
            uint32_t sum = (uint32_t)mask_acc[p] + mask_tmp[p];
            mask_acc[p] = sum > 255u ? 255u : (uint8_t)sum;
          }
        }
      }
      composite_premul(dst, layer_buf, npix, any_mask ? mask_acc : nullptr, false);
    }
    ccord_free(mask_tmp);
    ccord_free(mask_acc);
    ccord_free(layer_buf);
  }
}

static void render_layers(ccord_lottie_stream *s, uint8_t *dst, const ccord_json *layers,
                          float frame, lot_mat comp_m, float comp_op, int depth)
{
  if (!layers || ccord_json_type_of(layers) != CCORD_JSON_ARRAY || depth > LOT_MAX_DEPTH) return;
  size_t n = ccord_json_array_len(layers);
  size_t npix = (size_t)s->out_w * (size_t)s->out_h;
  for (size_t i = n; i > 0; i--) {
    size_t idx = i - 1u;
    const ccord_json *layer = ccord_json_array_at(layers, idx);
    if (!layer || ccord_json_bool(ccord_json_get(layer, "hd"), false)) continue;
    if ((int)j_get_num(layer, "td", 0.0f) != 0) continue;   // track matte source: rendered only via its consumer
    int tt = (int)j_get_num(layer, "tt", 0.0f);
    if (tt >= 1 && tt <= 4) {
      const ccord_json *matte_src = nullptr;
      int64_t tp_ind = 0;
      if (ccord_json_int64(ccord_json_get(layer, "tp"), &tp_ind)) {
        matte_src = find_layer_by_ind(layers, tp_ind);
      } else if (idx > 0) {
        matte_src = ccord_json_array_at(layers, idx - 1u);
      }
      if (matte_src) {
        uint8_t *lbuf = ccord_calloc(npix * 4u, 1);
        uint8_t *mbuf = ccord_calloc(npix * 4u, 1);
        uint8_t *malpha = ccord_malloc(npix);
        if (lbuf && mbuf && malpha) {
          render_single_layer(s, lbuf, layers, layer, frame, comp_m, comp_op, depth);
          render_single_layer(s, mbuf, layers, matte_src, frame, comp_m, 1.0f, depth);
          if (tt == 1 || tt == 2) {
            for (size_t p = 0; p < npix; p++) malpha[p] = mbuf[p * 4u + 3u];
          } else {
            for (size_t p = 0; p < npix; p++) {
              malpha[p] = (uint8_t)((77u * mbuf[p * 4u] + 150u * mbuf[p * 4u + 1u] + 29u * mbuf[p * 4u + 2u]) >> 8);
            }
          }
          composite_premul(dst, lbuf, npix, malpha, tt == 2 || tt == 4);
        }
        ccord_free(malpha);
        ccord_free(mbuf);
        ccord_free(lbuf);
        continue;
      }
    }
    render_single_layer(s, dst, layers, layer, frame, comp_m, comp_op, depth);
  }
}

static void render_frame_into_out(ccord_lottie_stream *s, float frame)
{
  size_t npix = (size_t)s->out_w * (size_t)s->out_h;
  memset(s->premul, 0, npix * 4u);
  lot_mat root_m = {(float)s->out_w / s->src_w, 0.0f, 0.0f, (float)s->out_h / s->src_h, 0.0f, 0.0f};
  render_layers(s, s->premul, ccord_json_get(s->root, "layers"), frame, root_m, 1.0f, 0);
  // Convert premultiplied RGBA -> straight RGBA for texture upload.
  for (size_t i = 0; i < npix; i++) {
    uint8_t a = s->premul[i * 4u + 3u];
    uint8_t *o = s->out_rgba + i * 4u;
    if (a == 0) {
      o[0] = o[1] = o[2] = o[3] = 0;
    } else if (a == 255) {
      o[0] = s->premul[i * 4u + 0u];
      o[1] = s->premul[i * 4u + 1u];
      o[2] = s->premul[i * 4u + 2u];
      o[3] = 255;
    } else {
      uint32_t r = ((uint32_t)s->premul[i * 4u + 0u] * 255u + (a / 2u)) / a;
      uint32_t g = ((uint32_t)s->premul[i * 4u + 1u] * 255u + (a / 2u)) / a;
      uint32_t b = ((uint32_t)s->premul[i * 4u + 2u] * 255u + (a / 2u)) / a;
      o[0] = r > 255u ? 255u : (uint8_t)r;
      o[1] = g > 255u ? 255u : (uint8_t)g;
      o[2] = b > 255u ? 255u : (uint8_t)b;
      o[3] = a;
    }
  }
}

// ---- public stream API ----

ccord_lottie_stream *ccord_lottie_open(const uint8_t *data, size_t len, int max_dim, ccord_error *err)
{
  ccord_error_clear(err);
  if (!ccord_lottie_sniff(data, len)) {
    ccord_error_set(err, CCORD_ERR_PARSE, "lottie: not a Lottie JSON payload");
    return nullptr;
  }
  ccord_lottie_stream *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "lottie: out of memory");
    return nullptr;
  }
  ccord_arena_init(&s->arena, 65536u);
  ccord_json_limits lim = {.max_depth = 48u, .max_nodes = 262144u, .max_string = 256u * 1024u};
  s->root = ccord_json_parse(&s->arena, (const char *)data, len, &lim, err);
  if (!s->root || ccord_json_type_of(s->root) != CCORD_JSON_OBJECT) {
    if (err && err->code == CCORD_OK) ccord_error_set(err, CCORD_ERR_PARSE, "lottie: invalid JSON root");
    ccord_lottie_close(s);
    return nullptr;
  }
  const ccord_json *layers = ccord_json_get(s->root, "layers");
  if (!layers || ccord_json_type_of(layers) != CCORD_JSON_ARRAY) {
    ccord_error_set(err, CCORD_ERR_PARSE, "lottie: missing layers");
    ccord_lottie_close(s);
    return nullptr;
  }
  s->src_w = j_get_num(s->root, "w", 0.0f);
  s->src_h = j_get_num(s->root, "h", 0.0f);
  s->fr = j_get_num(s->root, "fr", 30.0f);
  s->ip = j_get_num(s->root, "ip", 0.0f);
  s->op = j_get_num(s->root, "op", s->ip + 1.0f);
  if (s->src_w < 1.0f || s->src_h < 1.0f || s->src_w > 8192.0f || s->src_h > 8192.0f || s->fr <= 0.0f || s->fr > 240.0f) {
    ccord_error_set(err, CCORD_ERR_RANGE, "lottie: header values out of range");
    ccord_lottie_close(s);
    return nullptr;
  }

  int cap = max_dim > 0 && max_dim < LOT_MAX_DIM ? max_dim : LOT_MAX_DIM;
  float big = s->src_w > s->src_h ? s->src_w : s->src_h;
  float scale = big > (float)cap ? (float)cap / big : 1.0f;
  s->out_w = (int)lroundf(s->src_w * scale);
  s->out_h = (int)lroundf(s->src_h * scale);
  if (s->out_w < 1) s->out_w = 1;
  if (s->out_h < 1) s->out_h = 1;

  float span = s->op - s->ip;
  if (span < 1.0f) span = 1.0f;
  float dur_sec = span / s->fr;
  float fps = s->fr > LOT_TARGET_FPS ? LOT_TARGET_FPS : s->fr;
  if (fps < 1.0f) fps = 1.0f;
  uint32_t nf = (uint32_t)lroundf(dur_sec * fps);
  if (nf < 1u) nf = 1u;
  if (nf > LOT_MAX_FRAMES) nf = LOT_MAX_FRAMES;
  s->frame_count = nf;
  s->dur_ms = (uint32_t)lroundf((dur_sec * 1000.0f) / (float)nf);
  if (s->dur_ms < 20u) s->dur_ms = 20u;
  s->total_ms = s->dur_ms * nf;

  size_t npix = (size_t)s->out_w * (size_t)s->out_h;
  s->premul = ccord_malloc(npix * 4u);
  s->out_rgba = ccord_malloc(npix * 4u);
  s->cov = ccord_malloc((size_t)s->out_w * sizeof *s->cov);
  if (!s->premul || !s->out_rgba || !s->cov) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "lottie: out of memory");
    ccord_lottie_close(s);
    return nullptr;
  }
  return s;
}

void ccord_lottie_close(ccord_lottie_stream *s)
{
  if (!s) return;
  ccord_free(s->premul);
  ccord_free(s->out_rgba);
  ccord_free(s->edges);
  ccord_free(s->cross);
  ccord_free(s->cov);
  ccord_arena_free(&s->arena);
  ccord_free(s);
}

void ccord_lottie_info(const ccord_lottie_stream *s, int *out_w, int *out_h, uint32_t *out_duration_ms)
{
  if (!s) return;
  if (out_w) *out_w = s->out_w;
  if (out_h) *out_h = s->out_h;
  if (out_duration_ms) *out_duration_ms = s->total_ms;
}

bool ccord_lottie_next_frame(ccord_lottie_stream *s, const uint8_t **rgba, uint32_t *pts_ms, uint32_t *dur_ms)
{
  if (rgba) *rgba = nullptr;
  if (!s || !rgba || s->cur_idx >= s->frame_count) return false;
  float frac = s->frame_count > 1u ? (float)s->cur_idx / (float)s->frame_count : 0.0f;
  float frame = s->ip + frac * (s->op - s->ip);
  render_frame_into_out(s, frame);
  *rgba = s->out_rgba;
  if (pts_ms) *pts_ms = s->cur_idx * s->dur_ms;
  if (dur_ms) *dur_ms = s->dur_ms;
  s->cur_idx++;
  return true;
}

void ccord_lottie_seek(ccord_lottie_stream *s, uint32_t pos_ms)
{
  if (!s || s->dur_ms == 0) return;
  uint32_t idx = pos_ms / s->dur_ms;
  s->cur_idx = idx < s->frame_count ? idx : s->frame_count;
}

size_t ccord_lottie_mem_bytes(const ccord_lottie_stream *s)
{
  if (!s) return 0;
  size_t npix = (size_t)s->out_w * (size_t)s->out_h;
  return npix * 8u + s->cap_edges * sizeof *s->edges + s->cap_cross * sizeof *s->cross;
}

ccord_status ccord_lottie_render_first_frame(const uint8_t *data, size_t len, int max_dim,
                                             uint8_t **out_rgba, int *out_w, int *out_h, ccord_error *err)
{
  ccord_error_clear(err);
  if (!out_rgba || !out_w || !out_h) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "lottie: null output");
  }
  *out_rgba = nullptr;
  *out_w = *out_h = 0;
  ccord_lottie_stream *s = ccord_lottie_open(data, len, max_dim, err);
  if (!s) return err && err->code != CCORD_OK ? err->code : CCORD_ERR_PARSE;
  const uint8_t *px = nullptr;
  if (!ccord_lottie_next_frame(s, &px, nullptr, nullptr) || !px) {
    ccord_lottie_close(s);
    return ccord_error_set(err, CCORD_ERR_PARSE, "lottie: failed to render frame");
  }
  size_t bytes = (size_t)s->out_w * (size_t)s->out_h * 4u;
  uint8_t *copy = ccord_malloc(bytes);
  if (!copy) {
    ccord_lottie_close(s);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "lottie: out of memory");
  }
  memcpy(copy, px, bytes);
  *out_rgba = copy;
  *out_w = s->out_w;
  *out_h = s->out_h;
  ccord_lottie_close(s);
  return CCORD_OK;
}
