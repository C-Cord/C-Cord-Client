// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// A voice message is an audio attachment with a waveform (Discord sends one with every voice message).
static bool decor_is_voice(const ccord_attachment *a)
{
  return a && ccord_attachment_is_audio(a) && a->waveform_b64 && a->waveform_b64[0] != '\0';
}

// Waveform player for a voice message: play button, one bar per waveform byte (played bars in brand), duration.
// Height is the audio card's 58px; the bars are the scrub area, so ui_media_hit seeks like any player.
static void draw_voice_message_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  const ccord_attachment *a = p->att;
  float cw = clampf(w, 140.0f, 460.0f);
  float h = dc_media_player_h(a, w);
  ccord_rect card = {x, y, cw, h};
  ccord_id skey = dc_spoiler_att_key(p->msg_id, a, 0);
  if (a->spoiler && !dc_is_spoiler_revealed(ui, skey)) {
    dc_draw_spoiler_tile(ui, r, card, skey);
    return;
  }
  ui->vm.dbg_cards++;

  const char *url = dc_attachment_url(a);
  ccord_id att_id = a->id != CCORD_ID_NONE ? a->id : (p->msg_id ^ ui_name_id(url));
  bool active = (ui->media_att_id == att_id && url[0] && strcmp(ui->media_url, url) == 0);
  bool playing = active && ui->media_playing;
  uint32_t dur_ms = (active && ui->media_duration_ms > 0) ? ui->media_duration_ms : a->duration_ms;
  uint32_t pos_ms = active ? ui->media_pos_ms : 0u;
  if (pos_ms > dur_ms) pos_ms = dur_ms;
  float frac = dur_ms > 0 ? clampf((float)pos_ms / (float)dur_ms, 0.0f, 1.0f) : 0.0f;

  ccord_draw_rrect(r, card, 6.0f, COL(bg_secondary));

  ccord_rect play_r = {x + 8.0f, y + (h - 24.0f) * 0.5f, 24.0f, 24.0f};
  ccord_draw_circle(r, play_r.x + 12.0f, play_r.y + 12.0f, 12.0f, COL(brand));
  if (playing) {
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 8.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 13.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
  } else {
    ccord_draw_triangle(r, (ccord_vec2){play_r.x + 9.5f, play_r.y + 7.0f},
                        (ccord_vec2){play_r.x + 9.5f, play_r.y + 17.0f},
                        (ccord_vec2){play_r.x + 17.5f, play_r.y + 12.0f}, COL(text_on_brand));
  }

  char t_lbl[16];
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
  dc_format_mmss(active ? pos_ms : dur_ms, t_lbl, sizeof t_lbl);
  ccord_vec2 tw = ccord_text_measure(r, t_lbl, strlen(t_lbl), &ts, 0.0f);
  float time_x = x + cw - tw.x - 10.0f;
  ccord_text_draw(r, t_lbl, strlen(t_lbl), time_x, y + (h - tw.y) * 0.5f, &ts, 0.0f);

  float bx = play_r.x + play_r.w + 10.0f;
  ccord_rect bars = {bx, y + 12.0f, maxf(time_x - 10.0f - bx, 12.0f), h - 24.0f};
  uint8_t wv[UI_VM_BARS];
  size_t nw = ui_vm_waveform_bars(a->waveform_b64, wv, sizeof wv);
  size_t nb = (size_t)((bars.w + 1.0f) / 3.0f);
  for (size_t i = 0; i < nb; i++) {
    uint8_t v = nw > 0 ? wv[i * nw / nb] : 0;
    float bh = 3.0f + (float)v / 255.0f * (bars.h - 3.0f);
    bool played = (((float)i + 0.5f) / (float)nb) <= frac;
    ccord_draw_rrect(r, (ccord_rect){bars.x + (float)i * 3.0f, bars.y + (bars.h - bh) * 0.5f, 2.0f, bh}, 1.0f,
                     played ? COL(brand) : COL(text_muted));
  }

  if (url[0] && ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
    ui_media_hit *mh = &ui->media_hits[ui->nmedia_hits++];
    mh->play_rect = play_r;
    mh->scrub_rect = bars;
    mh->open_rect = (ccord_rect){0, 0, 0, 0};
    mh->att_id = att_id;
    mh->duration_ms = a->duration_ms;
    mh->url = strlen(url) < UI_URL_MAX ? ui_hit_cstr(ui, url) : "";
    mh->needs_resolve = false;
  }
}


// A video the inline player can show in a picture box.
typedef struct video_src {
  const char *url;         // the file (or page URL when needs_resolve) to play; "" = poster-only link
  const char *poster;      // still shown before playback, "" = none
  const char *poster_alt;  // asked for when `poster` fails to load
  ccord_id id;             // player identity (attachment id, or a hash for embeds)
  uint32_t duration_ms;    // 0 = unknown until the file is open
  bool needs_resolve;      // true for third-party video pages (YouTube, X, etc.) resolved on click
} video_src;

static void draw_overlay_pill(ccord_ui *ui, ccord_renderer *r, ccord_rect vp, const char *text)
{
  ccord_text_style os = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, (ccord_color){255, 255, 255, 255});
  size_t tl = strlen(text);
  ccord_vec2 sz = ccord_text_measure(r, text, tl, &os, 0.0f);
  float pw = fminf(sz.x + 20.0f, maxf(vp.w - 8.0f, 16.0f)), ph = sz.y + 10.0f;
  ccord_rect pill = {vp.x + (vp.w - pw) * 0.5f, vp.y + (vp.h - ph) * 0.5f, pw, ph};
  ccord_draw_rrect(r, pill, ph * 0.5f, (ccord_color){0, 0, 0, 190});
  ccord_text_draw_ellipsis(r, text, tl, pill.x + 10.0f, pill.y + 5.0f, maxf(pw - 20.0f, 8.0f), &os);
}

// The picture box of a video: the playing frame (letterboxed, never stretched), else the poster, with a play
// button, a loading or error note, and - with `overlay` - its own scrub bar and click targets along the bottom.
static void draw_video_surface(ccord_ui *ui, ccord_renderer *r, ccord_rect vp, float radius, const video_src *v,
                               bool overlay)
{
  bool can_play = v->url && v->url[0];
  bool active = can_play && ui->media_att_id == v->id && strcmp(ui->media_url, v->url) == 0;
  bool playing = active && ui->media_playing;
  ccord_draw_rrect(r, vp, radius, (ccord_color){0, 0, 0, 255});

  bool drew = false;
  if (active && ui->media_stream) {
    const ccord_av_info *inf = ccord_av_stream_info(ui->media_stream);
    uint32_t serial = 0;
    const uint8_t *px = ccord_av_stream_frame(ui->media_stream, &serial);
    if (px && inf->w > 0 && inf->h > 0) {
      bool have = serial == ui->media_frame_serial && ccord_render_has_texture(r, UI_VIDEO_TEX_KEY, nullptr, nullptr);
      if (!have) {
        ccord_error verr;
        have = ccord_render_upload_rgba(r, UI_VIDEO_TEX_KEY, px, inf->w, inf->h, &verr) == CCORD_OK;
        if (have) ui->media_frame_serial = serial;
        else ccord_error_clear(&verr);
      }
      if (have) {
        ccord_rect fr = dc_fit_rect(vp, (float)inf->w, (float)inf->h);
        bool full = fr.w >= vp.w - 1.0f && fr.h >= vp.h - 1.0f;
        ccord_draw_texture_rounded(r, UI_VIDEO_TEX_KEY, fr, full ? radius : 0.0f, 255);
        ui->dbg_attachment_textures_drawn++;
        drew = true;
      }
    }
  }
  if (!drew) {
    const char *ps = dc_media_pick(ui, r, v->poster, v->poster_alt);
    int tw = 0, th = 0;
    if (ps && ccord_render_has_texture(r, ps, &tw, &th) && tw > 0 && th > 0) {
      ccord_rect fr = dc_fit_rect(vp, (float)tw, (float)th);
      bool full = fr.w >= vp.w - 1.0f && fr.h >= vp.h - 1.0f;
      ccord_draw_texture_rounded_anim(r, ps, fr, full ? radius : 0.0f, 255, false);
      ui->dbg_attachment_textures_drawn++;
      drew = true;
    }
  }

  if (active && ui->media_failed) {
    draw_overlay_pill(ui, r, vp, ui->media_fail_hint[0] ? ui->media_fail_hint : "Couldn't load - click to retry");
  } else if (playing && (ui->media_resolving || ui->media_buffering || !ui->media_loaded)) {
    draw_overlay_pill(ui, r, vp, "Loading...");
  } else if (!playing) {
    float cx = vp.x + vp.w * 0.5f, cy = vp.y + vp.h * 0.5f;
    ccord_draw_circle(r, cx, cy, 22.0f, ui_alpha(drew ? COL(overlay) : COL(brand), 0.86f));
    ccord_draw_triangle(r, (ccord_vec2){cx - 6.0f, cy - 9.0f}, (ccord_vec2){cx - 6.0f, cy + 9.0f},
                        (ccord_vec2){cx + 10.0f, cy}, COL(text_on_brand));
  }
  if (!overlay || !can_play) return;

  ccord_rect play_r = vp, scrub_r = {0, 0, 0, 0};
  if (active && ui->media_loaded && vp.h > 60.0f) {
    uint32_t dur_ms = ui->media_duration_ms, pos_ms = ui->media_pos_ms;
    if (pos_ms > dur_ms) pos_ms = dur_ms;
    float frac = dur_ms > 0 ? clampf((float)pos_ms / (float)dur_ms, 0.0f, 1.0f) : 0.0f;
    const float bar_h = 26.0f;
    ccord_rect bar = {vp.x, vp.y + vp.h - bar_h, vp.w, bar_h};
    play_r.h -= bar_h;
    ccord_draw_rect(r, bar, (ccord_color){0, 0, 0, 150});
    char t_cur[16], t_tot[16], t_lbl[40];
    dc_format_mmss(pos_ms, t_cur, sizeof t_cur);
    dc_format_mmss(dur_ms, t_tot, sizeof t_tot);
    snprintf(t_lbl, sizeof t_lbl, "%s / %s", t_cur, t_tot);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, (ccord_color){255, 255, 255, 230});
    ccord_vec2 tw = ccord_text_measure(r, t_lbl, strlen(t_lbl), &ts, 0.0f);
    float time_x = bar.x + bar.w - tw.x - 8.0f;
    ccord_text_draw(r, t_lbl, strlen(t_lbl), time_x, bar.y + (bar.h - tw.y) * 0.5f, &ts, 0.0f);
    float sx = bar.x + 10.0f, sw = maxf(time_x - 10.0f - sx, 28.0f);
    scrub_r = (ccord_rect){sx, bar.y + 4.0f, sw, bar.h - 8.0f};
    ccord_rect track = {sx, bar.y + bar.h * 0.5f - 2.0f, sw, 4.0f};
    ccord_draw_rrect(r, track, 2.0f, (ccord_color){255, 255, 255, 70});
    if (frac > 0.0f) ccord_draw_rrect(r, (ccord_rect){sx, track.y, maxf(sw * frac, 4.0f), 4.0f}, 2.0f, COL(brand));
    ccord_draw_circle(r, sx + sw * frac, track.y + 2.0f, 5.0f, (ccord_color){255, 255, 255, 255});
  }
  if (ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
    ui_media_hit *mh = &ui->media_hits[ui->nmedia_hits++];
    mh->play_rect = play_r;
    mh->scrub_rect = scrub_r;
    mh->open_rect = (ccord_rect){0, 0, 0, 0};
    mh->att_id = v->id;
    mh->duration_ms = v->duration_ms;
    mh->url = (v->url && strlen(v->url) < UI_URL_MAX) ? ui_hit_cstr(ui, v->url) : "";
    mh->needs_resolve = v->needs_resolve;
  }
}

float dc_draw_media_player_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  const ccord_attachment *a = p ? p->att : nullptr;
  if (!a) return 0.0f;
  float h = dc_media_player_h(a, w);
  if (decor_is_voice(a)) {
    draw_voice_message_card(ui, r, p, x, y, w);
    return h;
  }
  float cw = dc_media_player_w(a, w);
  ccord_rect card = {x, y, cw, h};
  ccord_id skey = dc_spoiler_att_key(p->msg_id, a, 0);
  if (a->spoiler && !dc_is_spoiler_revealed(ui, skey)) {
    dc_draw_spoiler_tile(ui, r, card, skey);
    return h;
  }

  const char *url = dc_attachment_url(a);
  ccord_id att_id = a->id != CCORD_ID_NONE ? a->id : (p->msg_id ^ ui_name_id(url));
  bool active = (ui->media_att_id == att_id && url[0] && strcmp(ui->media_url, url) == 0);
  bool playing = active && ui->media_playing;
  uint32_t dur_ms = (active && ui->media_duration_ms > 0) ? ui->media_duration_ms
                                                          : (a->duration_ms > 0 ? a->duration_ms : 30000u);
  uint32_t pos_ms = active ? ui->media_pos_ms : 0u;
  if (pos_ms > dur_ms) pos_ms = dur_ms;
  float frac = dur_ms > 0 ? clampf((float)pos_ms / (float)dur_ms, 0.0f, 1.0f) : 0.0f;

  ccord_draw_rrect(r, card, 6.0f, COL(bg_secondary));
  ccord_draw_rect(r, (ccord_rect){x, y, cw, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){x, y + h - 1.0f, cw, 1.0f}, COL(border_subtle));

  bool is_video = ccord_attachment_is_video(a);
  ccord_rect vp = {0, 0, 0, 0};
  if (is_video) {
    vp = (ccord_rect){x + 6.0f, y + 6.0f, cw - 12.0f, h - 6.0f - 2.0f - MEDIA_CONTROLS_H};
    // The media proxy renders a video's first frame when asked for an image: that is the poster.
    char pbuf[UI_URL_MAX];
    const char *poster = "";
    if (a->proxy_url && a->proxy_url[0] &&
        ui_media_sized_url(a->proxy_url, a->width, a->height, 800u, 600u, false, pbuf, sizeof pbuf)) {
      poster = pbuf;
    }
    video_src vs = {.url = url, .poster = poster, .poster_alt = "", .id = att_id, .duration_ms = a->duration_ms};
    draw_video_surface(ui, r, vp, 5.0f, &vs, false);
  }

  float cy_top = is_video ? (vp.y + vp.h + 2.0f) : y;
  size_t nl = 0;
  const char *name = dc_attachment_label(a, &nl);
  char sz_buf[32] = "";
  if (a->size > 0) dc_format_size(a->size, sz_buf, sizeof sz_buf);
  char hdr[224];
  bool failed = active && ui->media_failed;
  snprintf(hdr, sizeof hdr, "%.*s%s%s%s", (int)(nl < 120u ? nl : 120u), name, sz_buf[0] ? "  " : "", sz_buf,
           (failed && !is_video) ? "  -  couldn't load" : "");

  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_link));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));

  ccord_vec2 open_sz = ccord_text_measure(r, "Open", 4, &ls, 0.0f);
  ccord_rect open_r = {x + cw - open_sz.x - 18.0f, cy_top + 4.0f, open_sz.x + 12.0f, 18.0f};
  ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x + 10.0f, cy_top + 5.0f, maxf(open_r.x - (x + 16.0f), 32.0f), &ns);
  if (url[0]) {
    ccord_draw_rrect(r, open_r, 4.0f, COL(bg_tertiary));
    ccord_text_draw(r, "Open", 4, open_r.x + 6.0f, open_r.y + (open_r.h - open_sz.y) * 0.5f, &ls, 0.0f);
  }

  ccord_rect play_r = {x + 8.0f, cy_top + 26.0f, 24.0f, 24.0f};
  ccord_draw_circle(r, play_r.x + 12.0f, play_r.y + 12.0f, 12.0f, COL(brand));
  if (playing) {
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 8.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
    ccord_draw_rrect(r, (ccord_rect){play_r.x + 13.0f, play_r.y + 7.0f, 3.0f, 10.0f}, 1.0f, COL(text_on_brand));
  } else {
    ccord_draw_triangle(r, (ccord_vec2){play_r.x + 9.5f, play_r.y + 7.0f},
                        (ccord_vec2){play_r.x + 9.5f, play_r.y + 17.0f},
                        (ccord_vec2){play_r.x + 17.5f, play_r.y + 12.0f}, COL(text_on_brand));
  }

  char t_cur[16], t_tot[16], t_lbl[40];
  dc_format_mmss(pos_ms, t_cur, sizeof t_cur);
  dc_format_mmss(dur_ms, t_tot, sizeof t_tot);
  snprintf(t_lbl, sizeof t_lbl, "%s / %s", t_cur, t_tot);
  ccord_vec2 tw = ccord_text_measure(r, t_lbl, strlen(t_lbl), &ts, 0.0f);
  float time_x = x + cw - tw.x - 10.0f;
  ccord_text_draw(r, t_lbl, strlen(t_lbl), time_x, play_r.y + (play_r.h - tw.y) * 0.5f, &ts, 0.0f);

  float sx = play_r.x + play_r.w + 10.0f;
  float sw = maxf(time_x - 10.0f - sx, 28.0f);
  ccord_rect scrub_r = {sx, play_r.y + 4.0f, sw, 16.0f};
  ccord_rect track_r = {sx, play_r.y + 10.0f, sw, 4.0f};
  ccord_draw_rrect(r, track_r, 2.0f, COL(bg_tertiary));
  if (frac > 0.0f) {
    ccord_draw_rrect(r, (ccord_rect){sx, track_r.y, maxf(sw * frac, 4.0f), 4.0f}, 2.0f, COL(brand));
  }
  ccord_draw_circle(r, sx + sw * frac, track_r.y + 2.0f, 5.0f, COL(text_header));

  if (url[0] && ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
    ui_media_hit *mh = &ui->media_hits[ui->nmedia_hits++];
    mh->play_rect = play_r;
    mh->scrub_rect = scrub_r;
    mh->open_rect = open_r;
    mh->att_id = att_id;
    mh->duration_ms = a->duration_ms;
    mh->url = strlen(url) < UI_URL_MAX ? ui_hit_cstr(ui, url) : "";
    mh->needs_resolve = false;
    if (is_video && ui->nmedia_hits < UI_MEDIA_HIT_MAX) {
      ui_media_hit *vh = &ui->media_hits[ui->nmedia_hits++];
      *vh = *mh;
      vh->play_rect = vp;
      vh->scrub_rect = (ccord_rect){0, 0, 0, 0};
      vh->open_rect = (ccord_rect){0, 0, 0, 0};
    }
  }
  return h;
}

// Streamer Mode: the embed string to draw — a masked copy in buf when invite masking replaced
// an invite code, else the original. *len receives the drawn byte length.
static const char *embed_str_masked(ccord_ui *ui, const char *s, char *buf, size_t cap, bool draw, size_t *len)
{
  size_t n = s ? strlen(s) : 0;
  size_t ml = ui_streamer_mask_invites(ui, s, n, buf, cap, draw);
  *len = ml > 0 ? ml : n;
  return ml > 0 ? buf : (s ? s : "");
}

float dc_embed_card_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw)
{
  const ccord_embed *e = p ? p->emb : nullptr;
  if (!e) return 0.0f;

  bool has_title = e->title && e->title[0];
  bool has_desc = e->description && e->description[0];
  bool has_prov = e->provider && e->provider[0];
  bool has_auth = e->author_name && e->author_name[0];
  bool has_foot = (e->footer_text && e->footer_text[0]) || e->timestamp_ms > 0;
  const char *img_url = dc_embed_media_url(&e->image);
  const char *thumb_url = dc_embed_media_url(&e->thumbnail);
  const char *vid_url = dc_embed_video_url(e);
  bool has_vid = (e->video.url && e->video.url[0]) || (e->video.proxy_url && e->video.proxy_url[0]);

  // Pure image / GIF / GIFV embed rendered framelessly (no embed border, provider header, or video-card chrome).
  if (dc_embed_is_bare_media(e)) {
    bool is_gif = dc_embed_is_gif_like(e);
    const char *clip_fb = "";
    const char *clip_url = is_gif ? dc_embed_gif_clip_url(e, &clip_fb) : "";
    const char *pic = img_url[0] ? img_url : (thumb_url[0] ? thumb_url : (e->url ? e->url : ""));
    uint32_t mw = 0, mh = 0;
    if (is_gif && clip_url[0] && e->video.width > 0 && e->video.height > 0) {
      mw = e->video.width;
      mh = e->video.height;
    } else if (e->image.width > 0 && e->image.height > 0) {
      mw = e->image.width;
      mh = e->image.height;
    } else if (e->thumbnail.width > 0 && e->thumbnail.height > 0) {
      mw = e->thumbnail.width;
      mh = e->thumbnail.height;
    } else {
      mw = e->video.width;
      mh = e->video.height;
    }
    if ((mw == 0 || mh == 0) && r) {
      int tw_px = 0, th_px = 0;
      const char *probe = (clip_url[0] && ccord_render_has_texture(r, clip_url, &tw_px, &th_px))
                            ? clip_url
                            : (pic[0] && ccord_render_has_texture(r, pic, &tw_px, &th_px) ? pic : nullptr);
      if (probe && tw_px > 4 && th_px > 4) {
        mw = (uint32_t)tw_px;
        mh = (uint32_t)th_px;
      }
    }
    float max_w = clampf(w, 80.0f, 400.0f), max_h = 300.0f;
    float iw = fminf(max_w, 260.0f), ih = 140.0f;
    if (mw > 0 && mh > 0) {
      float sc = fminf(1.0f, fminf(max_w / (float)mw, max_h / (float)mh));
      iw = clampf((float)mw * sc, 96.0f, max_w);
      ih = clampf((float)mh * sc, 72.0f, max_h);
    }
    if (draw) {
      ccord_rect box = {x, y, iw, ih};
      ccord_draw_rrect(r, box, 6.0f, COL(bg_secondary));
      // A "GIF" from Tenor, Klipy, Giphy or Imgur is a short silent clip or animated image: it plays inline as-is.
      bool gifv = is_gif && clip_url[0] != '\0';
      bool animate = ui_media_animate(ui, box);
      char sbuf[UI_URL_MAX];
      const char *still = pic;
      if (ui_media_sized_url(pic, mw, mh, 800u, 600u, (is_gif && !gifv) || dc_url_is_anim_image(pic), sbuf, sizeof sbuf)) {
        still = sbuf;
      }
      const char *shown = nullptr;
      const char *active_clip = "";
      if (gifv && animate) {
        if (ui_ensure_media_ex(ui, r, clip_url, 480, true)) {
          shown = clip_url;
          active_clip = clip_url;
        } else if (clip_fb[0] && ui_media_failed(ui, clip_url) && ui_ensure_media_ex(ui, r, clip_fb, 480, true)) {
          shown = clip_fb;
          active_clip = clip_fb;
        }
      }
      bool moving = shown != nullptr;
      if (!shown) shown = dc_media_pick(ui, r, still, pic);
      if (shown) {
        if (!moving && animate && r && ccord_render_texture_is_animated(r, shown, nullptr, nullptr)) moving = true;
        ccord_draw_texture_cover(r, shown, box, 6.0f, 255, animate);
        ui->dbg_attachment_textures_drawn++;
      }
      if (is_gif && !moving) {
        ccord_text_style gs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_header));
        ccord_vec2 gt = ccord_text_measure(r, "GIF", 3, &gs, 0.0f);
        ccord_rect gp = {box.x + 8.0f, box.y + 8.0f, gt.x + 10.0f, 16.0f};
        ccord_draw_rrect(r, gp, 3.0f, ui_alpha(COL(overlay), 0.75f));
        ccord_text_draw(r, "GIF", 3, gp.x + 5.0f, gp.y + (gp.h - gt.y) * 0.5f, &gs, 0.0f);
      }
      const char *big = active_clip[0] ? active_clip : (gifv ? clip_url : pic);
      if (big[0]) {
        const char *one_url = big;
        const char *one_prev = (shown && shown[0]) ? shown : still;
        uint32_t ow = mw, oh = mh;
        dc_img_hit_extra extra = {
          .preview_urls = &one_prev,
          .orig_w = &ow,
          .orig_h = &oh,
          .msg_id = p->msg_id,
          .author_id = p->author_id,
        };
        dc_record_img_hit_ex(ui, box, &one_url, 1u, 0u, &extra);
      }
    }
    return ih;
  }

  float cw = clampf(w, 120.0f, 432.0f);
  float pad_l = 14.0f, pad_r = 12.0f, pad_v = 8.0f;
  float full_w = maxf(cw - pad_l - pad_r, 48.0f);
  const char *resolve_url = vid_url[0] ? "" : dc_embed_resolvable_video_url(e);
  const char *play_url = vid_url[0] ? vid_url : resolve_url;
  bool needs_resolve = !vid_url[0] && resolve_url[0] != '\0';
  // The wide slot under the text holds, in this order of preference: the embed's video (its thumbnail is then the
  // poster), its image, or - for an article - the thumbnail as a lead picture. Only a thumbnail that is none of
  // these sits small in the corner.
  bool video_box = (has_vid || resolve_url[0]) && (play_url[0] || thumb_url[0] || img_url[0]);
  bool lead_thumb = !video_box && !img_url[0] && thumb_url[0] && e->kind == CCORD_EMBED_ARTICLE;
  const char *big_url = video_box ? "" : (img_url[0] ? img_url : (lead_thumb ? thumb_url : ""));
  const ccord_embed_media *big_m = img_url[0] ? &e->image : &e->thumbnail;
  bool has_thumb = thumb_url[0] != '\0' && !video_box && !lead_thumb &&
                   (!img_url[0] || strcmp(thumb_url, img_url) != 0);
  float thumb_w = 80.0f, thumb_h = 80.0f;
  if (has_thumb && e->thumbnail.width > 0 && e->thumbnail.height > 0) {
    float sc = fminf(80.0f / (float)e->thumbnail.width, 80.0f / (float)e->thumbnail.height);
    thumb_w = clampf((float)e->thumbnail.width * sc, 40.0f, 80.0f);
    thumb_h = clampf((float)e->thumbnail.height * sc, 40.0f, 80.0f);
  }
  // Player pages (YouTube and the like) come with a long page description Discord leaves out; X/Twitter posts and
  // tweet fixers keep their text readable above the video.
  if (video_box && !vid_url[0] && has_title && (!needs_resolve || !dc_url_is_x_host(play_url))) has_desc = false;
  float top_w = has_thumb ? maxf(full_w - thumb_w - 12.0f, 48.0f) : full_w;
  float tx = x + pad_l;
  float cy = y + pad_v;
  char emb_mask[CCORD_MESSAGE_MAX_BYTES + 1];   // streamer-mode invite mask scratch (one string at a time)

  struct ui_chat_caches *cc = dc_chat_caches_ensure(ui);
  int emb_wq = (int)lroundf(w * 2.0f);
  float emb_zoom = r ? ccord_render_get_zoom(r) : 1.0f;
  int emb_sq = (int)lroundf(emb_zoom * 100.0f) * 256 + (int)lroundf(M(font_size_base) * 4.0f);
  bool emb_streamer = ui_streamer_invites_active(ui);
  if (cc) dc_rowh_cache_sync(cc, ui, r);

  if (draw) {
    float total_h = 0.0f;
    bool hit_cached = false;
    if (cc && p->msg_id != CCORD_ID_NONE) {
      uintptr_t ep = (uintptr_t)e;
      for (size_t k = 0; k < CHAT_EMBEDH_CACHE_SLOTS; k++) {
        chat_embedh_entry *ce = &cc->embedh[k];
        if (ce->msg_id == p->msg_id && ce->edit_rev == p->edit_rev && ce->emb_ptr == ep && ce->width_q == emb_wq &&
            ce->scale_q == emb_sq && ce->streamer == emb_streamer) {
          ce->lru_tick = ++cc->rh_tick;
          total_h = ce->height;
          hit_cached = true;
          break;
        }
      }
    }
    if (!hit_cached) total_h = dc_embed_card_walk(ui, r, p, x, y, w, false);
    ccord_draw_rrect(r, (ccord_rect){x, y, cw, total_h}, 4.0f, COL(bg_secondary));
    ccord_color stripe = e->has_color ? ui_rgb_color(e->color) : COL(brand);
    ccord_draw_rrect(r, (ccord_rect){x, y, 4.0f, total_h}, 2.0f, stripe);
    if (has_thumb) {
      ccord_rect tr = {x + cw - pad_r - thumb_w, y + pad_v, thumb_w, thumb_h};
      ccord_draw_rrect(r, tr, 4.0f, COL(bg_tertiary));
      char sbuf[UI_URL_MAX];
      const char *small = thumb_url;
      if (ui_media_sized_url(thumb_url, e->thumbnail.width, e->thumbnail.height, 160u, 160u,
                             dc_url_is_anim_image(thumb_url), sbuf, sizeof sbuf)) {
        small = sbuf;
      }
      const char *shown = dc_media_pick(ui, r, small, thumb_url);
      if (shown) {
        ccord_draw_texture_cover(r, shown, tr, 4.0f, 255, ui_media_animate(ui, tr));
        ui->dbg_attachment_textures_drawn++;
      }
      const char *one_thumb = thumb_url;
      const char *one_prev = (shown && shown[0]) ? shown : small;
      uint32_t ow = e->thumbnail.width, oh = e->thumbnail.height;
      dc_img_hit_extra extra = {
        .preview_urls = &one_prev,
        .orig_w = &ow,
        .orig_h = &oh,
        .msg_id = p->msg_id,
        .author_id = p->author_id,
      };
      dc_record_img_hit_ex(ui, tr, &one_thumb, 1u, 0u, &extra);
    }
  }

  if (has_prov) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    float ph = ui_line_h(ui, ps.size);
    if (draw) {
      size_t pl = 0;
      const char *prov = embed_str_masked(ui, e->provider, emb_mask, sizeof emb_mask, draw, &pl);
      ccord_text_draw_ellipsis(r, prov, pl, tx, cy, top_w, &ps);
    }
    cy += ph + 2.0f;
  }

  if (has_auth) {
    ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
    float ah = ui_line_h(ui, as.size);
    bool has_icon = e->author_icon && e->author_icon[0];
    float row_h = maxf(ah, has_icon ? 18.0f : 0.0f);
    if (draw) {
      float ax = tx;
      if (has_icon) {
        ccord_rect ir = {ax, cy + (row_h - 18.0f) * 0.5f, 18.0f, 18.0f};
        ccord_draw_rrect(r, ir, 9.0f, COL(bg_tertiary));
        if (ui_ensure_media(ui, r, e->author_icon)) {
          ccord_draw_texture_rounded(r, e->author_icon, ir, 9.0f, 255);
          ui->dbg_attachment_textures_drawn++;
        }
        ax += 24.0f;
      }
      float max_aw = maxf(top_w - (ax - tx), 24.0f);
      size_t anl = 0;
      const char *aname = embed_str_masked(ui, e->author_name, emb_mask, sizeof emb_mask, draw, &anl);
      float drawn_w = ccord_text_draw_ellipsis(r, aname, anl, ax, cy + (row_h - ah) * 0.5f, max_aw, &as);
      if (e->author_url && e->author_url[0]) {
        dc_record_link_hit(ui, (ccord_rect){ax, cy, maxf(drawn_w, 16.0f), row_h}, e->author_url, strlen(e->author_url));
      }
    }
    cy += row_h + 3.0f;
  }

  const char *shown_title = has_title ? e->title
                            : ((!has_desc && e->field_count == 0 && !img_url[0] && !video_box && e->url && e->url[0])
                                 ? e->url : "");
  if (shown_title[0]) {
    // Invite embeds: the title (or the invite URL shown as title) gets its code masked, and a
    // masked invite is no longer a clickable link.
    size_t title_ml = ui_streamer_mask_invites(ui, shown_title, strlen(shown_title), emb_mask, sizeof emb_mask, draw);
    bool title_masked = title_ml > 0;
    size_t title_len = title_masked ? title_ml : strlen(shown_title);
    if (title_masked) shown_title = emb_mask;
    bool is_link = e->url && e->url[0];
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), is_link ? COL(text_link) : COL(text_header));
    float tw_lim = (has_thumb && cy < y + pad_v + thumb_h) ? top_w : full_w;
    ccord_vec2 tsz = ccord_text_measure(r, shown_title, title_len, &ts, tw_lim);
    float th = maxf(tsz.y, ui_line_h(ui, ts.size));
    if (draw) {
      ccord_text_draw(r, shown_title, title_len, tx, cy, &ts, tw_lim);
      if (is_link && !title_masked) {
        dc_record_link_hit(ui, (ccord_rect){tx, cy, fminf(maxf(tsz.x, 24.0f), tw_lim), th}, e->url, strlen(e->url));
      }
    }
    cy += th + 4.0f;
  }

  if (has_desc) {
    float dw = (has_thumb && cy < y + pad_v + thumb_h) ? top_w : full_w;
    ccord_id prev_id = ui->current_drawing_msg_id;
    ui->current_drawing_msg_id = p->msg_id;
    float dh = dc_md_body_walk(ui, r, e->description, strlen(e->description), tx, cy, dw, COL(text_normal), draw);
    ui->current_drawing_msg_id = prev_id;
    cy += dh + 4.0f;
  }

  if (has_thumb && (e->field_count > 0 || big_url[0] || has_foot) && cy < y + pad_v + thumb_h + 4.0f) {
    cy = y + pad_v + thumb_h + 4.0f;
  }

  size_t fi = 0;
  while (fi < e->field_count) {
    size_t cols = 1u;
    if (e->fields[fi].inline_) {
      while (cols < 3u && fi + cols < e->field_count && e->fields[fi + cols].inline_) cols++;
    }
    float col_gap = 8.0f;
    float col_w = cols > 1u ? maxf((full_w - col_gap * (float)(cols - 1u)) / (float)cols, 32.0f) : full_w;
    float row_h = 0.0f;
    for (size_t c = 0; c < cols; c++) {
      const ccord_embed_field *f = &e->fields[fi + c];
      float fx = tx + (float)c * (col_w + col_gap);
      float fy = cy;
      const char *fname = f->name ? f->name : "";
      const char *fval = f->value ? f->value : "";
      if (fname[0]) {
        float nh = dc_md_inline_walk_sized(ui, r, fname, strlen(fname), fx, fy, col_w, COL(text_header),
                                        M(font_size_small) - 1.0f, true, draw);
        fy += nh + 2.0f;
      }
      if (fval[0]) {
        ccord_id prev_id = ui->current_drawing_msg_id;
        ui->current_drawing_msg_id = p->msg_id;
        float vh = dc_md_body_walk(ui, r, fval, strlen(fval), fx, fy, col_w, COL(text_secondary), draw);
        ui->current_drawing_msg_id = prev_id;
        fy += vh;
      }
      if (draw) ui->dbg_embed_fields_drawn++;
      row_h = maxf(row_h, fy - cy);
    }
    cy += row_h + 6.0f;
    fi += cols;
  }

  if (video_box) {
    const ccord_embed_media *poster_m = thumb_url[0] ? &e->thumbnail : &e->image;
    uint32_t mw = e->video.width ? e->video.width : poster_m->width;
    uint32_t mh = e->video.height ? e->video.height : poster_m->height;
    float bw, bh;
    dc_video_box_size(mw, mh, full_w, 300.0f, &bw, &bh);
    if (draw) {
      ccord_rect vp = {tx, cy + 2.0f, bw, bh};
      const char *poster = thumb_url[0] ? thumb_url : img_url;
      char pbuf[UI_URL_MAX];
      const char *psized = poster;
      if (ui_media_sized_url(poster, poster_m->width, poster_m->height, 800u, 600u, false, pbuf, sizeof pbuf)) {
        psized = pbuf;
      }
      video_src vs = {.url = play_url, .poster = psized, .poster_alt = poster,
                      .id = p->msg_id ^ ui_name_id(play_url), .duration_ms = 0, .needs_resolve = needs_resolve};
      draw_video_surface(ui, r, vp, 4.0f, &vs, true);
      // Not playable or resolvable (e.g. non-HTTPS): the picture opens the page in the browser instead.
      if (!play_url[0] && e->url && e->url[0]) dc_record_link_hit(ui, vp, e->url, strlen(e->url));
    }
    cy += bh + 2.0f + 6.0f;
  } else if (big_url[0]) {
    if (p->count > 1u) {
      ccord_attachment tmp_atts[CCORD_RICH_MAX_ATTACHMENTS];
      memset(tmp_atts, 0, sizeof tmp_atts);
      size_t cnt = p->count < CCORD_RICH_MAX_ATTACHMENTS ? p->count : CCORD_RICH_MAX_ATTACHMENTS;
      for (size_t k = 0; k < cnt; k++) {
        tmp_atts[k].url = dc_embed_media_url(&p->emb[k].image);
        tmp_atts[k].proxy_url = tmp_atts[k].url;   // lets the gallery ask the proxy for a tile-sized copy
        tmp_atts[k].content_type = dc_url_is_anim_image(tmp_atts[k].url) ? "image/gif" : "image/png";
        tmp_atts[k].width = p->emb[k].image.width;
        tmp_atts[k].height = p->emb[k].image.height;
      }
      decor_part gp = {.kind = DECOR_GALLERY, .att = tmp_atts, .count = cnt, .msg_id = p->msg_id};
      cy += 2.0f + dc_gallery_walk(ui, r, &gp, tx, cy + 2.0f, full_w, draw) + 4.0f;
    } else {
      uint32_t mw = big_m->width, mh = big_m->height;
      if ((mw == 0 || mh == 0) && r) {
        int tw_px = 0, th_px = 0;
        if (ccord_render_has_texture(r, big_url, &tw_px, &th_px) && tw_px > 4 && th_px > 4) {
          mw = (uint32_t)tw_px;
          mh = (uint32_t)th_px;
        }
      }
      float iw = full_w, ih = 140.0f;
      if (mw > 0 && mh > 0) {
        float sc = fminf(1.0f, fminf(full_w / (float)mw, 300.0f / (float)mh));
        iw = clampf((float)mw * sc, 96.0f, full_w);
        ih = clampf((float)mh * sc, 72.0f, 300.0f);
      }
      if (draw) {
        ccord_rect ir = {tx, cy + 2.0f, iw, ih};
        ccord_draw_rrect(r, ir, 4.0f, COL(bg_tertiary));
        char sbuf[UI_URL_MAX];
        const char *sized = big_url;
        if (ui_media_sized_url(big_url, big_m->width, big_m->height, 800u, 600u, dc_url_is_anim_image(big_url),
                               sbuf, sizeof sbuf)) {
          sized = sbuf;
        }
        const char *shown = dc_media_pick(ui, r, sized, big_url);
        if (shown) {
          ccord_draw_texture_cover(r, shown, ir, 4.0f, 255, ui_media_animate(ui, ir));
          ui->dbg_attachment_textures_drawn++;
        }
        const char *one_big = big_url;
        const char *one_prev = (shown && shown[0]) ? shown : sized;
        uint32_t ow = big_m->width, oh = big_m->height;
        dc_img_hit_extra extra = {
          .preview_urls = &one_prev,
          .orig_w = &ow,
          .orig_h = &oh,
          .msg_id = p->msg_id,
          .author_id = p->author_id,
        };
        dc_record_img_hit_ex(ui, ir, &one_big, 1u, 0u, &extra);
      }
      cy += ih + 6.0f;
    }
  }

  if (has_foot) {
    ccord_text_style fs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    float fh = ui_line_h(ui, fs.size);
    bool has_ficon = e->footer_icon && e->footer_icon[0];
    float row_h = maxf(fh, has_ficon ? 16.0f : 0.0f);
    if (draw) {
      float fx = tx;
      if (has_ficon) {
        ccord_rect fir = {fx, cy + (row_h - 16.0f) * 0.5f, 16.0f, 16.0f};
        ccord_draw_rrect(r, fir, 8.0f, COL(bg_tertiary));
        if (ui_ensure_media(ui, r, e->footer_icon)) {
          ccord_draw_texture_rounded(r, e->footer_icon, fir, 8.0f, 255);
          ui->dbg_attachment_textures_drawn++;
        }
        fx += 22.0f;
      }
      float rem_w = maxf(full_w - (fx - tx), 24.0f);
      char tbuf[48] = "";
      size_t tl = e->timestamp_ms > 0
                    ? ccord_timefmt_message(tbuf, sizeof tbuf, e->timestamp_ms, ui->now_unix_ms, ui->use_utc)
                    : 0u;
      float fy_txt = cy + (row_h - fh) * 0.5f;
      if (e->footer_text && e->footer_text[0]) {
        float reserve = tl > 0 ? (ccord_text_measure(r, tbuf, tl, &fs, 0.0f).x + 18.0f) : 0.0f;
        size_t ftl = 0;
        const char *ftext = embed_str_masked(ui, e->footer_text, emb_mask, sizeof emb_mask, draw, &ftl);
        float fw = ccord_text_draw_ellipsis(r, ftext, ftl, fx, fy_txt, maxf(rem_w - reserve, 24.0f), &fs);
        fx += fw;
        if (tl > 0) {
          ccord_vec2 dt = ccord_text_measure(r, "  -  ", 5, &fs, 0.0f);
          ccord_text_draw(r, "  -  ", 5, fx, fy_txt, &fs, 0.0f);
          fx += dt.x;
        }
      }
      if (tl > 0) {
        ccord_vec2 tw = ccord_text_measure(r, tbuf, tl, &fs, 0.0f);
        ccord_text_draw(r, tbuf, tl, fx, fy_txt, &fs, 0.0f);
        dc_record_ts_hit(ui, (ccord_rect){fx, cy, tw.x, row_h}, e->timestamp_ms, false);
      }
    }
    cy += row_h + 2.0f;
  }

  if (has_thumb && cy < y + pad_v + thumb_h) cy = y + pad_v + thumb_h;
  if (cy <= y + pad_v) cy = y + pad_v + 24.0f;
  float final_h = (cy - y) + pad_v;
  if (cc && p->msg_id != CCORD_ID_NONE) {
    uintptr_t ep = (uintptr_t)e;
    size_t slot = 0;
    bool matched = false;
    for (size_t k = 0; k < CHAT_EMBEDH_CACHE_SLOTS; k++) {
      chat_embedh_entry *ce = &cc->embedh[k];
      if (ce->msg_id == p->msg_id && ce->edit_rev == p->edit_rev && ce->emb_ptr == ep && ce->width_q == emb_wq &&
          ce->scale_q == emb_sq && ce->streamer == emb_streamer) {
        slot = k;
        matched = true;
        break;
      }
      if (ce->msg_id == CCORD_ID_NONE) {
        slot = k;
        break;
      }
      if (ce->lru_tick < cc->embedh[slot].lru_tick) slot = k;
    }
    if (!matched) {
      cc->embedh[slot] = (chat_embedh_entry){
        .msg_id = p->msg_id,
        .edit_rev = p->edit_rev,
        .emb_ptr = ep,
        .width_q = emb_wq,
        .scale_q = emb_sq,
        .streamer = emb_streamer,
        .height = final_h,
        .lru_tick = ++cc->rh_tick,
      };
    } else {
      cc->embedh[slot].height = final_h;
      cc->embedh[slot].lru_tick = ++cc->rh_tick;
    }
  }
  return final_h;
}

