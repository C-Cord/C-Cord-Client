// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice multi-camera: several remote voice cameras over the offline
// mock transport through the real session, app_media camera decoders, renderer textures and call UI.
//
// Every camera clip is real H.264 made with the app's own encoder (ccord_video_stream_*), pushed through
// ccord_mock_push_camera_frame, decoded by app_media and drawn by the call stage. Pixels are read back from a
// screenshot taken between draw and present, so a tile showing the wrong user's frame fails.
#include "verify_multi_camera.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>

#include "ui_internal.h"
#include "ui_voice_surfaces.h"

#define MC_GUILD 2001u
#define MC_VOICE 3121u
#define MC_ADA 1002u
#define MC_U1 1005u
#define MC_U2 1006u
#define MC_U3 1007u
#define MC_U4 1008u
#define MC_U5 1009u
#define MC_CLIP_W 160
#define MC_CLIP_H 90
#define MC_AU_CAP (256u * 1024u)
#define MC_AUS 4   // IDR, P, P, IDR (restart)

typedef enum mc_color { MC_RED, MC_BLUE, MC_GREEN, MC_NCOLORS } mc_color;

typedef struct mc_clip {
  uint8_t *au[MC_AUS];
  size_t len[MC_AUS];
  bool key[MC_AUS];
} mc_clip;

typedef struct mc {
  const ccord_verify_multi_camera_ctx *c;
  int checks;
  mc_clip clip[MC_NCOLORS];
  uint32_t ts;
  char shot_path[512];
  char dir[400];
} mc;

static int mc_fail(const char *name, const char *detail)
{
  printf("multi-camera: FAIL %s%s%s\n", name, detail && detail[0] ? ": " : "", detail ? detail : "");
  fflush(stdout);
  return 1;
}

#define MC_REQUIRE(name, cond, detail)         \
  do {                                         \
    if (!(cond)) return mc_fail((name), (detail)); \
  } while (0)

static void mc_frames(mc *m, int n) { m->c->frames(m->c->ud, n); }

// ---- clips -----------------------------------------------------------------------------------------------------
static void mc_rgb(mc_color col, uint8_t out[3])
{
  static const uint8_t k[MC_NCOLORS][3] = {{230, 20, 20}, {20, 20, 230}, {20, 230, 20}};
  memcpy(out, k[col], 3);
}

// Result: 0 ok, 1 encoder is not H.264 (reported as encoder-not-h264), 2 other failure (printed).
static int mc_make_clip(mc *m, mc_color col)
{
  ccord_video_config cfg = {.width = MC_CLIP_W, .height = MC_CLIP_H, .fps = 30, .bitrate = 300000,
                            .codec = CCORD_VIDEO_CODEC_H264, .use_pipewire = false};
  ccord_error err = {0};
  ccord_video_stream *v = ccord_video_stream_create(&cfg, &err);
  if (!v) {
    (void)mc_fail("encoder-open", err.message);
    return 2;
  }
  const char *name = ccord_video_stream_codec_name(v);
  if (!name || !strstr(name, "264")) {
    ccord_video_stream_destroy(v);
    return 1;
  }
  uint8_t *rgba = ccord_malloc((size_t)MC_CLIP_W * MC_CLIP_H * 4u);
  uint8_t c[3];
  mc_rgb(col, c);
  for (int i = 0; i < MC_CLIP_W * MC_CLIP_H; i++) {
    rgba[i * 4] = c[0]; rgba[i * 4 + 1] = c[1]; rgba[i * 4 + 2] = c[2]; rgba[i * 4 + 3] = 255;
  }
  mc_clip *clip = &m->clip[col];
  int rc = 0;
  for (int i = 0; i < MC_AUS && rc == 0; i++) {
    if (i == 0 || i == MC_AUS - 1) ccord_video_stream_force_keyframe(v);
    clip->au[i] = ccord_malloc(MC_AU_CAP);
    size_t len = 0;
    if (ccord_video_stream_encode_rgba(v, rgba, MC_CLIP_W, MC_CLIP_H, 0, clip->au[i], MC_AU_CAP, &len, &err) !=
            CCORD_OK ||
        len == 0) {
      (void)mc_fail("encode", err.message[0] ? err.message : "encoder produced no access unit");
      rc = 2;
      break;
    }
    clip->len[i] = len;
    clip->key[i] = ccord_video_stream_last_keyframe(v);
  }
  if (rc == 0 && (!clip->key[0] || !clip->key[MC_AUS - 1])) {
    (void)mc_fail("encode", "no IDR when one was forced");
    rc = 2;
  }
  ccord_free(rgba);
  ccord_video_stream_destroy(v);
  return rc;
}

static void mc_free_clips(mc *m)
{
  for (int c = 0; c < MC_NCOLORS; c++)
    for (int i = 0; i < MC_AUS; i++) ccord_free(m->clip[c].au[i]);
}

static bool mc_push(mc *m, ccord_id user, mc_color col, int idx)
{
  ccord_error err = {0};
  const mc_clip *cl = &m->clip[col];
  m->ts += 3000;
  return ccord_mock_push_camera_frame(m->c->transport, user, cl->au[idx], cl->len[idx], m->ts, cl->key[idx], &err) ==
         CCORD_OK;
}

// The whole clip: IDR + two P frames.
static bool mc_push_clip(mc *m, ccord_id user, mc_color col)
{
  for (int i = 0; i < 3; i++)
    if (!mc_push(m, user, col, i)) return false;
  return true;
}

// ---- state readers ---------------------------------------------------------------------------------------------
static bool mc_set_cam(mc *m, ccord_id user, bool on)
{
  ccord_error err = {0};
  return ccord_mock_set_remote_camera(m->c->transport, MC_GUILD, MC_VOICE, user, on, &err) == CCORD_OK;
}

static size_t mc_senders(mc *m, ccord_camera_rx *out, size_t max)
{
  return ccord_session_camera_senders(m->c->session, out, max);
}

static const ccord_camera_rx *mc_find_sender(const ccord_camera_rx *s, size_t n, ccord_id user)
{
  for (size_t i = 0; i < n; i++)
    if (s[i].user_id == user) return &s[i];
  return nullptr;
}

static size_t mc_decoding(const ccord_camera_rx *s, size_t n)
{
  size_t d = 0;
  for (size_t i = 0; i < n; i++)
    if (s[i].decoding) d++;
  return d;
}

static void mc_stage(const mc *m, ui_stage_debug *out)
{
  memset(out, 0, sizeof *out);
  ui_stage_get_debug(m->c->ui, out);
}

static const typeof(((ui_stage_debug *)0)->cam_tile[0]) *mc_tile(const ui_stage_debug *d, ccord_id user)
{
  for (size_t i = 0; i < d->cam_tile_count && i < UI_CAM_TILE_MAX; i++)
    if (d->cam_tile[i].user_id == user) return &d->cam_tile[i];
  return nullptr;
}

static bool mc_has_tex(mc *m, ccord_id user, int *w, int *h)
{
  char key[CCORD_CAM_TEXTURE_KEY_MAX];
  ccord_session_camera_texture_key(user, key, sizeof key);
  return ccord_render_has_texture(m->c->renderer, key, w, h);
}

static bool mc_any_cam_texture(mc *m)
{
  static const ccord_id users[] = {MC_U1, MC_U2, MC_U3, MC_U4, MC_U5};
  for (size_t i = 0; i < sizeof users / sizeof users[0]; i++)
    if (mc_has_tex(m, users[i], nullptr, nullptr)) return true;
  return false;
}

static bool mc_rect_inside(ccord_rect r, ccord_rect v)
{
  const float e = 1.0f;
  return r.w > 0.0f && r.h > 0.0f && r.x >= v.x - e && r.y >= v.y - e && r.x + r.w <= v.x + v.w + e &&
         r.y + r.h <= v.y + v.h + e;
}

static bool mc_rects_overlap(ccord_rect a, ccord_rect b)
{
  return a.x < b.x + b.w - 0.5f && b.x < a.x + a.w - 0.5f && a.y < b.y + b.h - 0.5f && b.y < a.y + a.h - 0.5f;
}

static bool mc_is_16x9(ccord_rect v) { return v.w > 0.0f && fabsf(v.h - v.w * 9.0f / 16.0f) <= 1.0f; }

// ---- pixels ----------------------------------------------------------------------------------------------------
typedef enum mc_dom { MC_DOM_NONE, MC_DOM_R, MC_DOM_G, MC_DOM_B } mc_dom;

static mc_dom mc_dominant(const uint8_t *px)
{
  int r = px[0], g = px[1], b = px[2];
  if (r - g >= 80 && r - b >= 80) return MC_DOM_R;
  if (g - r >= 80 && g - b >= 80) return MC_DOM_G;
  if (b - r >= 80 && b - g >= 80) return MC_DOM_B;
  return MC_DOM_NONE;
}

static const char *mc_dom_name(mc_dom d)
{
  return d == MC_DOM_R ? "red" : d == MC_DOM_G ? "green" : d == MC_DOM_B ? "blue" : "none";
}

static mc_dom mc_col_dom(mc_color c) { return c == MC_RED ? MC_DOM_R : c == MC_BLUE ? MC_DOM_B : MC_DOM_G; }

typedef struct mc_shot {
  uint8_t *rgba;
  int w, h;
  float scale;
} mc_shot;

static void mc_shot_free(mc_shot *s)
{
  ccord_free(s->rgba);
  s->rgba = nullptr;
}

static bool mc_take_shot(mc *m, mc_shot *s, char *why, size_t why_cap)
{
  memset(s, 0, sizeof *s);
  ccord_error err = {0};
  if (m->c->screenshot(m->c->ud, m->shot_path, &err) != CCORD_OK) {
    snprintf(why, why_cap, "screenshot failed: %s", err.message);
    return false;
  }
  FILE *f = fopen(m->shot_path, "rb");
  if (!f) {
    snprintf(why, why_cap, "screenshot file missing");
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = n > 0 ? ccord_malloc((size_t)n) : nullptr;
  bool ok = data && fread(data, 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  if (!ok) {
    ccord_free(data);
    snprintf(why, why_cap, "screenshot unreadable");
    return false;
  }
  ccord_status st = ccord_image_decode_rgba(data, (size_t)n, &s->rgba, &s->w, &s->h, &err);
  ccord_free(data);
  if (st != CCORD_OK || !s->rgba) {
    snprintf(why, why_cap, "screenshot decode failed: %s", err.message);
    return false;
  }
  float lw = 0.0f, lh = 0.0f;
  m->c->viewport(m->c->ud, &lw, &lh);
  if (lw <= 0.0f) {
    mc_shot_free(s);
    snprintf(why, why_cap, "no viewport size");
    return false;
  }
  s->scale = (float)s->w / lw;
  return true;
}

// Mean colour of a 9x9 patch around the centre of `video` (logical px), written to out[3].
static bool mc_sample(const mc_shot *s, ccord_rect video, uint8_t out[3])
{
  int cx = (int)lroundf((video.x + video.w * 0.5f) * s->scale);
  int cy = (int)lroundf((video.y + video.h * 0.5f) * s->scale);
  if (cx < 4 || cy < 4 || cx + 4 >= s->w || cy + 4 >= s->h) return false;
  int sum[3] = {0, 0, 0};
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++) {
      const uint8_t *p = s->rgba + ((size_t)(cy + dy) * (size_t)s->w + (size_t)(cx + dx)) * 4u;
      sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
    }
  for (int i = 0; i < 3; i++) out[i] = (uint8_t)(sum[i] / 81);
  return true;
}

// Fresh screenshot; each (user, colour) pair must be dominated by that colour at its tile's video centre.
typedef struct mc_expect {
  ccord_id user;
  mc_color color;
} mc_expect;

static int mc_check_pixels(mc *m, const char *name, const mc_expect *ex, size_t n)
{
  mc_shot s;
  char why[320];
  if (!mc_take_shot(m, &s, why, sizeof why)) return mc_fail(name, why);
  ui_stage_debug d;
  mc_stage(m, &d);
  for (size_t i = 0; i < n; i++) {
    const typeof(d.cam_tile[0]) *t = mc_tile(&d, ex[i].user);
    if (!t || !t->textured) {
      mc_shot_free(&s);
      return mc_fail(name, "tile missing or not textured");
    }
    uint8_t px[3];
    if (!mc_rect_inside(t->video, d.stage_view) || !mc_sample(&s, t->video, px)) {
      mc_shot_free(&s);
      return mc_fail(name, "tile video centre is not on screen");
    }
    mc_dom got = mc_dominant(px), want = mc_col_dom(ex[i].color);
    if (got != want) {
      char msg[160];
      snprintf(msg, sizeof msg, "user %u tile is %s (%u,%u,%u), expected %s", (unsigned)ex[i].user, mc_dom_name(got),
               px[0], px[1], px[2], mc_dom_name(want));
      mc_shot_free(&s);
      return mc_fail(name, msg);
    }
  }
  mc_shot_free(&s);
  return 0;
}

// ---- the 18 checks ---------------------------------------------------------------------------------------------
static ccord_app_media_cam_debug mc_dbg(mc *m, ccord_id user, bool *ok)
{
  ccord_app_media_cam_debug d;
  memset(&d, 0, sizeof d);
  *ok = ccord_app_media_cam_debug_of(m->c->media, user, &d);
  return d;
}

static size_t mc_ctl_count(mc *m)
{
  ui_stage_debug d;
  mc_stage(m, &d);
  return d.ctl_count;
}

static int mc_join(mc *m)
{
  ccord_error err = {0};
  mc_frames(m, 8);   // the mock snapshot (guilds, channels, voice states) arrives through the first frames
  if (ccord_session_join_voice(m->c->session, MC_GUILD, MC_VOICE, &err) != CCORD_OK) return mc_fail("join", err.message);
  mc_frames(m, 3);
  m->c->ui->stage_channel = MC_VOICE;
  m->c->ui->dirty = true;
  mc_frames(m, 2);
  if (ccord_store_active_voice_channel(m->c->store) != MC_VOICE) return mc_fail("join", "not in the voice channel");
  return 0;
}

static int run(mc *m)
{
  // ---- 1. encoder is H.264 (never skipped) -------------------------------------------------------------------
  for (int c = 0; c < MC_NCOLORS; c++) {
    int rc = mc_make_clip(m, (mc_color)c);
    if (rc == 1) return mc_fail("encoder-not-h264", "the platform encoder fell back from H.264");
    if (rc != 0) return rc;
  }
  m->checks++;

  if (mc_join(m) != 0) return 1;
  const size_t base_ctl = mc_ctl_count(m);
  const size_t base_voice = ccord_store_voice_count(m->c->store, MC_VOICE);

  // ---- 2. two senders listed ---------------------------------------------------------------------------------
  MC_REQUIRE("senders-listed", mc_set_cam(m, MC_U1, true) && mc_set_cam(m, MC_U2, true), "set_remote_camera failed");
  mc_frames(m, 2);
  ccord_camera_rx snd[CCORD_CAM_RX_MAX];
  size_t ns = mc_senders(m, snd, CCORD_CAM_RX_MAX);
  const ccord_camera_rx *s1 = mc_find_sender(snd, ns, MC_U1), *s2 = mc_find_sender(snd, ns, MC_U2);
  MC_REQUIRE("senders-listed", ns == 2 && s1 && s2, "expected exactly senders 1005 and 1006");
  MC_REQUIRE("senders-listed", s1->decoding && s2->decoding, "a sender is not decoding");
  MC_REQUIRE("senders-listed", s1->generation != 0 && s2->generation != 0 && s1->generation != s2->generation,
             "generations are zero or equal");
  const uint32_t gen2_first = s2->generation;
  m->checks++;

  // ---- 3. store shows both cameras ---------------------------------------------------------------------------
  {
    bool u1 = false, u2 = false, ada_cam = false;
    size_t n = ccord_store_voice_count(m->c->store, MC_VOICE);
    for (size_t i = 0; i < n; i++) {
      const ccord_voice_state *v = ccord_store_voice_at(m->c->store, MC_VOICE, i);
      if (!v) continue;
      if (v->user_id == MC_U1 && v->self_video) u1 = true;
      if (v->user_id == MC_U2 && v->self_video) u2 = true;
      if (v->user_id == MC_ADA && v->self_video) ada_cam = true;
    }
    MC_REQUIRE("store-cameras", u1 && u2, "store lacks self_video for 1005/1006");
    MC_REQUIRE("store-cameras", !ada_cam, "Ada (stream only) shows self_video");
    m->checks++;
  }

  // ---- 4. tiles before frames --------------------------------------------------------------------------------
  {
    ui_stage_debug d;
    mc_stage(m, &d);
    MC_REQUIRE("tiles-before-frames", d.cam_tile_count == 2 && d.cam_cols == 2, "expected 2 tiles in 2 columns");
    const typeof(d.cam_tile[0]) *t1 = mc_tile(&d, MC_U1), *t2 = mc_tile(&d, MC_U2);
    MC_REQUIRE("tiles-before-frames", t1 && t2, "tiles for 1005/1006 missing");
    MC_REQUIRE("tiles-before-frames", !t1->textured && !t2->textured, "tile textured before any frame");
    MC_REQUIRE("tiles-before-frames", t1->connecting && t2->connecting, "tiles do not say connecting");
    MC_REQUIRE("tiles-before-frames", mc_tile(&d, MC_ADA) == nullptr, "Ada got a camera tile");
    m->checks++;
  }

  // ---- 5. Go Live untouched ----------------------------------------------------------------------------------
  MC_REQUIRE("golive-untouched", !ccord_render_has_texture(m->c->renderer, "__video_stream__", nullptr, nullptr),
             "a Go Live texture exists");
  MC_REQUIRE("golive-untouched", ccord_app_media_cam_count(m->c->media) == 2, "cam_count is not 2");
  m->checks++;

  // ---- 6. frames flow ----------------------------------------------------------------------------------------
  MC_REQUIRE("frames-flow", mc_push_clip(m, MC_U1, MC_RED) && mc_push_clip(m, MC_U2, MC_BLUE), "push failed");
  mc_frames(m, 4);
  MC_REQUIRE("frames-flow",
             ccord_mock_camera_frame_count(m->c->transport, MC_U1) == 0 &&
                 ccord_mock_camera_frame_count(m->c->transport, MC_U2) == 0,
             "mock FIFOs not drained");
  {
    int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    MC_REQUIRE("frames-flow", mc_has_tex(m, MC_U1, &w1, &h1) && mc_has_tex(m, MC_U2, &w2, &h2),
               "a camera texture is missing");
    MC_REQUIRE("frames-flow", w1 == MC_CLIP_W && h1 == MC_CLIP_H && w2 == MC_CLIP_W && h2 == MC_CLIP_H,
               "texture is not 160x90");
    m->checks++;
  }

  // ---- 7. distinct textures ----------------------------------------------------------------------------------
  uint64_t hash1, hash2;
  {
    bool o1, o2;
    ccord_app_media_cam_debug d1 = mc_dbg(m, MC_U1, &o1), d2 = mc_dbg(m, MC_U2, &o2);
    MC_REQUIRE("distinct-textures", o1 && o2, "no cam debug for a sender");
    MC_REQUIRE("distinct-textures", d1.frames >= 1 && d2.frames >= 1, "no frames decoded");
    MC_REQUIRE("distinct-textures", d1.decode_errors == 0 && d2.decode_errors == 0, "decode errors");
    MC_REQUIRE("distinct-textures", d1.frame_hash != d2.frame_hash, "frame hashes are equal");
    MC_REQUIRE("distinct-textures", mc_dominant(d1.center_rgb) == MC_DOM_R, "1005 decoded frame is not red");
    MC_REQUIRE("distinct-textures", mc_dominant(d2.center_rgb) == MC_DOM_B, "1006 decoded frame is not blue");
    hash1 = d1.frame_hash;
    hash2 = d2.frame_hash;
    m->checks++;
  }

  // ---- 8. UI tiles bound to the right user -------------------------------------------------------------------
  {
    ui_stage_debug d;
    mc_stage(m, &d);
    const typeof(d.cam_tile[0]) *t1 = mc_tile(&d, MC_U1), *t2 = mc_tile(&d, MC_U2);
    MC_REQUIRE("ui-tiles", t1 && t2, "tiles missing");
    MC_REQUIRE("ui-tiles", t1->textured && t2->textured, "tiles not textured");
    MC_REQUIRE("ui-tiles", !strcmp(t1->key, "__cam__:1005") && !strcmp(t2->key, "__cam__:1006"), "wrong texture key");
    MC_REQUIRE("ui-tiles", t1->tex_w == MC_CLIP_W && t1->tex_h == MC_CLIP_H && t2->tex_w == MC_CLIP_W &&
                               t2->tex_h == MC_CLIP_H,
               "tile texture is not 160x90");
    MC_REQUIRE("ui-tiles", !mc_rects_overlap(t1->rect, t2->rect), "tiles overlap");
    MC_REQUIRE("ui-tiles", mc_rect_inside(t1->rect, d.stage_view) && mc_rect_inside(t2->rect, d.stage_view),
               "tile outside the stage view");
    MC_REQUIRE("ui-tiles", mc_is_16x9(t1->video) && mc_is_16x9(t2->video), "video area is not 16:9");
    m->checks++;
  }

  // ---- 9. pixels on screen differ ----------------------------------------------------------------------------
  {
    const mc_expect ex[] = {{MC_U1, MC_RED}, {MC_U2, MC_BLUE}};
    if (mc_check_pixels(m, "pixels-differ", ex, 2) != 0) return 1;
    m->checks++;
  }

  // ---- 10. per-user update -----------------------------------------------------------------------------------
  MC_REQUIRE("per-user-update", mc_push(m, MC_U1, MC_GREEN, 0), "push failed");
  mc_frames(m, 3);
  {
    bool o1, o2;
    ccord_app_media_cam_debug d1 = mc_dbg(m, MC_U1, &o1), d2 = mc_dbg(m, MC_U2, &o2);
    MC_REQUIRE("per-user-update", o1 && o2, "no cam debug");
    MC_REQUIRE("per-user-update", d1.frame_hash != hash1, "1005 frame hash did not change");
    MC_REQUIRE("per-user-update", d2.frame_hash == hash2, "1006 frame hash changed");
    const mc_expect ex[] = {{MC_U1, MC_GREEN}, {MC_U2, MC_BLUE}};
    if (mc_check_pixels(m, "per-user-update", ex, 2) != 0) return 1;
    m->checks++;
  }

  // ---- 11. IDR gating ----------------------------------------------------------------------------------------
  MC_REQUIRE("idr-gating", mc_set_cam(m, MC_U2, false), "camera off failed");
  mc_frames(m, 3);
  MC_REQUIRE("idr-gating", !mc_has_tex(m, MC_U2, nullptr, nullptr), "texture survived camera off");
  MC_REQUIRE("idr-gating", mc_set_cam(m, MC_U2, true), "camera on failed");
  mc_frames(m, 3);
  ns = mc_senders(m, snd, CCORD_CAM_RX_MAX);
  s2 = mc_find_sender(snd, ns, MC_U2);
  MC_REQUIRE("idr-gating", s2 && s2->decoding && s2->generation != gen2_first, "generation did not change");
  MC_REQUIRE("idr-gating", !mc_has_tex(m, MC_U2, nullptr, nullptr), "texture exists before an IDR");
  MC_REQUIRE("idr-gating", mc_push(m, MC_U2, MC_BLUE, 1), "push failed");   // P frame only
  mc_frames(m, 3);
  MC_REQUIRE("idr-gating", !mc_has_tex(m, MC_U2, nullptr, nullptr), "a P frame created a texture");
  MC_REQUIRE("idr-gating", mc_push(m, MC_U2, MC_BLUE, 0), "push failed");   // IDR
  mc_frames(m, 3);
  MC_REQUIRE("idr-gating", mc_has_tex(m, MC_U2, nullptr, nullptr), "IDR did not create the texture");
  m->checks++;

  // ---- 12. cap of 4 ------------------------------------------------------------------------------------------
  MC_REQUIRE("cap-of-4", mc_set_cam(m, MC_U3, true) && mc_set_cam(m, MC_U4, true) && mc_set_cam(m, MC_U5, true),
             "set_remote_camera failed");
  mc_frames(m, 3);
  ns = mc_senders(m, snd, CCORD_CAM_RX_MAX);
  MC_REQUIRE("cap-of-4", ns == 5 && mc_decoding(snd, ns) == 4, "expected 5 senders with exactly 4 decoding");
  const ccord_camera_rx *s5 = mc_find_sender(snd, ns, MC_U5);
  MC_REQUIRE("cap-of-4", s5 && !s5->decoding, "1009 is decoding");
  MC_REQUIRE("cap-of-4", ccord_app_media_cam_count(m->c->media) == 4, "cam_count is not 4");
  MC_REQUIRE("cap-of-4", !mc_has_tex(m, MC_U5, nullptr, nullptr), "waiting sender has a texture");
  MC_REQUIRE("cap-of-4", mc_push(m, MC_U5, MC_BLUE, 0), "push for the waiting sender failed");
  mc_frames(m, 2);
  MC_REQUIRE("cap-of-4", ccord_mock_camera_frame_count(m->c->transport, MC_U5) == 0, "waiting sender's AU was queued");
  MC_REQUIRE("cap-of-4", !mc_has_tex(m, MC_U5, nullptr, nullptr), "waiting sender got a texture");
  {
    ui_stage_debug d;
    mc_stage(m, &d);
    const typeof(d.cam_tile[0]) *t5 = mc_tile(&d, MC_U5);
    MC_REQUIRE("cap-of-4", d.cam_tile_count == 5 && t5 && t5->limit && !t5->textured, "1009 tile is not 'Video limit'");
    m->checks++;
  }

  // ---- 13. promotion -----------------------------------------------------------------------------------------
  const uint32_t gen5_wait = s5->generation;
  MC_REQUIRE("promotion", mc_has_tex(m, MC_U1, nullptr, nullptr), "1005 had no texture before leaving");
  MC_REQUIRE("promotion", mc_set_cam(m, MC_U1, false), "camera off failed");
  for (int i = 0; i < 4; i++) {
    mc_frames(m, 1);
    MC_REQUIRE("promotion", ccord_app_media_cam_count(m->c->media) <= 4, "more than 4 camera decoders");
  }
  MC_REQUIRE("promotion", !mc_has_tex(m, MC_U1, nullptr, nullptr), "1005 texture was not evicted");
  ns = mc_senders(m, snd, CCORD_CAM_RX_MAX);
  s5 = mc_find_sender(snd, ns, MC_U5);
  MC_REQUIRE("promotion", s5 && s5->decoding && s5->generation != gen5_wait, "1009 was not promoted with a new generation");
  MC_REQUIRE("promotion", ccord_app_media_cam_count(m->c->media) == 4, "cam_count is not 4 after promotion");
  MC_REQUIRE("promotion", mc_push_clip(m, MC_U5, MC_GREEN), "push failed");
  mc_frames(m, 4);
  {
    ui_stage_debug d;
    mc_stage(m, &d);
    const typeof(d.cam_tile[0]) *t5 = mc_tile(&d, MC_U5);
    MC_REQUIRE("promotion", t5 && t5->textured && !t5->limit, "1009 tile is not textured after promotion");
    m->checks++;
  }

  // ---- 14. layout rules --------------------------------------------------------------------------------------
  // N = 1..5 camera participants: only 1006 (N=1) -> add 1005, 1007, 1008 and 1009 one at a time. Stop every camera
  // first so the order of the steps is the same as the order they are turned on.
  {
    static const ccord_id order[] = {MC_U2, MC_U1, MC_U3, MC_U4, MC_U5};
    static const int want_cols[] = {1, 2, 2, 2, 3};
    for (size_t i = 0; i < 5; i++)
      if (!mc_set_cam(m, order[i], false)) return mc_fail("layout", "camera off failed");
    mc_frames(m, 3);
    for (size_t n = 1; n <= 5; n++) {
      if (!mc_set_cam(m, order[n - 1], true)) return mc_fail("layout", "camera on failed");
      mc_frames(m, 3);
      ui_stage_debug d;
      mc_stage(m, &d);
      char msg[160];
      snprintf(msg, sizeof msg, "N=%zu: %zu tiles, %d columns (want %d)", n, d.cam_tile_count, d.cam_cols,
               want_cols[n - 1]);
      MC_REQUIRE("layout", d.cam_tile_count == n && d.cam_cols == want_cols[n - 1], msg);
      for (size_t i = 0; i < n; i++) {
        snprintf(msg, sizeof msg, "N=%zu tile %zu video is not 16:9", n, i);
        MC_REQUIRE("layout", mc_is_16x9(d.cam_tile[i].video), msg);
        snprintf(msg, sizeof msg, "N=%zu tile %zu is outside the stage view", n, i);
        // the stage scrolls vertically; every tile must still sit inside it horizontally and start inside it
        ccord_rect r = d.cam_tile[i].rect;
        bool horiz = r.x >= d.stage_view.x - 1.0f && r.x + r.w <= d.stage_view.x + d.stage_view.w + 1.0f;
        MC_REQUIRE("layout", horiz && r.y >= d.stage_view.y - 1.0f, msg);
        for (size_t j = i + 1; j < n; j++) {
          snprintf(msg, sizeof msg, "N=%zu tiles %zu and %zu overlap", n, i, j);
          MC_REQUIRE("layout", !mc_rects_overlap(r, d.cam_tile[j].rect), msg);
        }
      }
      if (n == 3) {
        float block_cx = (d.cam_tile[0].rect.x + d.cam_tile[1].rect.x + d.cam_tile[1].rect.w) * 0.5f;
        float third_cx = d.cam_tile[2].rect.x + d.cam_tile[2].rect.w * 0.5f;
        MC_REQUIRE("layout", fabsf(block_cx - third_cx) <= 1.5f, "the partial last row is not centred");
        MC_REQUIRE("layout", d.cam_tile[2].rect.y > d.cam_tile[0].rect.y, "third tile is not on a second row");
      }
    }
    m->checks++;
  }

  // ---- 15. Go Live coexistence -------------------------------------------------------------------------------
  {
    // Every camera of the call is on (N=5 -> 4 decoding); give them their colours.
    ccord_error err = {0};
    static const mc_expect ex[] = {{MC_U2, MC_BLUE}, {MC_U1, MC_RED}, {MC_U3, MC_GREEN}, {MC_U4, MC_RED}};
    for (size_t i = 0; i < 4; i++)
      if (!mc_push_clip(m, ex[i].user, ex[i].color)) return mc_fail("golive-coexist", "push failed");
    mc_frames(m, 6);
    ccord_stream_config cfg = {.encoder = CCORD_STREAM_ENCODER_AUTO,
                               .codec = CCORD_STREAM_CODEC_H264,
                               .framework = CCORD_STREAM_FRAMEWORK_AUTO,
                               .resolution = CCORD_STREAM_RES_720P,
                               .fps = CCORD_STREAM_FPS_30};
    MC_REQUIRE("golive-coexist",
               ccord_session_start_stream(m->c->session, MC_GUILD, MC_VOICE, &cfg, &err) == CCORD_OK, err.message);
    mc_frames(m, 6);
    MC_REQUIRE("golive-coexist", ccord_store_stream_active(m->c->store), "stream did not become active");
    MC_REQUIRE("golive-coexist",
               ccord_session_watch_stream(m->c->session, MC_GUILD, MC_VOICE, ccord_store_self_id(m->c->store), &err) ==
                   CCORD_OK,
               err.message);
    mc_frames(m, 6);
    const ccord_watched_stream *w = ccord_session_watched_stream(m->c->session);
    MC_REQUIRE("golive-coexist", w && w->active, "watch did not activate");
    int sw = 0, sh = 0;
    MC_REQUIRE("golive-coexist", ccord_render_has_texture(m->c->renderer, "__video_stream__", &sw, &sh),
               "no Go Live texture");
    bool o[4];
    uint64_t before[4];
    for (size_t i = 0; i < 4; i++) {
      ccord_app_media_cam_debug d = mc_dbg(m, ex[i].user, &o[i]);
      MC_REQUIRE("golive-coexist", o[i], "camera slot lost while streaming");
      MC_REQUIRE("golive-coexist", mc_has_tex(m, ex[i].user, nullptr, nullptr), "camera texture lost while streaming");
      before[i] = d.frame_hash;
    }
    mc_frames(m, 6);   // stream frames keep decoding; camera frames must not move
    for (size_t i = 0; i < 4; i++) {
      ccord_app_media_cam_debug d = mc_dbg(m, ex[i].user, &o[i]);
      MC_REQUIRE("golive-coexist", o[i] && d.frame_hash == before[i], "a camera frame changed when stream frames decoded");
    }
    ui_stage_debug d;
    mc_stage(m, &d);
    MC_REQUIRE("golive-coexist", d.stream_frame_present, "stage shows no stream frame");
    MC_REQUIRE("golive-coexist", d.stream_player.w > 0.0f && d.stream_player.h > 0.0f, "no stream player rect");
    for (size_t i = 0; i < d.cam_tile_count; i++)
      MC_REQUIRE("golive-coexist", d.stream_player.y + d.stream_player.h <= d.cam_tile[i].rect.y + 0.5f,
                 "the stream player is not above the camera tiles");
    // Camera tiles still carry their own colours; sample those that are on screen.
    if (d.cam_tile_count > 0) {
      mc_shot s;
      char why[320];
      MC_REQUIRE("golive-coexist", mc_take_shot(m, &s, why, sizeof why), why);
      int sampled = 0;
      for (size_t i = 0; i < 4; i++) {
        const typeof(d.cam_tile[0]) *t = mc_tile(&d, ex[i].user);
        uint8_t px[3];
        if (!t || !t->textured || !mc_rect_inside(t->video, d.stage_view) || !mc_sample(&s, t->video, px)) continue;
        sampled++;
        if (mc_dominant(px) != mc_col_dom(ex[i].color)) {
          mc_shot_free(&s);
          return mc_fail("golive-coexist", "a camera tile lost its colour beside the Go Live player");
        }
      }
      mc_shot_free(&s);
      MC_REQUIRE("golive-coexist", sampled >= 1, "no camera tile is on screen beside the Go Live player");
    }
    (void)ccord_session_stop_watching(m->c->session, &err);
    (void)ccord_session_stop_stream(m->c->session, MC_GUILD, &err);
    mc_frames(m, 6);
    m->checks++;
  }

  // ---- 16. leave ---------------------------------------------------------------------------------------------
  ccord_camera_rx before_leave[CCORD_CAM_RX_MAX];
  size_t n_before_leave = mc_senders(m, before_leave, CCORD_CAM_RX_MAX);
  {
    ccord_error err = {0};
    MC_REQUIRE("leave", ccord_session_leave_voice(m->c->session, MC_GUILD, &err) == CCORD_OK, err.message);
    mc_frames(m, 4);
    // The UI closes the stage on leave; look at the channel again as a spectator (peers still have cameras on).
    m->c->ui->stage_channel = MC_VOICE;
    m->c->ui->dirty = true;
    mc_frames(m, 3);
    MC_REQUIRE("leave", ccord_app_media_cam_count(m->c->media) == 0, "cam_count is not 0");
    MC_REQUIRE("leave", !mc_any_cam_texture(m), "a camera texture is left");
    MC_REQUIRE("leave", mc_senders(m, snd, CCORD_CAM_RX_MAX) == 0, "senders still listed");
    ui_stage_debug d;
    mc_stage(m, &d);
    MC_REQUIRE("leave", d.cam_tile_count == 0, "camera tiles still drawn");
    static const ccord_id users[] = {MC_U1, MC_U2, MC_U3, MC_U4, MC_U5};
    for (size_t i = 0; i < 5; i++)
      MC_REQUIRE("leave", ccord_mock_camera_frame_count(m->c->transport, users[i]) == 0, "mock FIFO not cleared");
    m->checks++;
  }

  // ---- 17. rejoin --------------------------------------------------------------------------------------------
  {
    if (mc_join(m) != 0) return 1;
    mc_frames(m, 3);
    ccord_camera_rx again[CCORD_CAM_RX_MAX];
    size_t na = mc_senders(m, again, CCORD_CAM_RX_MAX);
    MC_REQUIRE("rejoin", na == 5 && mc_decoding(again, na) == 4, "remote senders were not re-listed (5, 4 decoding)");
    for (size_t i = 0; i < na; i++)
      MC_REQUIRE("rejoin", again[i].generation != 0, "zero generation after rejoin");
    MC_REQUIRE("rejoin", n_before_leave == 5, "five senders were not listed before the leave");
    for (size_t i = 0; i < na; i++) {
      const ccord_camera_rx *old = mc_find_sender(before_leave, n_before_leave, again[i].user_id);
      MC_REQUIRE("rejoin", old && old->generation != again[i].generation, "a sender kept its generation across the rejoin");
    }
    m->checks++;
  }

  // ---- 18. no camera, no change ------------------------------------------------------------------------------
  {
    static const ccord_id users[] = {MC_U1, MC_U2, MC_U3, MC_U4, MC_U5};
    for (size_t i = 0; i < 5; i++)
      if (!mc_set_cam(m, users[i], false)) return mc_fail("no-camera", "camera off failed");
    mc_frames(m, 4);
    ui_stage_debug d;
    mc_stage(m, &d);
    MC_REQUIRE("no-camera", d.cam_tile_count == 0 && d.cam_cols == 0, "camera tiles drawn without a camera");
    MC_REQUIRE("no-camera", ccord_app_media_cam_count(m->c->media) == 0, "camera decoders open without a camera");
    // Every remote participant is an audio-only tile again: one control set per participant beyond the baseline.
    size_t voice_now = ccord_store_voice_count(m->c->store, MC_VOICE);
    size_t extra = voice_now > base_voice ? voice_now - base_voice : 0;
    MC_REQUIRE("no-camera", d.ctl_count == base_ctl + extra, "the audio-only grid lost or gained controls");
    m->checks++;
  }
  return 0;
}

int ccord_verify_multi_camera_slice(const ccord_verify_multi_camera_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->transport || !ctx->media ||
      !ctx->frames || !ctx->screenshot || !ctx->viewport)
    return mc_fail("setup", "incomplete context");
  mc *m = ccord_calloc(1, sizeof *m);
  m->c = ctx;
  const char *tmp = getenv("TMPDIR");
  snprintf(m->dir, sizeof m->dir, "%s", tmp && tmp[0] ? tmp : "/tmp");
  snprintf(m->shot_path, sizeof m->shot_path, "%s/ccord_multi_camera_%ld.png", m->dir, (long)getpid());
  int rc = run(m);
  (void)remove(m->shot_path);
  mc_free_clips(m);
  int checks = m->checks;
  ccord_free(m);
  if (rc != 0) return rc;
  printf("multi-camera: PASS checks=%d\n", checks);
  fflush(stdout);
  return 0;
}
