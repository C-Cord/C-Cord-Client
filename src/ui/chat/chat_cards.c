// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

static void draw_reply_avatar_circle(ccord_ui *ui, ccord_renderer *r, const ccord_user *u, float cx, float cy, float size)
{
  float rad = size * 0.5f;
  ccord_rect dst = {cx - rad, cy - rad, size, size};
  char url[256];
  if (u && ui_user_avatar_url(u, url, sizeof url) && ui_ensure_media(ui, r, url)) {
    ccord_draw_texture_rounded(r, url, dst, rad, 255);
    ui->dbg_avatar_textures_drawn++;
    return;
  }
  ccord_color bg = ui_user_color(ui, u ? u->avatar_palette : 0u);
  ccord_draw_circle(r, cx, cy, rad, bg);
  char init[8] = "?";
  if (u && u->name && u->name[0]) {
    size_t l = ccord_utf8_next(u->name, strlen(u->name), 0);
    if (l > 4u) l = 4u;
    memcpy(init, u->name, l);
    init[l] = '\0';
    if (l == 1u && init[0] >= 'a' && init[0] <= 'z') init[0] = (char)(init[0] - 'a' + 'A');
  }
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, size * 0.50f, COL(text_on_brand));
  ccord_vec2 m = ccord_text_measure(r, init, strlen(init), &st, 0.0f);
  ccord_text_draw(r, init, strlen(init), dst.x + (dst.w - m.x) * 0.5f, dst.y + (dst.h - m.y) * 0.5f, &st, 0.0f);
}

static void draw_reply_spine(ccord_renderer *r, float sx, float y_bot, float x_end, float cy, ccord_color col)
{
  const float th = UI_REPLY_SPINE_W;
  float dx = x_end - sx;
  float dy = y_bot - cy;
  if (dx < 6.0f || dy < 4.0f) {
    ccord_draw_line(r, (ccord_vec2){x_end - 8.0f, cy}, (ccord_vec2){x_end, cy}, th, col);
    return;
  }
  float rad = UI_REPLY_SPINE_R;
  if (rad > dx - 4.0f) rad = dx - 4.0f;
  if (rad > dy - 1.0f) rad = dy - 1.0f;
  if (rad < 2.0f) rad = 2.0f;
  // Curved L-connector rising from the replier's avatar (or grouped body line) into the reply pill with a directional tip.
  ccord_draw_circle(r, sx, y_bot, th * 0.5f, col);
  ccord_draw_line(r, (ccord_vec2){sx, y_bot}, (ccord_vec2){sx, cy + rad}, th, col);
  const float cx = sx + rad;
  const float ay = cy + rad;
  const int segs = 6;
  const float k_pi = 3.14159265358979323846f;
  ccord_vec2 prev = {sx, ay};
  for (int i = 1; i <= segs; i++) {
    float a = k_pi + (k_pi * 0.5f) * ((float)i / (float)segs);
    ccord_vec2 cur = {cx + cosf(a) * rad, ay + sinf(a) * rad};
    ccord_draw_line(r, prev, cur, th, col);
    prev = cur;
  }
  ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){x_end, cy}, th, col);
  ccord_draw_line(r, (ccord_vec2){x_end - 3.5f, cy - 3.0f}, (ccord_vec2){x_end, cy}, 1.5f, col);
  ccord_draw_line(r, (ccord_vec2){x_end - 3.5f, cy + 3.0f}, (ccord_vec2){x_end, cy}, 1.5f, col);
}

// One compact reply pill above the message header, connected by a curved spine from the replier's avatar:
// circular 16 px avatar, replied-to author (caption weight/size so it recedes behind the main message) with a
// colon separator, and a one-line snippet of the original. Whole pill is the click target; deleted originals are static.
void dc_draw_reply_line(ccord_ui *ui, ccord_renderer *r, const decor_info *d, ccord_id guild, float sx, float y1,
                            float x, float y, float w)
{
  const float h = UI_REPLY_LINE_H;
  const float cy = y + h * 0.5f;
  const float pad_l = 3.0f;   // concentric with the 16 px circular avatar inside the 22 px capsule pill
  const float pad_r = 9.0f;
  const float av_gap = 5.0f;
  const float colon_gap = 5.0f;
  const float fs = M(font_size_small) - 2.0f;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, fs, COL(text_secondary));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, fs, COL(text_muted));
  const float text_h = fminf(ccord_text_measure(r, "Ag", 2, &ts, 0.0f).y, h - 2.0f);
  const float ty = y + (h - text_h) * 0.5f;
  const float inner_x = x + pad_l;
  const float tx = inner_x + UI_REPLY_AVATAR + av_gap;
  const float max_right = x + w - pad_r;
  bool clickable = d->reply_target_id != CCORD_ID_NONE && !d->reply_deleted;
  bool has_user = !d->reply_deleted && d->reply_author_id != CCORD_ID_NONE;

  // Measure content first so the pill wraps tightly around the quoted message instead of stretching empty across the chat.
  char rtext[sizeof d->reply_text] = "";
  size_t rtl = 0u;
  const char *nm = d->reply_name[0] ? d->reply_name : "a message";
  float name_max = fminf(maxf(max_right - tx, 0.0f), maxf(w * 0.35f, 60.0f));
  float meas_nw = 0.0f, colon_w = 0.0f, content_right = tx;
  bool show_snip = false;
  if (d->reply_deleted) {
    float tw = ccord_text_measure(r, d->reply_text, strlen(d->reply_text), &ts, 0.0f).x;
    content_right = tx + fminf(tw, maxf(max_right - tx, 0.0f));
  } else {
    meas_nw = fminf(ccord_text_measure(r, nm, strlen(nm), &ns, 0.0f).x, name_max);
    colon_w = ccord_text_measure(r, ":", 1, &ts, 0.0f).x;
    memcpy(rtext, d->reply_text, sizeof rtext);
    rtl = ui_streamer_mask_inplace(ui, rtext, strlen(rtext), true);
    float sx2 = tx + meas_nw + colon_w + colon_gap;
    if (rtl > 0u && max_right - sx2 > 24.0f) {
      show_snip = true;
      float icon_w = d->reply_attach_only ? 15.0f : 0.0f;
      float sw = ccord_text_measure(r, rtext, rtl, &ts, 0.0f).x;
      content_right = sx2 + icon_w + fminf(sw, maxf(max_right - sx2 - icon_w, 0.0f));
    } else {
      content_right = tx + meas_nw;
    }
  }
  float pill_w = fminf(w, maxf(content_right - x + pad_r, UI_REPLY_AVATAR + pad_l + pad_r));
  ccord_rect line = {x, y, pill_w, h};

  bool hovered = clickable && ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, line);
  float hv = ui_fade(ui, "replyln", d->msg_id, hovered);

  // Curved spine: rises from above the replier's avatar on full rows, or from beside the first body line on grouped/dense rows.
  ccord_color spine_rest = d->reply_deleted ? ui_alpha(COL(text_muted), 0.45f)
                                            : ui_mix(COL(text_muted), COL(divider), 0.30f);
  ccord_color spine_col = hv > 0.01f ? ui_mix(spine_rest, COL(brand), hv) : spine_rest;
  bool has_avatar_below = y1 > y + h + 1.0f;
  float spine_x = has_avatar_below ? sx : fmaxf(sx, x - 16.0f);
  float spine_y_bot = has_avatar_below ? (y1 - 3.0f) : (y + h + UI_REPLY_LINE_GAP + ui_line_h(ui, M(font_size_base)) * 0.45f);
  draw_reply_spine(r, spine_x, spine_y_bot, x - 4.0f, cy, spine_col);

  // Tinted capsule pill with a crisp 1 px border and subtle brand wash on hover so it stands out over hovered rows.
  ccord_color border_col = hv > 0.01f ? ui_mix(COL(border_subtle), COL(brand), hv * 0.80f) : COL(border_subtle);
  ccord_color fill_col = hv > 0.01f ? ui_mix(COL(bg_secondary), COL(brand), hv * 0.14f) : COL(bg_secondary);
  ccord_rect border_rc = {line.x - 1.0f, line.y - 1.0f, line.w + 2.0f, line.h + 2.0f};
  ccord_draw_rrect(r, border_rc, border_rc.h * 0.5f, border_col);
  ccord_draw_rrect(r, line, line.h * 0.5f, fill_col);

  ccord_rect av = {0, 0, 0, 0};
  if (has_user) {
    const ccord_user *u = ccord_store_user(ui->d.store, d->reply_author_id);
    const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, d->reply_author_id);
    ccord_user ru = {
      .id = d->reply_author_id,
      .name = d->reply_name,
      .handle = (u && u->handle && u->handle[0]) ? u->handle : "",
      .status_text = "",
      .avatar_url = (prof && prof->guild_id == guild && prof->guild_avatar_url && prof->guild_avatar_url[0])
                      ? prof->guild_avatar_url
                      : ((u && u->avatar_url && u->avatar_url[0]) ? u->avatar_url
                         : ((prof && prof->avatar_url) ? prof->avatar_url : "")),
      .presence = CCORD_PRESENCE_OFFLINE,
      .is_bot = u ? u->is_bot : false,
      .avatar_palette = u ? u->avatar_palette : (uint8_t)(d->reply_author_id % 6u),
    };
    av = (ccord_rect){inner_x, cy - UI_REPLY_AVATAR * 0.5f, UI_REPLY_AVATAR, UI_REPLY_AVATAR};
    draw_reply_avatar_circle(ui, r, &ru, av.x + av.w * 0.5f, av.y + av.h * 0.5f, UI_REPLY_AVATAR);
  } else {
    float icx = inner_x + UI_REPLY_AVATAR * 0.5f;
    ccord_draw_circle(r, icx, cy, UI_REPLY_AVATAR * 0.5f, COL(bg_tertiary));
    if (d->reply_deleted) {
      ui_icon_close(r, icx, cy, 9.0f, COL(text_muted));
    } else {
      ui_icon_reply(r, icx, cy, 10.0f, COL(text_muted));
    }
  }

  float pill_inner_right = line.x + line.w - pad_r;
  ccord_rect snip = {0, 0, 0, 0};
  ccord_rect rname_rect = {0, 0, 0, 0};
  if (d->reply_deleted) {
    ts.color = ui_mix(COL(text_muted), COL(text_secondary), 0.25f);
    float tw = ccord_text_draw_ellipsis(r, d->reply_text, strlen(d->reply_text), tx, ty,
                                        maxf(pill_inner_right - tx, 0.0f), &ts);
    snip = (ccord_rect){tx, ty, tw, text_h};
  } else {
    bool has_role = false;
    ccord_color rc = ui_role_color(ui, guild, d->reply_author_id, &has_role);
    ns.color = has_role ? ui_mix(rc, COL(text_secondary), 0.12f * (1.0f - hv))
                        : ui_mix(COL(text_secondary), COL(text_header), hv);
    ts.color = ui_mix(COL(text_muted), COL(text_normal), hv * 0.90f);
    float nw = ccord_text_draw_ellipsis(r, nm, strlen(nm), tx, ty, name_max, &ns);
    if (has_user) rname_rect = (ccord_rect){tx, ty, maxf(nw, 4.0f), text_h};
    if (show_snip) {
      ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, fs, ui_mix(COL(text_muted), COL(text_secondary), hv));
      ccord_text_draw(r, ":", 1, tx + nw, ty, &cs, 0.0f);
      float sx2 = tx + nw + colon_w + colon_gap;
      float avail = maxf(pill_inner_right - sx2, 0.0f);
      if (d->reply_attach_only && avail > 16.0f) {
        ui_icon_file(r, sx2 + 5.5f, cy, 12.0f, ts.color);
        sx2 += 15.0f;
        avail = maxf(pill_inner_right - sx2, 0.0f);
        ts.color = ui_mix(ts.color, COL(text_secondary), 0.30f);
      }
      if (avail > 4.0f) {
        float sw = ccord_text_draw_ellipsis(r, rtext, rtl, sx2, ty, avail, &ts);
        snip = (ccord_rect){sx2, ty, sw, text_h};
      }
    }
  }
  ui->dbg_reply_cards++;
  bool in_area = line.y >= ui->layout.message_area.y - 0.01f &&
                 line.y + line.h <= ui->layout.message_area.y + ui->layout.message_area.h + 0.01f;
  if (clickable && in_area && ui->nreply_hits < UI_REPLY_HIT_MAX) {
    ui->reply_hits[ui->nreply_hits++] = (ui_reply_hit){line, d->reply_target_id, av, snip};
  }
  if (has_user && in_area) {
    if (av.w > 0.0f) dc_record_user_hit(ui, av, d->reply_author_id);
    if (rname_rect.w > 0.0f) dc_record_user_hit(ui, rname_rect, d->reply_author_id);
  }
}

// "2.0 KB" style sizes for the detail line of an attachment.
void dc_format_size(uint64_t bytes, char *out, size_t cap)
{
  if (bytes < 1024u) {
    snprintf(out, cap, "%llu bytes", (unsigned long long)bytes);
    return;
  }
  const char *unit = "KB";
  double v = (double)bytes / 1024.0;
  if (v >= 1024.0) { v /= 1024.0; unit = "MB"; }
  if (v >= 1024.0) { v /= 1024.0; unit = "GB"; }
  snprintf(out, cap, "%.1f %s", v, unit);
}

// The file name to show: as uploaded, or the last part of the link when Discord did not say.
const char *dc_attachment_label(const ccord_attachment *a, size_t *len)
{
  const char *name = a->filename ? a->filename : "";
  if (name[0] == '\0') {
    const char *u = (a->url && a->url[0]) ? a->url : (a->proxy_url ? a->proxy_url : "");
    const char *slash = strrchr(u, '/');
    name = slash ? slash + 1 : u;
    *len = strcspn(name, "?#");
    if (*len == 0) { name = "attachment"; *len = strlen(name); }
    return name;
  }
  *len = strlen(name);
  return name;
}

void dc_draw_attachment_card(ccord_ui *ui, ccord_renderer *r, const ccord_attachment *a, float x, float y, float w)
{
  float h = 46.0f;
  ccord_rect card = {x, y, w, h};
  ccord_draw_rrect(r, card, 4.0f, COL(bg_secondary));
  ccord_draw_rect(r, (ccord_rect){x, y, w, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){x, y + h - 1.0f, w, 1.0f}, COL(border_subtle));
  ccord_rect ib = {x + 6.0f, y + 6.0f, 34.0f, 34.0f};
  ccord_draw_rrect(r, ib, 4.0f, COL(bg_tertiary));
  const char *url = (a->url && a->url[0]) ? a->url : (a->proxy_url ? a->proxy_url : "");
  bool drew_tex = false;
  if (url[0]) {
    dc_record_link_hit(ui, card, url, strlen(url));
    if (ccord_attachment_is_image(a) && !a->spoiler && ui_ensure_media_ex(ui, r, url, 128, false)) {
      ccord_draw_texture_rounded_anim(r, url, ib, 4.0f, 255, ui_media_animate(ui, ib));
      ui->dbg_attachment_textures_drawn++;
      drew_tex = true;
    }
  }
  if (!drew_tex) {
    ui_icon_attachment(r, ib.x + ib.w * 0.5f, ib.y + ib.h * 0.5f, 18.0f, COL(text_secondary));
  }
  size_t nl = 0;
  const char *name = dc_attachment_label(a, &nl);
  char detail[160];
  char size[32] = "";
  if (a->size > 0) dc_format_size(a->size, size, sizeof size);
  const char *type = (a->content_type && a->content_type[0]) ? a->content_type : "";
  snprintf(detail, sizeof detail, "%s%s%s%s", a->spoiler ? "Spoiler  " : "", type, (type[0] && size[0]) ? "  " : "", size);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_link));
  ccord_text_style ds = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, name, nl, x + 48.0f, y + 6.0f, w - 56.0f, &ns);
  const char *shown = detail[0] ? detail : "attachment";
  ccord_text_draw_ellipsis(r, shown, strlen(shown), x + 48.0f, y + 22.0f, w - 56.0f, &ds);
}

const char *dc_attachment_url(const ccord_attachment *a)
{
  if (!a) return "";
  if (a->url && a->url[0]) return a->url;
  return a->proxy_url ? a->proxy_url : "";
}

// Embed media is loaded from Discord's copy (proxy_url): the site it came from is never contacted, and is not
// on the media allowlist anyway.
const char *dc_embed_media_url(const ccord_embed_media *m)
{
  if (!m) return "";
  if (m->proxy_url && m->proxy_url[0]) return m->proxy_url;
  return m->url ? m->url : "";
}

// A video file an embed points at that the inline player can fetch directly: Discord's proxied copy, a direct media
// file link, or a known direct-video CDN / tweet-fixer redirect host (excluding HLS/DASH manifests). "" for player
// pages (YouTube, X/Twitter watch pages, etc.), which resolve via dc_embed_resolvable_video_url().
const char *dc_embed_video_url(const ccord_embed *e)
{
  if (e->video.proxy_url && e->video.proxy_url[0]) return e->video.proxy_url;
  const char *u = e->video.url ? e->video.url : "";
  if (!u[0]) return "";
  if (ui_url_path_ends(u, ".mp4") || ui_url_path_ends(u, ".webm") || ui_url_path_ends(u, ".mov") ||
      ui_url_path_ends(u, ".m4v") || ui_url_path_ends(u, ".mkv") || ui_url_path_ends(u, ".ogv")) {
    return u;
  }
  if (!ui_url_path_ends(u, ".m3u8") && !ui_url_path_ends(u, ".mpd")) {
    static const char *const k_direct_vid_hosts[] = {
      "video.twimg.com", "d.fxtwitter.com", "t.fxtwitter.com", "dl.fxtwitter.com", "d.vxtwitter.com",
    };
    if (dc_url_host_in_list(u, k_direct_vid_hosts, sizeof k_direct_vid_hosts / sizeof k_direct_vid_hosts[0])) return u;
  }
  return "";
}

// Looping clip URL for a GIF/GIFV embed, plus an optional origin fallback when proxy_url fails.
const char *dc_embed_gif_clip_url(const ccord_embed *e, const char **fallback_out)
{
  if (fallback_out) *fallback_out = "";
  if (!e) return "";
  const char *vproxy = (e->video.proxy_url && e->video.proxy_url[0]) ? e->video.proxy_url : "";
  const char *vurl = (e->video.url && e->video.url[0]) ? e->video.url : "";
  bool vurl_ok = vurl[0] && (e->kind == CCORD_EMBED_GIFV || ui_url_path_ends(vurl, ".mp4") ||
                             ui_url_path_ends(vurl, ".webm") || ui_url_path_ends(vurl, ".mov") ||
                             ui_url_path_ends(vurl, ".m4v") || dc_url_is_anim_image(vurl));
  if (vproxy[0]) {
    if (fallback_out && vurl_ok && strcmp(vproxy, vurl) != 0) *fallback_out = vurl;
    return vproxy;
  }
  return vurl_ok ? vurl : "";
}

bool dc_url_is_x_host(const char *u)
{
  static const char *const k_x[] = {
    "x.com", "www.x.com", "mobile.x.com", "twitter.com", "www.twitter.com", "mobile.twitter.com",
    "fxtwitter.com", "www.fxtwitter.com", "vxtwitter.com", "www.vxtwitter.com",
    "fixupx.com", "www.fixupx.com", "fixvx.com", "www.fixvx.com", "twittpr.com", "xcancel.com",
  };
  return dc_url_host_in_list(u, k_x, sizeof k_x / sizeof k_x[0]);
}

// Discord often sends X/Twitter video embeds without populating `embed.video`, putting Twitter's video thumbnail URL
// (`ext_tw_video_thumb/`, `amplify_video_thumb/`, `tweet_video_thumb/`) in `image` or `thumbnail` instead.
static bool embed_is_x_video_thumb(const ccord_embed *e)
{
  if (!e || !e->url || !e->url[0]) return false;
  if (!dc_url_is_x_host(e->url)) return false;
  const char *urls[4] = {e->image.url, e->image.proxy_url, e->thumbnail.url, e->thumbnail.proxy_url};
  for (size_t i = 0; i < 4; i++) {
    if (!urls[i] || !urls[i][0]) continue;
    if (strstr(urls[i], "ext_tw_video_thumb/") || strstr(urls[i], "amplify_video_thumb/") ||
        strstr(urls[i], "tweet_video_thumb/")) {
      return true;
    }
  }
  return false;
}

// Canonical https:// page URL for a third-party video embed (YouTube, X/Twitter, Vimeo, etc.) that needs on-click
// resolution before streaming inline. Returns "" when the embed is not a resolvable video page.
const char *dc_embed_resolvable_video_url(const ccord_embed *e)
{
  if (!e) return "";
  bool has_vid = (e->video.url && e->video.url[0]) || (e->video.proxy_url && e->video.proxy_url[0]);
  if (!has_vid && !embed_is_x_video_thumb(e)) return "";
  if (e->url && strncasecmp(e->url, "https://", 8) == 0) return e->url;
  if (e->video.url && strncasecmp(e->video.url, "https://", 8) == 0) return e->video.url;
  return "";
}

// Formats the media proxy must not convert to a WebP still.
static bool att_keeps_format(const ccord_attachment *a)
{
  const char *ct = a->content_type ? a->content_type : "";
  return strstr(ct, "gif") || strstr(ct, "webp") || strstr(ct, "apng") || dc_url_is_anim_image(dc_attachment_url(a));
}

// What to load for an attachment shown inline: the media proxy's screen-sized copy when there is one (a phone
// photo becomes a few dozen KB instead of several MB), else the file itself.
static const char *att_thumb_url(const ccord_attachment *a, char *buf, size_t cap)
{
  if (a->proxy_url && a->proxy_url[0] &&
      ui_media_sized_url(a->proxy_url, a->width, a->height, 800u, 600u, att_keeps_format(a), buf, cap)) {
    return buf;
  }
  return dc_attachment_url(a);
}

// The loaded texture to draw for media with a preferred source and a fallback, or nullptr while neither is ready.
// Whatever is already loaded is used as it is; the fallback is only asked for once the preferred source has failed.
const char *dc_media_pick_max(ccord_ui *ui, ccord_renderer *r, const char *primary, const char *fallback, int max_dim)
{
  if (!primary) primary = "";
  if (r && primary[0] && ccord_render_has_texture(r, primary, nullptr, nullptr)) return primary;
  if (r && fallback && fallback[0] && ccord_render_has_texture(r, fallback, nullptr, nullptr)) return fallback;
  if (primary[0] && ui_ensure_media_ex(ui, r, primary, max_dim, false)) return primary;
  if (fallback && fallback[0] && strcmp(primary, fallback) != 0 && (!primary[0] || ui_media_failed(ui, primary)) &&
      ui_ensure_media_ex(ui, r, fallback, max_dim, false)) {
    return fallback;
  }
  return nullptr;
}

const char *dc_media_pick(ccord_ui *ui, ccord_renderer *r, const char *primary, const char *fallback)
{
  return dc_media_pick_max(ui, r, primary, fallback, 0);
}

// Fits a w x h picture inside box, centred, keeping its shape.
ccord_rect dc_fit_rect(ccord_rect box, float w, float h)
{
  if (w <= 0.0f || h <= 0.0f) return box;
  float sc = fminf(box.w / w, box.h / h);
  float fw = roundf(w * sc), fh = roundf(h * sc);
  return (ccord_rect){box.x + floorf((box.w - fw) * 0.5f), box.y + floorf((box.h - fh) * 0.5f), fw, fh};
}

void dc_format_mmss(uint32_t ms, char *out, size_t cap)
{
  uint32_t sec = ms / 1000u;
  snprintf(out, cap, "%u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

void dc_draw_spoiler_tile(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, ccord_id key)
{
  ccord_draw_rrect(r, rc, 6.0f, COL(bg_tertiary));
  ccord_draw_rect(r, (ccord_rect){rc.x, rc.y, rc.w, 1.0f}, COL(border_subtle));
  ccord_draw_rect(r, (ccord_rect){rc.x, rc.y + rc.h - 1.0f, rc.w, 1.0f}, COL(border_subtle));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
  ccord_vec2 st = ccord_text_measure(r, "SPOILER", 7, &ss, 0.0f);
  float pw = fminf(st.x + 22.0f, maxf(rc.w - 8.0f, 16.0f));
  float ph = fminf(st.y + 10.0f, maxf(rc.h - 6.0f, 14.0f));
  ccord_rect pill = {rc.x + (rc.w - pw) * 0.5f, rc.y + (rc.h - ph) * 0.5f, pw, ph};
  ccord_draw_rrect(r, pill, ph * 0.5f, COL(bg_primary));
  ccord_text_draw_ellipsis(r, "SPOILER", 7, pill.x + (pill.w - st.x) * 0.5f, pill.y + (pill.h - st.y) * 0.5f,
                           maxf(pw - 6.0f, 8.0f), &ss);
  dc_record_spoiler_hit(ui, rc, key);
}

float dc_gallery_walk(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w, bool draw)
{
  if (!p || !p->att || p->count == 0) return 0.0f;
  size_t n = p->count < CCORD_RICH_MAX_ATTACHMENTS ? p->count : CCORD_RICH_MAX_ATTACHMENTS;
  size_t shown = n < 5u ? n : 5u;
  float gw = clampf(w, 80.0f, 520.0f);
  float gap = 4.0f;
  ccord_rect tiles[5] = {0};
  float total_h = 0.0f;

  if (n == 1u) {
    const ccord_attachment *a0 = &p->att[0];
    const char *u0 = dc_attachment_url(a0);
    uint32_t iw = a0->width, ih = a0->height;
    if ((iw == 0 || ih == 0) && r && u0[0]) {
      int tw_px = 0, th_px = 0;
      if (ccord_render_has_texture(r, u0, &tw_px, &th_px) && tw_px > 4 && th_px > 4) {
        iw = (uint32_t)tw_px;
        ih = (uint32_t)th_px;
      }
    }
    float tw = fminf(gw, 360.0f), th = 112.0f;
    if (iw > 0 && ih > 0) {
      float max_w = fminf(gw, 440.0f);
      float max_h = 320.0f;
      float sc = fminf(1.0f, fminf(max_w / (float)iw, max_h / (float)ih));
      tw = clampf((float)iw * sc, 96.0f, max_w);
      th = clampf((float)ih * sc, 72.0f, max_h);
    }
    tiles[0] = (ccord_rect){x, y, tw, th};
    total_h = th;
  } else if (n == 2u) {
    float cell_w = (gw - gap) * 0.5f;
    float th = 168.0f;
    tiles[0] = (ccord_rect){x, y, cell_w, th};
    tiles[1] = (ccord_rect){x + cell_w + gap, y, cell_w, th};
    total_h = th;
  } else if (n == 3u) {
    float left_w = floorf((gw - gap) * 0.60f);
    float right_w = gw - gap - left_w;
    float th = 220.0f;
    float half_h = floorf((th - gap) * 0.5f);
    tiles[0] = (ccord_rect){x, y, left_w, th};
    tiles[1] = (ccord_rect){x + left_w + gap, y, right_w, half_h};
    tiles[2] = (ccord_rect){x + left_w + gap, y + half_h + gap, right_w, th - half_h - gap};
    total_h = th;
  } else if (n == 4u) {
    float cell_w = (gw - gap) * 0.5f;
    float th = 236.0f;
    float half_h = (th - gap) * 0.5f;
    tiles[0] = (ccord_rect){x, y, cell_w, half_h};
    tiles[1] = (ccord_rect){x + cell_w + gap, y, cell_w, half_h};
    tiles[2] = (ccord_rect){x, y + half_h + gap, cell_w, half_h};
    tiles[3] = (ccord_rect){x + cell_w + gap, y + half_h + gap, cell_w, half_h};
    total_h = th;
  } else {
    float top_w = (gw - gap) * 0.5f;
    float bot_w = (gw - 2.0f * gap) / 3.0f;
    float top_h = 124.0f, bot_h = 96.0f;
    tiles[0] = (ccord_rect){x, y, top_w, top_h};
    tiles[1] = (ccord_rect){x + top_w + gap, y, top_w, top_h};
    for (size_t k = 0; k < 3u; k++) {
      tiles[2u + k] = (ccord_rect){x + (float)k * (bot_w + gap), y + top_h + gap, bot_w, bot_h};
    }
    total_h = top_h + gap + bot_h;
  }

  if (!draw) return total_h;

  const char *rev_urls[CCORD_RICH_MAX_ATTACHMENTS];
  const char *rev_previews[CCORD_RICH_MAX_ATTACHMENTS];
  const char *rev_hds[CCORD_RICH_MAX_ATTACHMENTS];
  const char *rev_names[CCORD_RICH_MAX_ATTACHMENTS];
  const char *rev_descs[CCORD_RICH_MAX_ATTACHMENTS];
  uint32_t rev_w[CCORD_RICH_MAX_ATTACHMENTS];
  uint32_t rev_h[CCORD_RICH_MAX_ATTACHMENTS];
  uint64_t rev_sizes[CCORD_RICH_MAX_ATTACHMENTS];
  size_t rev_idx[CCORD_RICH_MAX_ATTACHMENTS];
  size_t rev_count = 0;
  for (size_t k = 0; k < n; k++) {
    ccord_id sk = dc_spoiler_att_key(p->msg_id, &p->att[k], k);
    bool hidden = p->att[k].spoiler && !dc_is_spoiler_revealed(ui, sk);
    const char *u = dc_attachment_url(&p->att[k]);
    if (!hidden && u[0]) {
      char pbuf[UI_URL_MAX];
      const char *thumb = att_thumb_url(&p->att[k], pbuf, sizeof pbuf);
      char hbuf[UI_URL_MAX];
      const char *hd = u;
      // On Android / mobile, cap in-viewer HD fetch to 2048px proxy when original exceeds 2048px (saves cellular
      // bandwidth & stays inside Android's 48 MiB VRAM budget), while Save/Share/Browser still use full `u`.
      if ((CCORD_OS_ANDROID || ui->mobile_layout) && p->att[k].proxy_url && p->att[k].proxy_url[0] &&
          (p->att[k].width > 2048u || p->att[k].height > 2048u) &&
          ui_media_sized_url(p->att[k].proxy_url, p->att[k].width, p->att[k].height, 2048u, 2048u,
                             att_keeps_format(&p->att[k]), hbuf, sizeof hbuf)) {
        hd = hbuf;
      }
      rev_idx[k] = rev_count;
      rev_urls[rev_count] = u;
      rev_previews[rev_count] = ui_hit_cstr(ui, thumb);
      rev_hds[rev_count] = ui_hit_cstr(ui, hd);
      rev_names[rev_count] = p->att[k].filename ? p->att[k].filename : "";
      rev_descs[rev_count] = p->att[k].description ? p->att[k].description : "";
      rev_w[rev_count] = p->att[k].width;
      rev_h[rev_count] = p->att[k].height;
      rev_sizes[rev_count] = p->att[k].size;
      rev_count++;
    } else {
      rev_idx[k] = SIZE_MAX;
    }
  }

  for (size_t k = 0; k < shown; k++) {
    ccord_id sk = dc_spoiler_att_key(p->msg_id, &p->att[k], k);
    if (p->att[k].spoiler && !dc_is_spoiler_revealed(ui, sk)) {
      dc_draw_spoiler_tile(ui, r, tiles[k], sk);
      continue;
    }
    ccord_draw_rrect(r, tiles[k], 6.0f, COL(bg_secondary));
    const char *u = dc_attachment_url(&p->att[k]);
    char tbuf[UI_URL_MAX];
    const char *tex = u[0] ? dc_media_pick(ui, r, att_thumb_url(&p->att[k], tbuf, sizeof tbuf), u) : nullptr;
    if (tex) {
      // Fill the tile and crop the overflow: a tile is rarely the picture's own shape.
      ccord_draw_texture_cover(r, tex, tiles[k], 6.0f, 255, ui_media_animate(ui, tiles[k]));
      ui->dbg_attachment_textures_drawn++;
    } else {
      ui_icon_attachment(r, tiles[k].x + tiles[k].w * 0.5f, tiles[k].y + tiles[k].h * 0.5f, 20.0f, COL(text_secondary));
    }
    if (k == 4u && n > 5u) {
      ccord_draw_rrect(r, tiles[k], 6.0f, ui_alpha(COL(overlay), 0.72f));
      char more[16];
      snprintf(more, sizeof more, "+%zu", n - 5u);
      ccord_text_style ms = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 2.0f, COL(text_header));
      ccord_vec2 mt = ccord_text_measure(r, more, strlen(more), &ms, 0.0f);
      ccord_text_draw(r, more, strlen(more), tiles[k].x + (tiles[k].w - mt.x) * 0.5f,
                      tiles[k].y + (tiles[k].h - mt.y) * 0.5f, &ms, 0.0f);
    }
    if (rev_idx[k] < rev_count) {
      if (tex && tex[0]) rev_previews[rev_idx[k]] = ui_hit_cstr(ui, tex);
      dc_img_hit_extra extra = {
        .preview_urls = rev_previews,
        .hd_urls = rev_hds,
        .filenames = rev_names,
        .descriptions = rev_descs,
        .orig_w = rev_w,
        .orig_h = rev_h,
        .file_sizes = rev_sizes,
        .msg_id = p->msg_id,
        .author_id = p->author_id,
      };
      dc_record_img_hit_ex(ui, tiles[k], rev_urls, rev_count, rev_idx[k], &extra);
    }
  }
  return total_h;
}

static void draw_sticker_card(ccord_ui *ui, ccord_renderer *r, const ccord_msg_sticker *st, float x, float y)
{
  const char *name = st->name ? st->name : "";
  ccord_id id = st->id;
  uint8_t format_type = st->format ? st->format : 1u;
  if (id == CCORD_ID_NONE && ui->d.store && name[0]) {
    const ccord_sticker *known = ccord_store_find_sticker_by_name(ui->d.store, name);
    if (known) {
      id = known->id;
      format_type = known->format ? (uint8_t)known->format : 1u;
    }
  }
  ccord_rect box = {x, y + 2.0f, 160.0f, 160.0f};
  ui_expr_draw_sticker_art(ui, r, id, name, strlen(name), format_type, box, true);
}

static void draw_forward_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  float h = 46.0f;
  ccord_draw_rrect(r, (ccord_rect){x, y, w, h}, 4.0f, COL(bg_secondary));
  ccord_draw_rect(r, (ccord_rect){x, y + 4.0f, 2.0f, h - 8.0f}, COL(text_muted));
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_normal));
  ccord_text_draw_ellipsis(r, "Forwarded", 9, x + 12.0f, y + 5.0f, w - 20.0f, &ls);
  if (p->text && p->len > 0) {
    ccord_text_draw_ellipsis(r, p->text, p->len, x + 12.0f, y + 22.0f, w - 20.0f, &ts);
  }
}

static void draw_poll_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  const ccord_poll *poll = p->poll;
  if (!poll) return;
  bool expired = (poll->expiry_ms > 0 && ui->now_unix_ms > 0 && ui->now_unix_ms >= poll->expiry_ms);
  bool closed = poll->finalized || expired;
  ccord_id ch_id = ccord_store_selected_channel(ui->d.store);
  float h = 52.0f + (float)poll->answer_count * DECOR_POLL_ROW_H;
  ccord_draw_rrect(r, (ccord_rect){x, y, w, h}, 6.0f, COL(bg_secondary));
  ccord_text_style qs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_header));
  ccord_text_style as = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_normal));
  ccord_text_style fs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, poll->question, strlen(poll->question), x + 12.0f, y + 8.0f, w - 24.0f, &qs);
  uint32_t total = 0;
  for (size_t i = 0; i < poll->answer_count; i++) total += poll->answers[i].votes;
  float ay = y + 32.0f;
  for (size_t i = 0; i < poll->answer_count; i++) {
    const ccord_poll_answer *a = &poll->answers[i];
    ccord_rect row = {x + 10.0f, ay, w - 20.0f, DECOR_POLL_ROW_H - 3.0f};
    bool row_hov = !closed && ui->mouse_in && !ui->menu.open && !ui->settings_open &&
                   !ui->select_open && ui->modal_kind == UI_MODAL_NONE && ui_in_rect(ui->mx, ui->my, row);
    ccord_draw_rrect(r, row, 4.0f, COL(bg_tertiary));
    if (!closed) {
      ui_state_fill(ui, r, row, 4.0f, "pollopt", p->msg_id ^ ((uint64_t)a->id << 32), row_hov, a->me_voted);
    }
    if (total > 0 && a->votes > 0) {
      ccord_color fill = a->me_voted ? COL(brand) : COL(text_muted);
      fill.a = 70;
      ccord_draw_rrect(r, (ccord_rect){row.x, row.y, row.w * ((float)a->votes / (float)total), row.h}, 4.0f, fill);
    }
    ccord_rect ind = {row.x + 7.0f, row.y + (row.h - 12.0f) * 0.5f, 12.0f, 12.0f};
    float rad = poll->multiselect ? 3.0f : 6.0f;
    if (a->me_voted) {
      ccord_draw_rrect(r, ind, rad, COL(brand));
      ccord_draw_rrect(r, (ccord_rect){ind.x + 3.5f, ind.y + 3.5f, 5.0f, 5.0f}, poll->multiselect ? 1.5f : 2.5f,
                       COL(text_on_brand));
    } else {
      ccord_draw_rrect(r, ind, rad, COL(text_muted));
      ccord_draw_rrect(r, (ccord_rect){ind.x + 1.5f, ind.y + 1.5f, 9.0f, 9.0f}, maxf(rad - 1.0f, 1.0f),
                       COL(bg_secondary));
    }
    char line[320];
    snprintf(line, sizeof line, "%s%s%s", a->emoji ? a->emoji : "", (a->emoji && a->emoji[0]) ? " " : "", a->text ? a->text : "");
    char votes[32];
    snprintf(votes, sizeof votes, "%u", (unsigned)a->votes);
    ccord_vec2 vw = ccord_text_measure(r, votes, strlen(votes), &as, 0.0f);
    ccord_text_draw_ellipsis(r, line, strlen(line), row.x + 25.0f, row.y + 3.0f, maxf(row.w - vw.x - 41.0f, 10.0f), &as);
    ccord_text_draw(r, votes, strlen(votes), row.x + row.w - vw.x - 8.0f, row.y + 3.0f, &as, 0.0f);
    if (ui->npoll_hits < UI_POLL_HIT_MAX) {
      ui->poll_hits[ui->npoll_hits++] = (ui_poll_hit){
        .rect = row, .channel_id = ch_id, .message_id = p->msg_id, .answer_id = a->id, .closed = closed,
      };
    }
    ay += DECOR_POLL_ROW_H;
  }
  char foot[96];
  const char *status_tag = poll->finalized ? "  -  Poll closed"
                           : (expired ? "  -  Poll expired" : (poll->multiselect ? "  -  Select one or more" : ""));
  snprintf(foot, sizeof foot, "%u vote%s%s", (unsigned)total, total == 1u ? "" : "s", status_tag);
  ccord_text_draw_ellipsis(r, foot, strlen(foot), x + 12.0f, y + h - 16.0f, w - 24.0f, &fs);
}

static void draw_thread_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, ccord_id channel_id,
                             float x, float y, float w)
{
  if (!p || !p->rich || p->rich->thread.id == CCORD_ID_NONE) return;
  ccord_id tid = p->rich->thread.id;
  const ccord_channel *tc = ccord_store_channel(ui->d.store, tid);
  const ccord_channel *pch = ccord_store_channel(ui->d.store, channel_id);
  const char *tname = (tc && tc->name && tc->name[0]) ? tc->name
                      : ((p->rich->thread.name && p->rich->thread.name[0]) ? p->rich->thread.name : "Thread");
  uint32_t mc = p->rich->thread.message_count;
  if (tc && tc->thread_message_count > mc) mc = tc->thread_message_count;
  bool archived = tc && tc->thread_archived;

  float cw = w < 420.0f ? w : 420.0f;
  ccord_rect card = {x, y, cw, 42.0f};
  bool hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, card);
  ccord_draw_rrect(r, card, 6.0f, hover ? COL(bg_item_hover) : COL(bg_secondary));
  ccord_draw_rrect(r, (ccord_rect){card.x, card.y, 3.0f, card.h}, 2.0f, COL(brand));

  ui_icon_hash(r, card.x + 16.0f, card.y + 14.0f, 14.0f, COL(brand));
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
  ccord_text_draw_ellipsis(r, tname, strlen(tname), card.x + 28.0f, card.y + 6.0f, card.w - 40.0f, &ns);

  char sub[96];
  if (archived) {
    snprintf(sub, sizeof sub, "%u Message%s • Archived  >", (unsigned)mc, mc == 1u ? "" : "s");
  } else {
    snprintf(sub, sizeof sub, "%u Message%s  >", (unsigned)mc, mc == 1u ? "" : "s");
  }
  ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(brand));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), card.x + 28.0f, card.y + 23.0f, card.w - 40.0f, &ss);

  if (ui->nthread_hits < UI_THREAD_HIT_MAX) {
    ui_thread_hit *th = &ui->thread_hits[ui->nthread_hits++];
    th->rect = card;
    th->guild_id = pch ? pch->guild_id : ccord_store_selected_guild(ui->d.store);
    th->parent_id = channel_id;
    th->thread_id = tid;
    snprintf(th->name, sizeof th->name, "%.99s", tname);
  }
}

static void draw_invite_card(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float x, float y, float w)
{
  if (!p || p->invite_code[0] == '\0') return;
  bool loading = false, failed = false;
  const ccord_invite_preview *prev = ui->d.session
    ? ccord_session_invite_lookup(ui->d.session, p->invite_code, &loading, &failed)
    : nullptr;
  if (!prev && !loading && !failed && ui->d.session) {
    ccord_error err;
    if (ccord_session_invite_fetch(ui->d.session, p->invite_code, &err) == CCORD_OK) loading = true;
  }
  bool joined = prev && prev->guild_id != CCORD_ID_NONE &&
                ui->d.store && ccord_store_guild(ui->d.store, prev->guild_id) != nullptr;
  ccord_id self = ui->d.store ? ccord_store_self_id(ui->d.store) : CCORD_ID_NONE;

  float cw = w < 420.0f ? w : 420.0f;
  ccord_rect card = {x, y, cw, 88.0f};
  uis_plate(ui, r, card, 6.0f, COL(bg_secondary));

  const char *hdr = (self != CCORD_ID_NONE && p->author_id == self)
                      ? "YOU SENT AN INVITE TO JOIN A SERVER"
                      : "YOU'VE BEEN INVITED TO JOIN A SERVER";
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, hdr, strlen(hdr), card.x + 12.0f, card.y + 8.0f, card.w - 24.0f, &hs);

  ccord_rect ic = {card.x + 12.0f, card.y + 28.0f, 46.0f, 46.0f};
  if (prev && prev->guild_icon_url[0] && ui_ensure_media(ui, r, prev->guild_icon_url)) {
    ccord_draw_texture_rounded(r, prev->guild_icon_url, ic, 12.0f, 255);
  } else {
    uis_plate(ui, r, ic, 12.0f, failed ? COL(bg_tertiary) : COL(brand));
    char init[8] = "!";
    if (prev && prev->guild_name[0]) ui_initials(prev->guild_name, init);
    else if (!failed) snprintf(init, sizeof init, "?");
    ccord_text_style ist = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), COL(text_on_brand));
    ccord_vec2 it = ccord_text_measure(r, init, strlen(init), &ist, 0.0f);
    ccord_text_draw(r, init, strlen(init), ic.x + (ic.w - it.x) * 0.5f, ic.y + (ic.h - it.y) * 0.5f, &ist, 0.0f);
  }

  float btn_w = joined ? 78.0f : 72.0f;
  float btn_h = 34.0f;
  ccord_rect btn = {card.x + card.w - btn_w - 12.0f, card.y + 34.0f, btn_w, btn_h};
  float tx = ic.x + ic.w + 12.0f;
  float tw = btn.x - tx - 10.0f;
  if (tw < 60.0f) tw = 60.0f;

  const char *title = prev ? prev->guild_name : (failed ? "Invalid Invite" : "Loading invite...");
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base),
                                 failed ? COL(danger) : COL(text_header));
  ccord_text_draw_ellipsis(r, title, strlen(title), tx, card.y + 31.0f, tw, &ts);

  char sub[96];
  if (prev) {
    snprintf(sub, sizeof sub, "%u Online  •  %u Members", (unsigned)prev->online_count, (unsigned)prev->member_count);
  } else if (failed) {
    snprintf(sub, sizeof sub, "This invite may be expired or invalid");
  } else {
    snprintf(sub, sizeof sub, "Fetching server invite...");
  }
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), tx, card.y + 52.0f, tw, &ss);

  if (!failed) {
    bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui->modal_kind == UI_MODAL_NONE &&
               ui_in_rect(ui->mx, ui->my, btn);
    const char *blbl = joined ? "Joined" : "Join";
    uis_button(ui, r, btn, blbl, joined ? UIS_BTN_SECONDARY : UIS_BTN_PRIMARY, UIS_BTN_S_SM,
               (uis_state){.hover = hov}, "invjoin", p->msg_id);
    if (ui->ninvite_hits < UI_INVITE_HIT_MAX) {
      size_t idx = ui->ninvite_hits++;
      ui->invite_hits[idx].rect = card;
      ui->invite_hits[idx].btn_rect = btn;
      ui->invite_hits[idx].guild_id = prev ? prev->guild_id : CCORD_ID_NONE;
      snprintf(ui->invite_hits[idx].code, sizeof ui->invite_hits[idx].code, "%s", p->invite_code);
      ui->invite_hits[idx].joined = joined;
    }
  } else {
    btn = (ccord_rect){0};
  }
}

float dc_draw_decor_cards(ccord_ui *ui, ccord_renderer *r, const decor_info *d, float x, float y, float w)
{
  float y0 = y;
  for (size_t i = 0; i < d->nparts; i++) {
    const decor_part *p = &d->parts[i];
    float ph = 0.0f;
    switch (p->kind) {
      case DECOR_REPLY:   // drawn by draw_row as a line above the header
        continue;
      case DECOR_ATTACHMENT:
        dc_draw_attachment_card(ui, r, p->att, x, y, w);
        ui->dbg_attachment_cards++;
        ph = 46.0f;
        break;
      case DECOR_GALLERY:
        ph = dc_gallery_walk(ui, r, p, x, y, w, true);
        ui->dbg_gallery_cards++;
        ui->dbg_attachment_cards += p->count;
        break;
      case DECOR_MEDIA_PLAYER:
        ph = dc_draw_media_player_card(ui, r, p, x, y, w);
        ui->dbg_media_player_cards++;
        ui->dbg_attachment_cards++;
        break;
      case DECOR_EMBED:
        ph = dc_embed_card_walk(ui, r, p, x, y, w, true);
        ui->dbg_embed_cards += p->count;
        break;
      case DECOR_STICKER:
        draw_sticker_card(ui, r, p->stk, x, y);
        ui->dbg_sticker_cards++;
        ph = 164.0f;
        break;
      case DECOR_FORWARD:
        draw_forward_card(ui, r, p, x, y, w);
        ui->dbg_forward_cards++;
        ph = 46.0f;
        break;
      case DECOR_POLL:
        draw_poll_card(ui, r, p, x, y, w);
        ui->dbg_poll_cards++;
        ph = 52.0f + (p->poll ? (float)p->poll->answer_count : 0.0f) * DECOR_POLL_ROW_H;
        break;
      case DECOR_COMPONENTS:
        ph = dc_components_row_walk(ui, r, p, x, y, w, true);
        break;
      case DECOR_COMPONENTS_V2:
        ph = dc_cv2_walk(ui, r, p, x, y, w, true);
        break;
      case DECOR_THREAD:
        draw_thread_card(ui, r, p, d->channel_id, x, y, w);
        ui->dbg_thread_cards++;
        ph = 42.0f;
        break;
      case DECOR_INVITE:
        draw_invite_card(ui, r, p, x, y, w);
        ui->dbg_invite_cards++;
        ph = 88.0f;
        break;
    }
    y += ph + 4.0f;
  }
  return y - y0;
}

