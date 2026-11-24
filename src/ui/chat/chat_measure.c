// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

struct ui_chat_caches *dc_chat_caches_ensure(ccord_ui *ui)
{
  if (!ui) return nullptr;
  if (!ui->chat_caches) ui->chat_caches = ccord_calloc(1, sizeof *ui->chat_caches);
  return ui->chat_caches;
}

static void free_chat_caches_inner(struct ui_chat_caches *cc)
{
  if (!cc) return;
  ccord_free(cc->tl_rows);
  cc->tl_rows = nullptr;
  cc->tl_count = cc->tl_cap = 0;
  for (size_t i = 0; i < LOCAL_PARSE_CACHE_SLOTS; i++) {
    if (cc->lparse[i].inited) {
      ccord_arena_free(&cc->lparse[i].arena);
      cc->lparse[i].inited = false;
    }
  }
}

void ui_chat_caches_clear(ccord_ui *ui)
{
  if (!ui || !ui->chat_caches) return;
  free_chat_caches_inner(ui->chat_caches);
  memset(ui->chat_caches, 0, sizeof *ui->chat_caches);
}

void ui_chat_caches_free(ccord_ui *ui)
{
  if (!ui) return;
  free_chat_caches_inner(ui->chat_caches);
  ccord_free(ui->chat_caches);
  ui->chat_caches = nullptr;
}

// Discord plays animated chat media only while gif_auto_play is on (and reduced motion is off);
// with gif_auto_play off, hovering one tile still plays it unless reduced motion is enabled.
static bool ui_gif_auto_play(const ccord_ui *ui)
{
  if (ui->anim.reduced_motion || ui->settings_reduced_motion) return false;
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return us ? us->gif_auto_play : ui->settings_gif_auto_play;
}

bool ui_media_animate(const ccord_ui *ui, ccord_rect rect)
{
  if (ui->anim.reduced_motion || ui->settings_reduced_motion) return false;
  return ui_gif_auto_play(ui) || (ui->mouse_in && ui_in_rect(ui->mx, ui->my, rect));
}

// ---------------------------------------------------------------- rich cards (reply / attachment / embed / ...)

// The size of a video's picture box: its own shape, as large as fits max_w x max_h, never narrower than the
// controls need. Unknown dimensions are taken as 16:9.
void dc_video_box_size(uint32_t vw, uint32_t vh, float max_w, float max_h, float *bw, float *bh)
{
  float aw = vw > 0 && vh > 0 ? (float)vw : 16.0f, ah = vw > 0 && vh > 0 ? (float)vh : 9.0f;
  float sc = fminf(max_w / aw, max_h / ah);
  float w = floorf(aw * sc), h = floorf(ah * sc);
  float min_w = fminf(200.0f, max_w);
  *bw = w < min_w ? min_w : w;
  *bh = h < 90.0f ? 90.0f : h;
}


float dc_media_player_w(const ccord_attachment *a, float w)
{
  float cw = clampf(w, 140.0f, 460.0f);
  if (!a || !ccord_attachment_is_video(a)) return cw;
  float bw, bh;
  dc_video_box_size(a->width, a->height, cw - 12.0f, MEDIA_VIDEO_MAX_H, &bw, &bh);
  return bw + 12.0f;
}

float dc_media_player_h(const ccord_attachment *a, float w)
{
  if (!a || !ccord_attachment_is_video(a)) return 58.0f;
  float bw, bh;
  dc_video_box_size(a->width, a->height, clampf(w, 140.0f, 460.0f) - 12.0f, MEDIA_VIDEO_MAX_H, &bw, &bh);
  return 6.0f + bh + 2.0f + MEDIA_CONTROLS_H;
}

static float decor_part_h(ccord_ui *ui, ccord_renderer *r, const decor_part *p, float w)
{
  switch (p->kind) {
    case DECOR_REPLY: return 0.0f;   // drawn as a line above the header, see dc_decor_reply_h
    case DECOR_ATTACHMENT: return 46.0f;
    case DECOR_GALLERY: return dc_gallery_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_MEDIA_PLAYER: return dc_media_player_h(p->att, w);
    case DECOR_EMBED: return dc_embed_card_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_STICKER: return 164.0f;
    case DECOR_FORWARD: return 46.0f;
    case DECOR_POLL: return 52.0f + (p->poll ? (float)p->poll->answer_count : 0.0f) * DECOR_POLL_ROW_H;
    case DECOR_COMPONENTS: return dc_components_row_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_COMPONENTS_V2: return dc_cv2_walk(ui, r, p, 0.0f, 0.0f, w, false);
    case DECOR_THREAD: return 42.0f;
    case DECOR_INVITE: return 88.0f;
  }
  return 0.0f;
}

static void decor_add(decor_info *d, decor_kind kind, const ccord_attachment *att, const ccord_embed *emb,
                      const ccord_msg_sticker *stk, const ccord_poll *poll, const char *text, size_t len, size_t count)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){kind, att, emb, stk, poll, nullptr, text, len, count, 0u, d->msg_id, d->author_id, "", d->edit_rev};
}

static void decor_add_comp(decor_info *d, const ccord_message_rich *rich, size_t row_idx)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){DECOR_COMPONENTS, nullptr, nullptr, nullptr, nullptr, rich,
                                       nullptr, 0u, 1u, row_idx, d->msg_id, d->author_id, "", d->edit_rev};
}

// Components v2: the whole tree is one part (the message's attachments stay reachable for attachment:// media).
static void decor_add_v2(decor_info *d, const ccord_message_rich *rich)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){DECOR_COMPONENTS_V2, nullptr, nullptr, nullptr, nullptr, rich,
                                       nullptr, 0u, 1u, 0u, d->msg_id, d->author_id, "", d->edit_rev};
}

static void decor_add_thread(decor_info *d, const ccord_message_rich *rich)
{
  if (d->nparts >= DECOR_MAX_PARTS) return;
  d->parts[d->nparts++] = (decor_part){DECOR_THREAD, nullptr, nullptr, nullptr, nullptr, rich,
                                       nullptr, 0u, 1u, 0u, d->msg_id, d->author_id, "", d->edit_rev};
}

static bool inv_ieq_at_n(const char *s, size_t rem, const char *lit, size_t n)
{
  if (rem < n) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char a = (unsigned char)s[i];
    unsigned char b = (unsigned char)lit[i];
    if (a >= 'A' && a <= 'Z') a += (unsigned char)('a' - 'A');
    if (a != b) return false;   // table literals are already lowercase
  }
  return true;
}

static void decor_scan_invites(decor_info *d, const char *text, size_t len)
{
  if (!d || !text || len == 0) return;
  typedef struct inv_prefix {
    const char *str;
    size_t len;
  } inv_prefix;
#define INV_PFX(s) {s, sizeof(s) - 1u}
  static const inv_prefix k_prefixes[] = {
    INV_PFX("https://www.discord.gg/"),         INV_PFX("http://www.discord.gg/"),         INV_PFX("www.discord.gg/"),
    INV_PFX("https://discord.gg/"),             INV_PFX("http://discord.gg/"),             INV_PFX("discord.gg/"),
    INV_PFX("https://www.discord.com/invite/"), INV_PFX("http://www.discord.com/invite/"), INV_PFX("www.discord.com/invite/"),
    INV_PFX("https://discord.com/invite/"),     INV_PFX("http://discord.com/invite/"),     INV_PFX("discord.com/invite/"),
    INV_PFX("https://ptb.discord.com/invite/"), INV_PFX("http://ptb.discord.com/invite/"), INV_PFX("ptb.discord.com/invite/"),
    INV_PFX("https://canary.discord.com/invite/"), INV_PFX("http://canary.discord.com/invite/"), INV_PFX("canary.discord.com/invite/"),
    INV_PFX("https://discordapp.com/invite/"),  INV_PFX("http://discordapp.com/invite/"),  INV_PFX("discordapp.com/invite/"),
    INV_PFX("discord://-/invite/"),             INV_PFX("discord://invite/"),
  };
#undef INV_PFX
  size_t found = 0;
  for (size_t i = 0; i < len && found < 3u && d->nparts < DECOR_MAX_PARTS; i++) {
    unsigned char c0 = (unsigned char)text[i];
    if (c0 >= 'A' && c0 <= 'Z') c0 += (unsigned char)('a' - 'A');
    if (c0 != 'h' && c0 != 'w' && c0 != 'd' && c0 != 'p' && c0 != 'c') continue;
    if (i > 0) {
      unsigned char prev = (unsigned char)text[i - 1u];
      bool word_prev = (prev >= '0' && prev <= '9') || (prev >= 'a' && prev <= 'z') ||
                       (prev >= 'A' && prev <= 'Z') || prev == '.' || prev == '_' || prev == '/';
      if (word_prev) continue;
    }
    size_t plen = 0;
    for (size_t pi = 0; pi < sizeof k_prefixes / sizeof k_prefixes[0]; pi++) {
      if ((unsigned char)k_prefixes[pi].str[0] != c0) continue;
      if (inv_ieq_at_n(text + i, len - i, k_prefixes[pi].str, k_prefixes[pi].len)) {
        plen = k_prefixes[pi].len;
        break;
      }
    }
    if (plen == 0) continue;
    size_t cs = i + plen;
    size_t ce = cs;
    while (ce < len && ce - cs <= CCORD_ADMIN_INVITE_CODE_MAX) {
      char c = text[ce];
      bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
      if (!ok) break;
      ce++;
    }
    size_t clen = ce - cs;
    if (clen >= 2 && clen <= CCORD_ADMIN_INVITE_CODE_MAX) {
      char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
      memcpy(code, text + cs, clen);
      code[clen] = '\0';
      bool dup = false;
      for (size_t k = 0; k < d->nparts; k++) {
        if (d->parts[k].kind == DECOR_INVITE && strcmp(d->parts[k].invite_code, code) == 0) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        decor_part part = {.kind = DECOR_INVITE, .count = 1u, .msg_id = d->msg_id, .author_id = d->author_id};
        memcpy(part.invite_code, code, clen + 1u);
        d->parts[d->nparts++] = part;
        found++;
      }
    }
    i = ce;
  }
}

static bool embed_is_extra_gallery_image(const ccord_embed *head, const ccord_embed *e)
{
  if (!head || !e || !head->url || !head->url[0] || !e->url || strcmp(head->url, e->url) != 0) return false;
  if (!e->image.url || !e->image.url[0]) return false;
  if ((e->title && e->title[0]) || (e->description && e->description[0]) || e->field_count > 0) return false;
  if ((e->footer_text && e->footer_text[0]) || e->timestamp_ms > 0) return false;
  return true;
}

bool dc_url_host_in_list(const char *url, const char *const *hosts, size_t nhosts)
{
  if (!url || !url[0]) return false;
  const char *p = nullptr;
  if (strncasecmp(url, "https://", 8) == 0) p = url + 8;
  else if (strncasecmp(url, "http://", 7) == 0) p = url + 7;
  else return false;
  const char *end = p;
  while (*end && *end != '/' && *end != '?' && *end != '#') end++;
  const char *colon = memchr(p, ':', (size_t)(end - p));
  if (colon) end = colon;
  size_t hlen = (size_t)(end - p);
  if (hlen == 0) return false;
  for (size_t i = 0; i < nhosts; i++) {
    if (strlen(hosts[i]) == hlen && strncasecmp(p, hosts[i], hlen) == 0) return true;
  }
  return false;
}

// Formats that play inline as looping animated images (and must not be flattened to a WebP still by the proxy).
bool dc_url_is_anim_image(const char *u)
{
  if (!u || !u[0]) return false;
  return ui_url_path_ends(u, ".gif") || ui_url_path_ends(u, ".webp") || ui_url_path_ends(u, ".apng") ||
         ui_url_path_ends(u, ".gifv");
}

static bool embed_is_gif_provider(const ccord_embed *e)
{
  if (!e) return false;
  if (e->provider && e->provider[0]) {
    static const char *const k_provs[] = {"Tenor", "GIPHY", "Klipy", "Gfycat", "RedGIFs"};
    for (size_t i = 0; i < sizeof k_provs / sizeof k_provs[0]; i++) {
      if (strcasecmp(e->provider, k_provs[i]) == 0) return true;
    }
  }
  static const char *const k_gif_hosts[] = {
    "tenor.com", "www.tenor.com", "c.tenor.com", "media.tenor.com", "media1.tenor.com", "media2.tenor.com",
    "media.tenor.co", "giphy.com", "www.giphy.com", "media.giphy.com", "media0.giphy.com", "media1.giphy.com",
    "media2.giphy.com", "media3.giphy.com", "media4.giphy.com", "i.giphy.com", "gph.is",
    "klipy.com", "www.klipy.com", "static.klipy.com", "media.klipy.com",
  };
  return dc_url_host_in_list(e->url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]) ||
         dc_url_host_in_list(e->video.url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]) ||
         dc_url_host_in_list(e->image.url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]) ||
         dc_url_host_in_list(e->thumbnail.url, k_gif_hosts, sizeof k_gif_hosts / sizeof k_gif_hosts[0]);
}

// Rich text chrome other than `provider`: Tenor/GIPHY/Klipy/Imgur GIFV and image embeds routinely carry a provider
// name ("Tenor", "GIPHY", "Klipy") while still rendering framelessly in Discord.
static bool embed_has_rich_chrome(const ccord_embed *e)
{
  if (!e) return false;
  return (e->title && e->title[0]) || (e->description && e->description[0]) || (e->author_name && e->author_name[0]) ||
         (e->footer_text && e->footer_text[0]) || e->timestamp_ms > 0 || e->field_count > 0;
}

static bool embed_has_visual_media(const ccord_embed *e)
{
  if (!e) return false;
  return (e->video.proxy_url && e->video.proxy_url[0]) || (e->video.url && e->video.url[0]) ||
         (e->image.proxy_url && e->image.proxy_url[0]) || (e->image.url && e->image.url[0]) ||
         (e->thumbnail.proxy_url && e->thumbnail.proxy_url[0]) || (e->thumbnail.url && e->thumbnail.url[0]);
}

// True for GIFs and looping GIF-like embeds (Tenor/GIPHY/Klipy/Imgur gifv, direct .gif/.webp/.apng embeds, or
// GIF-provider video embeds) that should play framelessly without video-player chrome.
bool dc_embed_is_gif_like(const ccord_embed *e)
{
  if (!e || !embed_has_visual_media(e)) return false;
  if (e->kind == CCORD_EMBED_GIFV) return true;
  if (embed_has_rich_chrome(e)) return false;
  if (e->kind == CCORD_EMBED_IMAGE) {
    return dc_url_is_anim_image(e->url) || dc_url_is_anim_image(e->image.url) || dc_url_is_anim_image(e->image.proxy_url) ||
           dc_url_is_anim_image(e->thumbnail.url) || dc_url_is_anim_image(e->thumbnail.proxy_url) || embed_is_gif_provider(e);
  }
  if (e->kind == CCORD_EMBED_VIDEO) {
    return embed_is_gif_provider(e) || dc_url_is_anim_image(e->url) || dc_url_is_anim_image(e->video.url) ||
           dc_url_is_anim_image(e->video.proxy_url) || dc_url_is_anim_image(e->thumbnail.url) || dc_url_is_anim_image(e->image.url);
  }
  return false;
}

// Pure frameless media embed (GIF/GIFV/animated image or bare image with no rich text frame).
bool dc_embed_is_bare_media(const ccord_embed *e)
{
  if (!e || !embed_has_visual_media(e)) return false;
  if (dc_embed_is_gif_like(e)) return true;
  return e->kind == CCORD_EMBED_IMAGE && !embed_has_rich_chrome(e);
}

static bool url_slice_matches_loose(const char *s, size_t slen, const char *cand)
{
  if (!s || slen == 0 || !cand || !cand[0]) return false;
  const char *sp = s;
  size_t sn = slen;
  if (sn >= 8 && strncasecmp(sp, "https://", 8) == 0) { sp += 8; sn -= 8; }
  else if (sn >= 7 && strncasecmp(sp, "http://", 7) == 0) { sp += 7; sn -= 7; }
  else return false;
  const char *cp = cand;
  size_t cn = strlen(cp);
  if (cn >= 8 && strncasecmp(cp, "https://", 8) == 0) { cp += 8; cn -= 8; }
  else if (cn >= 7 && strncasecmp(cp, "http://", 7) == 0) { cp += 7; cn -= 7; }
  else return false;
  if (sn > 0 && sp[sn - 1] == '/') sn--;
  if (cn > 0 && cp[cn - 1] == '/') cn--;
  if (sn != cn || sn == 0) return false;
  // Scheme-stripped host is case-insensitive; path/query are exact.
  const char *s_slash = memchr(sp, '/', sn);
  const char *c_slash = memchr(cp, '/', cn);
  size_t s_hlen = s_slash ? (size_t)(s_slash - sp) : sn;
  size_t c_hlen = c_slash ? (size_t)(c_slash - cp) : cn;
  if (s_hlen != c_hlen) return false;
  if (strncasecmp(sp, cp, s_hlen) != 0) return false;
  return memcmp(sp + s_hlen, cp + c_hlen, sn - s_hlen) == 0;
}

// Hides the raw link text when a message consists solely of a single URL whose embed renders as a frameless GIF/media
// item (like Discord does for Tenor/GIPHY/Klipy/direct-GIF posts with no accompanying text).
static bool message_should_hide_bare_link(const ccord_ui *ui, const ccord_message *m)
{
  if (!ui || !m || !m->content || !m->content[0] || !m->rich) return false;
  if (!ui->settings_embeds) return false;
  const ccord_message_rich *rich = m->rich;
  if ((rich->flags & (CCORD_MSGFLAG_SUPPRESS_EMBEDS | CCORD_MSGFLAG_IS_COMPONENTS_V2)) != 0) return false;
  if (ccord_message_type_is_system(rich->type) || rich->forward.present) return false;
  if (rich->embed_count != 1u) return false;

  const char *s = m->content;
  size_t slen = strlen(s);
  while (slen > 0 && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) { s++; slen--; }
  while (slen > 0 && (s[slen - 1] == ' ' || s[slen - 1] == '\t' || s[slen - 1] == '\r' || s[slen - 1] == '\n')) slen--;
  if (slen <= 8u) return false;
  if (strncasecmp(s, "https://", 8) != 0 && strncasecmp(s, "http://", 7) != 0) return false;
  for (size_t i = 0; i < slen; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c <= 0x20u || c == 0x7fu) return false;   // extra words / whitespace -> keep message text
  }

  const ccord_embed *e = &rich->embeds[0];
  if (dc_embed_is_gif_like(e)) return true;
  if (dc_embed_is_bare_media(e)) {
    return url_slice_matches_loose(s, slen, e->url) || url_slice_matches_loose(s, slen, e->image.url) ||
           url_slice_matches_loose(s, slen, e->thumbnail.url);
  }
  return false;
}

bool dc_decor_has_body_cards(const decor_info *d)
{
  if (!d) return false;
  for (size_t i = 0; i < d->nparts; i++) {
    if (d->parts[i].kind != DECOR_REPLY) return true;
  }
  return false;
}

static void decor_add_media(const ccord_ui *ui, decor_info *d, const ccord_attachment *atts, size_t natt,
                            const ccord_embed *embs, size_t nemb, const ccord_msg_sticker *stks, size_t nstk)
{
  if (ui->settings_attachments) {
    size_t i = 0;
    while (i < natt) {
      if (ccord_attachment_is_image(&atts[i])) {
        size_t k = 1u;
        while (i + k < natt && k < CCORD_RICH_MAX_ATTACHMENTS && ccord_attachment_is_image(&atts[i + k])) k++;
        decor_add(d, DECOR_GALLERY, &atts[i], nullptr, nullptr, nullptr, nullptr, 0, k);
        i += k;
      } else if (ccord_attachment_is_video(&atts[i]) || ccord_attachment_is_audio(&atts[i])) {
        decor_add(d, DECOR_MEDIA_PLAYER, &atts[i], nullptr, nullptr, nullptr, nullptr, 0, 1u);
        i++;
      } else {
        decor_add(d, DECOR_ATTACHMENT, &atts[i], nullptr, nullptr, nullptr, nullptr, 0, 1u);
        i++;
      }
    }
  }
  if (ui->settings_embeds) {
    size_t i = 0;
    while (i < nemb) {
      size_t k = 1u;
      if (embs[i].url && embs[i].url[0] && embs[i].image.url && embs[i].image.url[0]) {
        while (i + k < nemb && k < CCORD_RICH_MAX_ATTACHMENTS && embed_is_extra_gallery_image(&embs[i], &embs[i + k])) {
          k++;
        }
      }
      decor_add(d, DECOR_EMBED, nullptr, &embs[i], nullptr, nullptr, nullptr, 0, k);
      i += k;
    }
  }
  for (size_t i = 0; i < nstk; i++) decor_add(d, DECOR_STICKER, nullptr, nullptr, &stks[i], nullptr, nullptr, 0, 1u);
}

static void body_put(decor_info *d, size_t *out, const char *s, size_t n)
{
  if (*out + n > sizeof d->body - 1) n = sizeof d->body - 1 - *out;
  memcpy(d->body + *out, s, n);
  *out += n;
}

// Sets the reply line from what is stored, or failing that from what Discord attached to the reply.
static bool decor_reply(const ccord_ui *ui, const ccord_message *m, decor_info *d)
{
  const ccord_message_rich *rich = m->rich;
  ccord_id target = m->reply_to_id != CCORD_ID_NONE ? m->reply_to_id
                    : (rich ? rich->reference.message_id : CCORD_ID_NONE);
  d->reply_target_id = target;
  ccord_id guild = ccord_store_selected_guild(ui->d.store);
  if (target != CCORD_ID_NONE) {
    const ccord_message *r = ccord_store_find_message(ui->d.store, m->channel_id, target);
    if (r) {
      const ccord_user *u = ccord_store_user(ui->d.store, r->author_id);
      const ccord_member *gm = (guild != CCORD_ID_NONE && guild != CCORD_DM_GUILD_ID)
                                   ? ccord_store_find_member(ui->d.store, guild, r->author_id) : nullptr;
      snprintf(d->reply_name, sizeof d->reply_name, "%s", (gm && gm->nick[0] != '\0') ? gm->nick : (u ? u->name : "Unknown User"));
      d->reply_author_id = r->author_id;
      d->reply_attach_only = (!r->content || r->content[0] == '\0') && r->rich && r->rich->attachment_count > 0;
      (void)ccord_message_preview(r, d->reply_text, sizeof d->reply_text);
      return true;
    }
  }
  if (!rich) return false;
  if (rich->ref_preview.present) {
    const ccord_user *u = rich->ref_preview.author_id != CCORD_ID_NONE
                              ? ccord_store_user(ui->d.store, rich->ref_preview.author_id) : nullptr;
    const char *name = (rich->ref_preview.author_name && rich->ref_preview.author_name[0]) ? rich->ref_preview.author_name
                       : (u ? u->name : "Unknown User");
    snprintf(d->reply_name, sizeof d->reply_name, "%s", name);
    d->reply_author_id = rich->ref_preview.author_id;
    if (rich->ref_preview.content && rich->ref_preview.content[0]) {
      size_t tl = dc_utf8_boundary(rich->ref_preview.content, strlen(rich->ref_preview.content), sizeof d->reply_text);
      memcpy(d->reply_text, rich->ref_preview.content, tl);
      d->reply_text[tl] = '\0';
    } else {
      d->reply_attach_only = rich->ref_preview.has_attachments;
      snprintf(d->reply_text, sizeof d->reply_text, "%s",
               rich->ref_preview.has_attachments ? "Click to see attachment" : "");
    }
    return true;
  }
  if (rich->ref_preview.deleted) {
    d->reply_deleted = true;
    d->reply_target_id = CCORD_ID_NONE;
    d->reply_name[0] = '\0';
    snprintf(d->reply_text, sizeof d->reply_text, "Original message was deleted");
    return true;
  }
  return false;
}

// What goes above/below a message: its text, a system message's wording, and the cards for everything it carries.
void dc_decor_scan(const ccord_ui *ui, const ccord_message *m, bool compact, decor_info *d)
{
  d->body_len = 0;
  d->nparts = 0;
  d->channel_id = m->channel_id;
  d->msg_id = m->id;
  d->author_id = m->author_id;
  d->edit_rev = m->edit_rev;
  d->reply_target_id = CCORD_ID_NONE;
  d->reply_deleted = false;
  d->reply_attach_only = false;
  d->reply_author_id = CCORD_ID_NONE;
  d->reply_name[0] = d->reply_text[0] = '\0';
  const ccord_message_rich *rich = m->rich;
  if (decor_reply(ui, m, d)) decor_add(d, DECOR_REPLY, nullptr, nullptr, nullptr, nullptr, d->reply_text, strlen(d->reply_text), 1u);

  ui_msg_style_ov style_ov;
  bool has_style_ov = ui_decor_message_style_ex(ui, m->id, &style_ov);
  bool has_content_ov = has_style_ov && style_ov.has_content_override;
  const char *content = has_content_ov ? style_ov.content_override : (m->content ? m->content : "");
  size_t out = 0;
  if (rich && ccord_message_type_is_system(rich->type)) {
    // "<name> joined the server." in place of text. Types that carry text (a new channel name, a thread title) append it.
    const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
    const char *name = u ? u->name : "Someone";
    const char *phrase = ccord_message_type_phrase(rich->type);
    if (phrase || content[0] == '\0') {
      body_put(d, &out, name, strlen(name));
      body_put(d, &out, " ", 1);
      const char *words = phrase ? phrase : "sent a system message.";
      body_put(d, &out, words, strlen(words));
      if (phrase && content[0] != '\0' && ccord_message_type_appends_content(rich->type)) {
        body_put(d, &out, " ", 1);
        body_put(d, &out, content, strlen(content));
      }
    } else {
      body_put(d, &out, content, strlen(content));
    }
  } else if (has_content_ov || !message_should_hide_bare_link(ui, m)) {
    body_put(d, &out, content, strlen(content));
  }
  if (rich) {
    // A Components v2 message carries its whole body in the component tree: no text, embeds, stickers or poll of its
    // own, and its attachments only feed attachment:// media inside the tree (no cards of their own).
    bool v2 = (rich->flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0;
    if (rich->forward.present) {
      decor_add(d, DECOR_FORWARD, nullptr, nullptr, nullptr, nullptr, rich->forward.content,
                rich->forward.content ? strlen(rich->forward.content) : 0u, 1u);
      decor_add_media(ui, d, rich->forward.attachments, rich->forward.attachment_count, rich->forward.embeds,
                      rich->forward.embed_count, rich->forward.stickers, rich->forward.sticker_count);
    }
    if (!v2) {
      size_t nemb = (rich->flags & CCORD_MSGFLAG_SUPPRESS_EMBEDS) ? 0u : rich->embed_count;
      decor_add_media(ui, d, rich->attachments, rich->attachment_count, rich->embeds, nemb, rich->stickers,
                      rich->sticker_count);
      if (rich->poll.present) decor_add(d, DECOR_POLL, nullptr, nullptr, nullptr, &rich->poll, nullptr, 0, 1u);
    }
    if (rich->thread.id != CCORD_ID_NONE) decor_add_thread(d, rich);
    if (v2) {
      if (rich->v2_component_count > 0) decor_add_v2(d, rich);
    } else {
      for (size_t ri = 0; ri < rich->component_row_count; ri++) {
        if (rich->components[ri].component_count > 0) decor_add_comp(d, rich, ri);
      }
    }
  }
  decor_scan_invites(d, content, strlen(content));
  if (compact && dc_message_edited(m)) {
    const char *tag = " (edited)";
    size_t tl = strlen(tag);
    if (out + tl < sizeof d->body) {
      memcpy(d->body + out, tag, tl);
      out += tl;
    }
  }
  if (compact && m->tts) {   // compact rows have no header: the TTS marker rides at the end of the body
    const char *tag = " (tts)";
    size_t tl = strlen(tag);
    if (out + tl < sizeof d->body) {
      memcpy(d->body + out, tag, tl);
      out += tl;
    }
  }
  d->body[out] = '\0';
  d->body_len = out;
}

size_t ui_message_display_body(const ccord_ui *ui, const ccord_message *m, char *out, size_t cap)
{
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!m) return 0;
  decor_info d;
  dc_decor_scan(ui, m, false, &d);
  size_t len = d.body_len;
  if (len >= cap) len = dc_utf8_boundary(d.body, len, cap);
  memcpy(out, d.body, len);
  out[len] = '\0';
  return len;
}

size_t ui_message_clean_body(const ccord_ui *ui, const ccord_message *m, char *out, size_t cap)
{
  (void)ui;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!m || !m->content) return 0;
  size_t len = strlen(m->content);
  if (len >= cap) len = dc_utf8_boundary(m->content, len, cap);
  memcpy(out, m->content, len);
  out[len] = '\0';
  return len;
}

bool dc_message_mentions_self(const ccord_ui *ui, const ccord_message *m)
{
  if (!ui || !ui->d.store || !m) return false;
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  if (self_id == CCORD_ID_NONE) return false;
  if (m->reply_to_id != CCORD_ID_NONE) {
    const ccord_message *rm = ccord_store_find_message(ui->d.store, m->channel_id, m->reply_to_id);
    if (rm && rm->author_id == self_id) return true;
  }
  if (m->rich && (m->rich->mention_everyone || ccord_rich_mentions_user(m->rich, self_id))) return true;   // incl. reply pings
  if (!m->content || !m->content[0]) return false;
  if (strstr(m->content, "@everyone") || strstr(m->content, "@here")) return true;
  {
    const char *p = m->content;
    while ((p = strstr(p, "@you")) != nullptr) {
      bool ok_left = (p == m->content || (!((unsigned char)p[-1] >= 'a' && (unsigned char)p[-1] <= 'z') &&
                                          !((unsigned char)p[-1] >= 'A' && (unsigned char)p[-1] <= 'Z') &&
                                          !((unsigned char)p[-1] >= '0' && (unsigned char)p[-1] <= '9')));
      bool ok_right = (!((unsigned char)p[4] >= 'a' && (unsigned char)p[4] <= 'z') &&
                       !((unsigned char)p[4] >= 'A' && (unsigned char)p[4] <= 'Z') &&
                       !((unsigned char)p[4] >= '0' && (unsigned char)p[4] <= '9'));
      if (ok_left && ok_right) return true;
      p += 4;
    }
  }
  char tag1[32], tag2[32];
  snprintf(tag1, sizeof tag1, "<@%llu>", (unsigned long long)self_id);
  snprintf(tag2, sizeof tag2, "<@!%llu>", (unsigned long long)self_id);
  if (strstr(m->content, tag1) || strstr(m->content, tag2)) return true;
  const ccord_user *su = ccord_store_user(ui->d.store, self_id);
  if (su && su->name && su->name[0]) {
    char at_name[96];
    snprintf(at_name, sizeof at_name, "@%s", su->name);
    if (strstr(m->content, at_name)) return true;
  }
  return false;
}

static float decor_total_h(ccord_ui *ui, ccord_renderer *r, const decor_info *d, float w)
{
  float h = 0.0f;
  for (size_t i = 0; i < d->nparts; i++) {
    if (d->parts[i].kind == DECOR_REPLY) continue;   // the reply line sits above the header, not in the card stack
    h += decor_part_h(ui, r, &d->parts[i], w) + 4.0f;
  }
  return h;
}

// Height the reply line adds above the message header (0 when the message is not a reply).
float dc_decor_reply_h(const decor_info *d)
{
  for (size_t i = 0; i < d->nparts; i++) {
    if (d->parts[i].kind == DECOR_REPLY) return UI_REPLY_LINE_H + UI_REPLY_LINE_GAP;
  }
  return 0.0f;
}

// ---- Merged timeline cache (live store messages + positioned plugin local messages) ----

static ccord_id local_eff_sort_id(const ccord_local_message *lm)
{
  if (!lm) return 0;
  if (lm->sort_id != CCORD_ID_NONE) return lm->sort_id;
  int64_t ms = (int64_t)lm->timestamp_ms - 1420070400000LL;
  if (ms < 0) ms = 0;
  return ((uint64_t)ms) << 22;
}

static int cmp_local_chrono(const void *a, const void *b)
{
  const ccord_local_message *la = *(const ccord_local_message *const *)a;
  const ccord_local_message *lb = *(const ccord_local_message *const *)b;
  ccord_id sa = local_eff_sort_id(la), sb = local_eff_sort_id(lb);
  if (sa != sb) return sa < sb ? -1 : 1;
  if (la->timestamp_ms != lb->timestamp_ms) return la->timestamp_ms < lb->timestamp_ms ? -1 : 1;
  if (la->local_id != lb->local_id) return la->local_id < lb->local_id ? -1 : 1;
  return 0;
}

static timeline_row make_live_tl_row(size_t store_i, const ccord_message *m)
{
  return (timeline_row){
    .is_local = false,
    .store_i = store_i,
    .local_id = 0,
    .sort_id = m ? m->id : CCORD_ID_NONE,
    .timestamp_ms = m ? m->timestamp_ms : 0,
    .author_id = m ? m->author_id : CCORD_ID_NONE,
    .break_group = false,
    .compact_mode = CCORD_LOCAL_COMPACT_AUTO,
  };
}

static timeline_row make_local_tl_row(const ccord_local_message *lm)
{
  return (timeline_row){
    .is_local = true,
    .store_i = SIZE_MAX,
    .local_id = lm->local_id,
    .sort_id = local_eff_sort_id(lm),
    .timestamp_ms = (int64_t)lm->timestamp_ms,
    .author_id = lm->author_id,
    .break_group = lm->break_group,
    .compact_mode = lm->compact_mode,
  };
}

void dc_timeline_sync(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui) return;
  struct ui_chat_caches *cc = dc_chat_caches_ensure(ui);
  if (!cc) return;
  uint64_t srev = ui->d.store ? ccord_store_revision(ui->d.store) : 0;
  uint64_t lrev = ui->d.contrib ? ccord_contrib_local_messages_revision(ui->d.contrib) : 0;
  bool hmore = (channel_id != CCORD_ID_NONE && ui->d.session)
                 ? ccord_session_history_has_more(ui->d.session, channel_id)
                 : false;
  if (cc->tl_valid && cc->tl_chan == channel_id && cc->tl_store_rev == srev &&
      cc->tl_local_rev == lrev && cc->tl_hist_more == hmore) {
    return;
  }
  cc->tl_valid = true;
  cc->tl_chan = channel_id;
  cc->tl_store_rev = srev;
  cc->tl_local_rev = lrev;
  cc->tl_hist_more = hmore;

  size_t store_n = (channel_id != CCORD_ID_NONE && ui->d.store)
                     ? ccord_store_message_count(ui->d.store, channel_id)
                     : 0u;
  size_t n_all_loc = (channel_id != CCORD_ID_NONE && ui->d.contrib)
                       ? ccord_contrib_local_message_count(ui->d.contrib, channel_id)
                       : 0u;
  if (n_all_loc == 0u || ui_plugin_security_surface_active(ui)) {
    cc->tl_merged = false;
    cc->tl_count = store_n;
    return;
  }

  const ccord_local_message *chrono[256];
  const ccord_local_message *anchored[256];
  size_t nchrono = 0, nanc = 0;
  const ccord_message *oldest_live = store_n > 0 ? ccord_store_message_at(ui->d.store, channel_id, 0) : nullptr;
  ccord_id oldest_live_id = oldest_live ? oldest_live->id : CCORD_ID_NONE;

  for (size_t i = 0; i < n_all_loc && (nchrono + nanc) < 256u; i++) {
    const ccord_local_message *lm = ccord_contrib_local_message_at(ui->d.contrib, channel_id, i);
    if (!lm || lm->position != CCORD_LOCAL_POS_TIMELINE) continue;
    bool has_anc = (lm->after_message_id != CCORD_ID_NONE || lm->before_message_id != CCORD_ID_NONE);
    if (lm->clip_to_history && store_n > 0 && hmore) {
      bool anc_loaded = false;
      if (lm->after_message_id != CCORD_ID_NONE &&
          ccord_store_find_message(ui->d.store, channel_id, lm->after_message_id) != nullptr) anc_loaded = true;
      if (lm->before_message_id != CCORD_ID_NONE &&
          ccord_store_find_message(ui->d.store, channel_id, lm->before_message_id) != nullptr) anc_loaded = true;
      if (!anc_loaded && local_eff_sort_id(lm) < oldest_live_id) continue;
    }
    if (has_anc) anchored[nanc++] = lm;
    else chrono[nchrono++] = lm;
  }
  if (nchrono == 0 && nanc == 0) {
    cc->tl_merged = false;
    cc->tl_count = store_n;
    return;
  }

  if (nchrono > 1u) qsort(chrono, nchrono, sizeof chrono[0], cmp_local_chrono);
  if (nanc > 1u) qsort(anchored, nanc, sizeof anchored[0], cmp_local_chrono);

  size_t need = store_n + nchrono + nanc;
  if (need > cc->tl_cap) {
    size_t ncap = need < 64u ? 64u : need;
    timeline_row *nr = ccord_realloc_array(cc->tl_rows, ncap, sizeof *nr);
    if (!nr) {
      cc->tl_merged = false;
      cc->tl_count = store_n;
      return;
    }
    cc->tl_rows = nr;
    cc->tl_cap = ncap;
  }

  // Two-pointer merge of live store messages and chronological locals.
  size_t out_n = 0, si = 0, ci = 0;
  while (si < store_n || ci < nchrono) {
    bool take_live = false;
    if (si < store_n && ci < nchrono) {
      const ccord_message *m = ccord_store_message_at(ui->d.store, channel_id, si);
      ccord_id lsid = local_eff_sort_id(chrono[ci]);
      if (!m) {
        take_live = true;
      } else if (m->send_state != CCORD_MSG_SEND_CONFIRMED) {
        take_live = (m->timestamp_ms <= (int64_t)chrono[ci]->timestamp_ms);
      } else {
        take_live = (m->id <= lsid);
      }
    } else if (si < store_n) {
      take_live = true;
    }
    if (take_live) {
      const ccord_message *m = ccord_store_message_at(ui->d.store, channel_id, si);
      cc->tl_rows[out_n++] = make_live_tl_row(si, m);
      si++;
    } else {
      cc->tl_rows[out_n++] = make_local_tl_row(chrono[ci]);
      ci++;
    }
  }

  // Resolve anchored items (supporting chains up to depth 16).
  bool resolved[256] = {0};
  ccord_id *after_tag = nanc > 0 ? ccord_calloc(need, sizeof *after_tag) : nullptr;
  for (int pass = 0; pass < 16 && nanc > 0; pass++) {
    bool progress = false;
    for (size_t ai = 0; ai < nanc; ai++) {
      if (resolved[ai]) continue;
      const ccord_local_message *lm = anchored[ai];
      bool is_after = (lm->after_message_id != CCORD_ID_NONE);
      ccord_id aid = is_after ? lm->after_message_id : lm->before_message_id;
      size_t pos = SIZE_MAX;
      for (size_t k = 0; k < out_n; k++) {
        if (cc->tl_rows[k].sort_id == aid) { pos = k; break; }
      }
      if (pos == SIZE_MAX) continue;
      size_t ins = is_after ? (pos + 1u) : pos;
      if (is_after && after_tag) {
        while (ins < out_n && cc->tl_rows[ins].is_local && after_tag[ins] == aid) ins++;
      }
      memmove(&cc->tl_rows[ins + 1u], &cc->tl_rows[ins], (out_n - ins) * sizeof cc->tl_rows[0]);
      if (after_tag) {
        memmove(&after_tag[ins + 1u], &after_tag[ins], (out_n - ins) * sizeof after_tag[0]);
        after_tag[ins] = is_after ? aid : CCORD_ID_NONE;
      }
      cc->tl_rows[ins] = make_local_tl_row(lm);
      out_n++;
      resolved[ai] = true;
      progress = true;
    }
    if (!progress) break;
  }
  ccord_free(after_tag);

  // Any anchored item whose anchor wasn't found falls back to chronological placement.
  for (size_t ai = 0; ai < nanc; ai++) {
    if (resolved[ai]) continue;
    const ccord_local_message *lm = anchored[ai];
    ccord_id lsid = local_eff_sort_id(lm);
    size_t ins = out_n;
    for (size_t k = 0; k < out_n; k++) {
      if (cc->tl_rows[k].sort_id > lsid) { ins = k; break; }
    }
    memmove(&cc->tl_rows[ins + 1u], &cc->tl_rows[ins], (out_n - ins) * sizeof cc->tl_rows[0]);
    cc->tl_rows[ins] = make_local_tl_row(lm);
    out_n++;
  }

  cc->tl_merged = true;
  cc->tl_count = out_n;
}

static bool tl_cache_matches(const ccord_ui *ui, ccord_id ch)
{
  const struct ui_chat_caches *cc = ui ? ui->chat_caches : nullptr;
  if (!cc || !cc->tl_valid || !cc->tl_merged || cc->tl_chan != ch) return false;
  uint64_t srev = ui->d.store ? ccord_store_revision(ui->d.store) : 0;
  uint64_t lrev = ui->d.contrib ? ccord_contrib_local_messages_revision(ui->d.contrib) : 0;
  return cc->tl_store_rev == srev && cc->tl_local_rev == lrev;
}

size_t ui_timeline_count(const ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || channel_id == CCORD_ID_NONE) return 0u;
  if (tl_cache_matches(ui, channel_id)) return ui->chat_caches->tl_count;
  return ui->d.store ? ccord_store_message_count(ui->d.store, channel_id) : 0u;
}

bool ui_timeline_row_info(const ccord_ui *ui, ccord_id channel_id, size_t view_i,
                          bool *out_is_local, size_t *out_store_i, uint64_t *out_local_id,
                          int64_t *out_ts_ms, ccord_id *out_author_id, bool *out_break_group,
                          ccord_local_compact *out_compact_mode)
{
  if (!ui || channel_id == CCORD_ID_NONE) return false;
  if (tl_cache_matches(ui, channel_id)) {
    const struct ui_chat_caches *cc = ui->chat_caches;
    if (view_i >= cc->tl_count) return false;
    const timeline_row *tr = &cc->tl_rows[view_i];
    if (out_is_local) *out_is_local = tr->is_local;
    if (out_store_i) *out_store_i = tr->store_i;
    if (out_local_id) *out_local_id = tr->local_id;
    if (out_ts_ms) *out_ts_ms = tr->timestamp_ms;
    if (out_author_id) *out_author_id = tr->author_id;
    if (out_break_group) *out_break_group = tr->break_group;
    if (out_compact_mode) *out_compact_mode = tr->compact_mode;
    return true;
  }
  const ccord_message *m = ui->d.store ? ccord_store_message_at(ui->d.store, channel_id, view_i) : nullptr;
  if (!m) return false;
  if (out_is_local) *out_is_local = false;
  if (out_store_i) *out_store_i = view_i;
  if (out_local_id) *out_local_id = 0;
  if (out_ts_ms) *out_ts_ms = m->timestamp_ms;
  if (out_author_id) *out_author_id = m->author_id;
  if (out_break_group) *out_break_group = false;
  if (out_compact_mode) *out_compact_mode = CCORD_LOCAL_COMPACT_AUTO;
  return true;
}

size_t ui_timeline_view_index_for_store(const ccord_ui *ui, ccord_id channel_id, size_t store_i)
{
  if (!ui || channel_id == CCORD_ID_NONE || store_i == SIZE_MAX) return SIZE_MAX;
  if (!tl_cache_matches(ui, channel_id)) return store_i;
  const struct ui_chat_caches *cc = ui->chat_caches;
  for (size_t i = 0; i < cc->tl_count; i++) {
    if (!cc->tl_rows[i].is_local && cc->tl_rows[i].store_i == store_i) return i;
  }
  return SIZE_MAX;
}

size_t ui_timeline_view_index_for_msg_id(const ccord_ui *ui, ccord_id channel_id, ccord_id msg_id)
{
  if (!ui || channel_id == CCORD_ID_NONE || msg_id == CCORD_ID_NONE) return SIZE_MAX;
  size_t lo = 0, hi = ccord_store_message_count(ui->d.store, channel_id);
  size_t found_si = SIZE_MAX;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    const ccord_message *m = ccord_store_message_at(ui->d.store, channel_id, mid);
    if (!m) break;
    if (m->id == msg_id) { found_si = mid; break; }
    if (m->id < msg_id) lo = mid + 1u;
    else hi = mid;
  }
  if (found_si == SIZE_MAX) return SIZE_MAX;
  return ui_timeline_view_index_for_store(ui, channel_id, found_si);
}

size_t ui_timeline_view_index_for_local_id(const ccord_ui *ui, ccord_id channel_id, uint64_t local_id)
{
  if (!ui || channel_id == CCORD_ID_NONE || local_id == 0) return SIZE_MAX;
  if (!tl_cache_matches(ui, channel_id)) return SIZE_MAX;
  const struct ui_chat_caches *cc = ui->chat_caches;
  for (size_t i = 0; i < cc->tl_count; i++) {
    if (cc->tl_rows[i].is_local && cc->tl_rows[i].local_id == local_id) return i;
  }
  return SIZE_MAX;
}

const ccord_local_message *dc_view_local_at(const ccord_ui *ui, ccord_id channel, size_t view_i)
{
  if (!ui || !ui->d.contrib || !tl_cache_matches(ui, channel)) return nullptr;
  const struct ui_chat_caches *cc = ui->chat_caches;
  if (view_i >= cc->tl_count || !cc->tl_rows[view_i].is_local) return nullptr;
  return ccord_contrib_find_local_message(ui->d.contrib, cc->tl_rows[view_i].local_id);
}

const ccord_message *dc_view_message_at(const ccord_ui *ui, ccord_id channel, size_t view_i, size_t *out_store_i)
{
  size_t raw = (size_t)ui_view_index(ui, view_i);
  if (raw == SIZE_MAX) {
    if (out_store_i) *out_store_i = SIZE_MAX;
    return nullptr;
  }
  if ((raw & (1ULL << 62)) != 0) {
    size_t si = raw & ~(1ULL << 62);
    if (out_store_i) *out_store_i = SIZE_MAX;
    size_t sc = 0;
    const ccord_message *sr = ui->d.session ? ccord_session_search_results(ui->d.session, &sc) : nullptr;
    return (sr && si < sc) ? &sr[si] : nullptr;
  }
  if (out_store_i) *out_store_i = raw;
  return ccord_store_message_at(ui->d.store, channel, raw);
}

// ---- Parsed local message attachments & embeds cache ----

static const char *lp_astr(ccord_arena *a, const ccord_json *obj, const char *key)
{
  if (!obj) return nullptr;
  size_t n = 0;
  const char *s = ccord_json_string(ccord_json_get(obj, key), &n);
  return (s && n > 0) ? ccord_arena_strndup(a, s, n) : nullptr;
}

static void parse_local_embed_obj(ccord_arena *a, const ccord_json *ej, ccord_embed *out, const char **out_img_key)
{
  memset(out, 0, sizeof *out);
  *out_img_key = nullptr;
  if (!ej || ccord_json_type_of(ej) != CCORD_JSON_OBJECT) return;
  out->title = lp_astr(a, ej, "title");
  out->description = lp_astr(a, ej, "description");
  out->url = lp_astr(a, ej, "url");
  const ccord_json *aj = ccord_json_get(ej, "author");
  if (aj && ccord_json_type_of(aj) == CCORD_JSON_OBJECT) {
    out->author_name = lp_astr(a, aj, "name");
    out->author_url = lp_astr(a, aj, "url");
  }
  const ccord_json *fj = ccord_json_get(ej, "footer");
  if (fj && ccord_json_type_of(fj) == CCORD_JSON_OBJECT) {
    out->footer_text = lp_astr(a, fj, "text");
  }
  const ccord_json *cj = ccord_json_get(ej, "color");
  if (cj && ccord_json_type_of(cj) == CCORD_JSON_NUMBER) {
    int64_t cv = 0;
    if (ccord_json_int64(cj, &cv) && cv >= 0) {
      out->color = (uint32_t)cv & 0xFFFFFFu;
      out->has_color = true;
    }
  } else if (cj && ccord_json_type_of(cj) == CCORD_JSON_STRING) {
    size_t cl = 0; const char *cs = ccord_json_string(cj, &cl);
    if (cs && cs[0] == '#' && cl == 7) {
      uint32_t cv = (uint32_t)strtoul(cs + 1, nullptr, 16);
      out->color = cv & 0xFFFFFFu;
      out->has_color = true;
    }
  }
  const ccord_json *tsj = ccord_json_get(ej, "timestamp_ms");
  if (tsj) (void)ccord_json_int64(tsj, &out->timestamp_ms);

  // Texture key or already-sanitized HTTPS URL on embed image/thumbnail.
  const ccord_json *imj = ccord_json_get(ej, "image");
  if (imj && ccord_json_type_of(imj) == CCORD_JSON_OBJECT) {
    *out_img_key = lp_astr(a, imj, "texture_key");
    const char *iu = lp_astr(a, imj, "url");
    if (iu) {
      out->image.url = iu;
      out->image.proxy_url = iu;
      int64_t iw = 0, ih = 0;
      (void)ccord_json_int64(ccord_json_get(imj, "width"), &iw);
      (void)ccord_json_int64(ccord_json_get(imj, "height"), &ih);
      if (iw > 0) out->image.width = (uint32_t)iw;
      if (ih > 0) out->image.height = (uint32_t)ih;
    }
  } else {
    const char *itk = lp_astr(a, ej, "image_texture");
    if (itk) *out_img_key = itk;
    const char *iu = lp_astr(a, ej, "image_url");
    if (iu) { out->image.url = iu; out->image.proxy_url = iu; }
  }
  const ccord_json *thj = ccord_json_get(ej, "thumbnail");
  if (thj && ccord_json_type_of(thj) == CCORD_JSON_OBJECT) {
    if (!*out_img_key) *out_img_key = lp_astr(a, thj, "texture_key");
    const char *tu = lp_astr(a, thj, "url");
    if (tu) {
      out->thumbnail.url = tu;
      out->thumbnail.proxy_url = tu;
      int64_t tw = 0, th = 0;
      (void)ccord_json_int64(ccord_json_get(thj, "width"), &tw);
      (void)ccord_json_int64(ccord_json_get(thj, "height"), &th);
      if (tw > 0) out->thumbnail.width = (uint32_t)tw;
      if (th > 0) out->thumbnail.height = (uint32_t)th;
    }
  } else {
    if (!*out_img_key) {
      const char *ttk = lp_astr(a, ej, "thumbnail_texture");
      if (ttk) *out_img_key = ttk;
    }
    const char *tu = lp_astr(a, ej, "thumbnail_url");
    if (tu) { out->thumbnail.url = tu; out->thumbnail.proxy_url = tu; }
  }
  const ccord_json *flj = ccord_json_get(ej, "fields");
  if (flj && ccord_json_type_of(flj) == CCORD_JSON_ARRAY) {
    size_t nf = ccord_json_array_len(flj);
    if (nf > 25u) nf = 25u;
    if (nf > 0) {
      ccord_embed_field *flds = ccord_arena_alloc(a, nf * sizeof *flds, _Alignof(ccord_embed_field));
      if (flds) {
        size_t cnt = 0;
        for (size_t i = 0; i < nf; i++) {
          const ccord_json *fi = ccord_json_array_at(flj, i);
          if (!fi || ccord_json_type_of(fi) != CCORD_JSON_OBJECT) continue;
          flds[cnt++] = (ccord_embed_field){
            .name = lp_astr(a, fi, "name"),
            .value = lp_astr(a, fi, "value"),
            .inline_ = ccord_json_bool(ccord_json_get(fi, "inline"), false),
          };
        }
        out->fields = flds;
        out->field_count = cnt;
      }
    }
  }
}

const local_msg_parsed *dc_local_parsed_get(ccord_ui *ui, const ccord_local_message *lm)
{
  if (!ui || !lm) return nullptr;
  struct ui_chat_caches *cc = dc_chat_caches_ensure(ui);
  if (!cc) return nullptr;
  size_t victim = 0;
  for (size_t i = 0; i < LOCAL_PARSE_CACHE_SLOTS; i++) {
    if (cc->lparse[i].inited && cc->lparse[i].local_id == lm->local_id && cc->lparse[i].revision == lm->revision) {
      cc->lparse[i].lru_tick = ++cc->lparse_tick;
      return &cc->lparse[i];
    }
    if (!cc->lparse[i].inited) victim = i;
    else if (cc->lparse[victim].inited && cc->lparse[i].lru_tick < cc->lparse[victim].lru_tick) victim = i;
  }
  local_msg_parsed *sl = &cc->lparse[victim];
  if (!sl->inited) {
    ccord_arena_init(&sl->arena, 0);
    sl->inited = true;
  } else {
    ccord_arena_reset(&sl->arena);
  }
  sl->local_id = lm->local_id;
  sl->revision = lm->revision;
  sl->lru_tick = ++cc->lparse_tick;
  sl->natts = 0;
  sl->nembeds = 0;

  static const ccord_json_limits k_llim = {12, 2048, 65536};
  ccord_error err = {0};
  if (lm->attachments_json && lm->attachments_json[0]) {
    const ccord_json *aj = ccord_json_parse(&sl->arena, lm->attachments_json, strlen(lm->attachments_json), &k_llim, &err);
    if (aj && ccord_json_type_of(aj) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(aj);
      if (n > LOCAL_MAX_ATTS) n = LOCAL_MAX_ATTS;
      for (size_t i = 0; i < n; i++) {
        const ccord_json *it = ccord_json_array_at(aj, i);
        if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
        int64_t sz = 0, w = 0, h = 0;
        (void)ccord_json_int64(ccord_json_get(it, "size"), &sz);
        (void)ccord_json_int64(ccord_json_get(it, "width"), &w);
        (void)ccord_json_int64(ccord_json_get(it, "height"), &h);
        sl->atts[sl->natts++] = (local_att_parsed){
          .filename = lp_astr(&sl->arena, it, "filename"),
          .content_type = lp_astr(&sl->arena, it, "content_type"),
          .texture_key = lp_astr(&sl->arena, it, "texture_key"),
          .size = sz > 0 ? (uint64_t)sz : 0u,
          .width = w > 0 ? (uint32_t)w : 0u,
          .height = h > 0 ? (uint32_t)h : 0u,
          .spoiler = ccord_json_bool(ccord_json_get(it, "spoiler"), false),
        };
      }
    }
  }
  if (lm->embed_json && lm->embed_json[0]) {
    const ccord_json *ej = ccord_json_parse(&sl->arena, lm->embed_json, strlen(lm->embed_json), &k_llim, &err);
    if (ej && ccord_json_type_of(ej) == CCORD_JSON_ARRAY) {
      size_t n = ccord_json_array_len(ej);
      if (n > LOCAL_MAX_EMBEDS) n = LOCAL_MAX_EMBEDS;
      for (size_t i = 0; i < n; i++) {
        const ccord_json *it = ccord_json_array_at(ej, i);
        if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
        parse_local_embed_obj(&sl->arena, it, &sl->embeds[sl->nembeds], &sl->embed_img_keys[sl->nembeds]);
        sl->nembeds++;
      }
    } else if (ej && ccord_json_type_of(ej) == CCORD_JSON_OBJECT) {
      parse_local_embed_obj(&sl->arena, ej, &sl->embeds[0], &sl->embed_img_keys[0]);
      sl->nembeds = 1;
    }
  }
  return sl;
}

static void fit_thumb_box(int tw, int th, float max_w, float max_h, float *out_w, float *out_h)
{
  float fw = tw > 0 ? (float)tw : 240.0f;
  float fh = th > 0 ? (float)th : 160.0f;
  float s = fminf(1.0f, fminf(max_w / fw, max_h / fh));
  *out_w = maxf(floorf(fw * s), 48.0f);
  *out_h = maxf(floorf(fh * s), 48.0f);
}

float dc_local_cards_walk(ccord_ui *ui, ccord_renderer *r, const ccord_local_message *lm,
                          float x, float y, float card_w, bool draw)
{
  const local_msg_parsed *lp = dc_local_parsed_get(ui, lm);
  if (!lp || (lp->natts == 0 && lp->nembeds == 0)) return 0.0f;
  float cy = 0.0f;
  const float gap = M(message_line_gap) + 4.0f;
  const float max_img_w = fminf(card_w, 400.0f);
  const float max_img_h = 300.0f;

  for (size_t i = 0; i < lp->natts; i++) {
    const local_att_parsed *a = &lp->atts[i];
    int tw = 0, th = 0;
    bool has_tex = r && a->texture_key && a->texture_key[0] && ccord_render_has_texture(r, a->texture_key, &tw, &th);
    if (has_tex) {
      if (tw <= 0 && a->width > 0) tw = (int)a->width;
      if (th <= 0 && a->height > 0) th = (int)a->height;
      float dw = 0.0f, dh = 0.0f;
      fit_thumb_box(tw, th, max_img_w, max_img_h, &dw, &dh);
      if (draw) {
        ccord_rect rc = {x, y + cy, dw, dh};
        ccord_draw_texture_rounded(r, a->texture_key, rc, 6.0f, 255);
      }
      cy += dh + gap;
    } else {
      if (draw) {
        ccord_attachment fa = {
          .filename = a->filename ? a->filename : "attachment",
          .content_type = a->content_type ? a->content_type : "",
          .size = a->size,
          .width = a->width,
          .height = a->height,
        };
        dc_draw_attachment_card(ui, r, &fa, x, y + cy, card_w);
      }
      cy += 46.0f + gap;
    }
  }
  for (size_t i = 0; i < lp->nembeds; i++) {
    decor_part ep = {
      .kind = DECOR_EMBED,
      .emb = &lp->embeds[i],
      .count = 1u,
      .msg_id = lm->local_id | (1ULL << 62),
      .edit_rev = (uint32_t)lm->revision,
    };
    float eh = dc_embed_card_walk(ui, r, &ep, x, y + cy, card_w, draw);
    if (eh > 0.0f) cy += eh + gap;
    const char *ek = lp->embed_img_keys[i];
    int tw = 0, th = 0;
    if (r && ek && ek[0] && ccord_render_has_texture(r, ek, &tw, &th)) {
      float dw = 0.0f, dh = 0.0f;
      fit_thumb_box(tw, th, max_img_w, max_img_h, &dw, &dh);
      if (draw) ccord_draw_texture_rounded(r, ek, (ccord_rect){x, y + cy, dw, dh}, 6.0f, 255);
      cy += dh + gap;
    }
  }
  return cy;
}

static const local_msg_parsed *dc_decor_embeds_parsed_get(ccord_ui *ui, ccord_id msg_id, const char *embeds_json)
{
  if (!ui || !embeds_json || !embeds_json[0]) return nullptr;
  struct ui_chat_caches *cc = dc_chat_caches_ensure(ui);
  if (!cc) return nullptr;
  uint64_t key_id = msg_id | (1ULL << 63);
  uint64_t rev = ui->d.contrib ? ccord_contrib_decorators_revision(ui->d.contrib) : 1u;
  size_t victim = 0;
  for (size_t i = 0; i < LOCAL_PARSE_CACHE_SLOTS; i++) {
    if (cc->lparse[i].inited && cc->lparse[i].local_id == key_id && cc->lparse[i].revision == rev) {
      cc->lparse[i].lru_tick = ++cc->lparse_tick;
      return &cc->lparse[i];
    }
    if (!cc->lparse[i].inited) victim = i;
    else if (cc->lparse[victim].inited && cc->lparse[i].lru_tick < cc->lparse[victim].lru_tick) victim = i;
  }
  local_msg_parsed *sl = &cc->lparse[victim];
  if (!sl->inited) {
    ccord_arena_init(&sl->arena, 0);
    sl->inited = true;
  } else {
    ccord_arena_reset(&sl->arena);
  }
  sl->local_id = key_id;
  sl->revision = rev;
  sl->lru_tick = ++cc->lparse_tick;
  sl->natts = 0;
  sl->nembeds = 0;

  static const ccord_json_limits k_elim = {12, 4096, 131072};
  ccord_error err = {0};
  const ccord_json *ej = ccord_json_parse(&sl->arena, embeds_json, strlen(embeds_json), &k_elim, &err);
  if (ej && ccord_json_type_of(ej) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(ej);
    if (n > LOCAL_MAX_EMBEDS) n = LOCAL_MAX_EMBEDS;
    for (size_t i = 0; i < n; i++) {
      const ccord_json *it = ccord_json_array_at(ej, i);
      if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
      parse_local_embed_obj(&sl->arena, it, &sl->embeds[sl->nembeds], &sl->embed_img_keys[sl->nembeds]);
      sl->nembeds++;
    }
  } else if (ej && ccord_json_type_of(ej) == CCORD_JSON_OBJECT) {
    parse_local_embed_obj(&sl->arena, ej, &sl->embeds[0], &sl->embed_img_keys[0]);
    sl->nembeds = 1;
  }
  return sl;
}

float dc_decor_appended_embeds_walk(ccord_ui *ui, ccord_renderer *r, ccord_id msg_id, const char *embeds_json,
                                    float x, float y, float card_w, bool draw)
{
  const local_msg_parsed *lp = dc_decor_embeds_parsed_get(ui, msg_id, embeds_json);
  if (!lp || lp->nembeds == 0) return 0.0f;
  float cy = 0.0f;
  const float gap = M(message_line_gap) + 4.0f;
  const float max_img_w = fminf(card_w, 400.0f);
  const float max_img_h = 300.0f;
  for (size_t i = 0; i < lp->nembeds; i++) {
    decor_part ep = {
      .kind = DECOR_EMBED,
      .emb = &lp->embeds[i],
      .count = 1u,
      .msg_id = msg_id | (1ULL << 63),
      .edit_rev = (uint32_t)lp->revision,
    };
    float eh = dc_embed_card_walk(ui, r, &ep, x, y + cy, card_w, draw);
    if (eh > 0.0f) cy += eh + gap;
    const char *ek = lp->embed_img_keys[i];
    int tw = 0, th = 0;
    if (r && ek && ek[0] && ccord_render_has_texture(r, ek, &tw, &th)) {
      float dw = 0.0f, dh = 0.0f;
      fit_thumb_box(tw, th, max_img_w, max_img_h, &dw, &dh);
      if (draw) ccord_draw_texture_rounded(r, ek, (ccord_rect){x, y + cy, dw, dh}, 6.0f, 255);
      cy += dh + gap;
    }
  }
  return cy;
}
// ---- Blocked-author runs ----

static bool msg_author_blocked(const ccord_ui *ui, const ccord_message *m)
{
  return m && m->author_id != CCORD_ID_NONE && !ccord_message_is_ephemeral(m) &&
         (ccord_store_relationship(ui->d.store, m->author_id) == CCORD_REL_BLOCKED ||
          ui_user_is_ignored(ui, m->author_id));
}

static bool blocked_is_revealed(const ccord_ui *ui, ccord_id first_id)
{
  for (size_t k = 0; k < ui->nblocked_revealed; k++) {
    if (ui->blocked_revealed[k] == first_id) return true;
  }
  return false;
}

blocked_run dc_blocked_run_at(const ccord_ui *ui, ccord_id channel, size_t store_i, const ccord_message *m)
{
  blocked_run br = {0};
  if (store_i == SIZE_MAX || !msg_author_blocked(ui, m)) return br;
  br.blocked = true;
  struct ui_chat_caches *cc = ui->chat_caches;
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (cc && cc->br_valid && cc->br_rev == rev && cc->br_blocked_sig == ui->blocked_sig &&
      cc->br_chan == channel && store_i >= cc->br_a && store_i <= cc->br_b) {
    br.first = cc->br_a == store_i;
    br.first_id = cc->br_first_id;
    br.count = (uint32_t)(cc->br_b - cc->br_a + 1u);
    br.revealed = blocked_is_revealed(ui, br.first_id);
    return br;
  }
  size_t a = store_i;
  while (a > 0 && msg_author_blocked(ui, ccord_store_message_at(ui->d.store, channel, a - 1))) a--;
  size_t total = ccord_store_message_count(ui->d.store, channel);
  size_t b = store_i;
  while (b + 1 < total && msg_author_blocked(ui, ccord_store_message_at(ui->d.store, channel, b + 1))) b++;
  const ccord_message *fm = ccord_store_message_at(ui->d.store, channel, a);
  br.first = a == store_i;
  br.first_id = fm ? fm->id : m->id;
  br.count = (uint32_t)(b - a + 1);
  br.revealed = blocked_is_revealed(ui, br.first_id);
  if (cc) {
    cc->br_valid = true;
    cc->br_rev = rev;
    cc->br_blocked_sig = ui->blocked_sig;
    cc->br_chan = channel;
    cc->br_a = a;
    cc->br_b = b;
    cc->br_first_id = br.first_id;
  }
  return br;
}

void ui_blocked_reveal_for(ccord_ui *ui, ccord_id channel_id, size_t store_i)
{
  const ccord_message *m = ccord_store_message_at(ui->d.store, channel_id, store_i);
  blocked_run br = dc_blocked_run_at(ui, channel_id, store_i, m);
  if (!br.blocked || br.revealed) return;
  if (ui->nblocked_revealed == sizeof ui->blocked_revealed / sizeof ui->blocked_revealed[0]) {
    memmove(ui->blocked_revealed, ui->blocked_revealed + 1, (ui->nblocked_revealed - 1) * sizeof(ccord_id));
    ui->nblocked_revealed--;
  }
  ui->blocked_revealed[ui->nblocked_revealed++] = br.first_id;
  ui_chat_caches_clear(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

uint64_t ui_blocked_signature(const ccord_ui *ui)
{
  uint64_t sig = 1469598103934665603ULL;
  size_t n = ccord_store_relationship_count(ui->d.store);
  for (size_t i = 0; i < n; i++) {
    const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
    if (rel && rel->kind == CCORD_RELATIONSHIP_BLOCKED) sig += (rel->user_id ^ 0x9e3779b97f4a7c15ULL) * 1099511628211ULL;
  }
  for (size_t i = 0; i < ui->nignored_users; i++) {
    sig += (ui->ignored_users[i] ^ 0x9e3779b97f4a7c15ULL) * 1099511628211ULL;
  }
  return sig;
}

bool ui_blocked_toggle_at(ccord_ui *ui, float x, float y)
{
  for (size_t i = 0; i < ui->nblocked_hits; i++) {
    if (!ui_in_rect(x, y, ui->blocked_hits[i].rect)) continue;
    ccord_id first = ui->blocked_hits[i].first_id;
    if (ui->blocked_hits[i].revealed) {
      for (size_t k = 0; k < ui->nblocked_revealed; k++) {
        if (ui->blocked_revealed[k] != first) continue;
        memmove(&ui->blocked_revealed[k], &ui->blocked_revealed[k + 1],
                (ui->nblocked_revealed - k - 1) * sizeof(ccord_id));
        ui->nblocked_revealed--;
        break;
      }
    } else if (ui->nblocked_revealed < sizeof ui->blocked_revealed / sizeof ui->blocked_revealed[0]) {
      ui->blocked_revealed[ui->nblocked_revealed++] = first;
    }
    ui_chat_caches_clear(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
    return true;
  }
  return false;
}

// The strip above a run: "N blocked messages" and a Show or Hide control. Its rect is the click target.
void dc_draw_blocked_strip(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma, float y, const blocked_run *br)
{
  float x = ma->x + M(message_pad_x);
  float w = ma->w - 2.0f * M(message_pad_x);
  ccord_rect strip = {x, y + 2.0f, w, UI_BLOCKED_ROW_H - 4.0f};
  char txt[64];
  snprintf(txt, sizeof txt, "%u blocked message%s", (unsigned)br->count, br->count == 1u ? "" : "s");
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  float ty = strip.y + (strip.h - ui_line_h(ui, ts.size)) * 0.5f;
  ccord_text_draw(r, txt, strlen(txt), strip.x + 4.0f, ty, &ts, 0.0f);
  float tw = ccord_text_measure(r, txt, strlen(txt), &ts, 0.0f).x;
  const char *act = br->revealed ? "Hide" : "Show";
  ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_link));
  ccord_text_draw(r, act, strlen(act), strip.x + 4.0f + tw + 12.0f, ty, &as, 0.0f);
  float aw = ccord_text_measure(r, act, strlen(act), &as, 0.0f).x;
  ccord_rect hit = {strip.x, strip.y, tw + 12.0f + aw + 8.0f, strip.h};
  if (ui->nblocked_hits < sizeof ui->blocked_hits / sizeof ui->blocked_hits[0]) {
    ui->blocked_hits[ui->nblocked_hits++] = (typeof(ui->blocked_hits[0])){hit, br->first_id, br->count, br->revealed};
  }
}

static uint64_t hash_bytes_fnv(const char *s, size_t n)
{
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
  return h;
}

void dc_rowh_cache_sync(struct ui_chat_caches *cc, const ccord_ui *ui, const ccord_renderer *r)
{
  int tex_cnt = r ? ccord_render_texture_cache_count(r) : 0;
  if (cc->rh_r != r || cc->rh_theme != ui->theme || cc->rh_tex_count != tex_cnt ||
      cc->rh_nrevealed_spoilers != ui->nrevealed_spoilers ||
      cc->rh_nblocked_revealed != ui->nblocked_revealed || cc->rh_blocked_sig != ui->blocked_sig) {
    cc->rh_r = r;
    cc->rh_theme = ui->theme;
    cc->rh_tex_count = tex_cnt;
    cc->rh_nrevealed_spoilers = ui->nrevealed_spoilers;
    cc->rh_nblocked_revealed = ui->nblocked_revealed;
    cc->rh_blocked_sig = ui->blocked_sig;
    memset(cc->rowh, 0, sizeof cc->rowh);
    memset(cc->embedh, 0, sizeof cc->embedh);
  }
}

// Emoji-only chat messages (up to UI_JUMBO_EMOJI_MAX Unicode or custom emojis) render enlarged (3x base font = 48 px).
size_t dc_chat_message_jumbo(const ccord_ui *ui, const ccord_message *m,
                                 ui_jumbo_item out[static UI_JUMBO_EMOJI_MAX])
{
  if (!m || !m->content || m->content[0] == '\0') return 0;
  if (m->rich && ccord_message_type_is_system(m->rich->type)) return 0;
  if (message_should_hide_bare_link(ui, m)) return 0;
  return ui_parse_jumbo_emojis(m->content, strlen(m->content), out);
}

float dc_chat_jumbo_emoji_walk(ccord_ui *ui, ccord_renderer *r, const ui_jumbo_item *items, size_t count,
                                   float x, float y, float wrap, ccord_color base, bool show_edited,
                                   bool show_tts, bool draw)
{
  float jumbo_sz = M(font_size_base) * 3.0f;   // 48 px at default 16 px base font
  float gap = 4.0f;
  float line_h = jumbo_sz + 4.0f;
  float top_pad = 4.0f;
  float cx = 0.0f, cy = top_pad;
  bool line_has = false;
  for (size_t i = 0; i < count; i++) {
    const ui_jumbo_item *it = &items[i];
    if (line_has && (it->line_break_before || cx + jumbo_sz > wrap)) {
      if (draw) {
        dc_chat_sel_line_end(ui, r, x + cx, y + cy, 6.0f, line_h, it->line_break_before);
        dc_record_text_hit(ui, (ccord_rect){x, y + cy, cx, line_h});
      }
      cx = 0.0f;
      cy += line_h;
      line_has = false;
    }
    if (draw) {
      ccord_rect er = {x + cx, y + cy + (line_h - jumbo_sz) * 0.5f, jumbo_sz, jumbo_sz};
      if (it->custom) {
        float sel_x0, sel_x1;
        bool goes_on;
        if (dc_chat_sel_line(ui, y + cy, line_h, &sel_x0, &sel_x1, &goes_on) &&
            sel_x1 > er.x && sel_x0 < er.x + er.w) {
          dc_chat_sel_fill(ui, r, (ccord_rect){er.x, y + cy, er.w, line_h});
          dc_chat_sel_append(ui, it->raw, it->raw_len, false);
        }
        ui_expr_draw_custom_emoji(ui, r, it->id, it->text, it->len, it->animated, er);
      } else {
        ccord_text_style es = ui_style(ui, CCORD_FONT_EMOJI, jumbo_sz, base);
        ccord_vec2 et = ccord_text_measure(r, it->text, it->len, &es, 0.0f);
        float ex = er.x + (er.w - et.x) * 0.5f;
        float ey = er.y + (er.h - et.y) * 0.5f;
        dc_chat_sel_process_run(ui, r, it->text, it->len, &es, ex, y + cy, maxf(et.x, jumbo_sz), line_h, 0.0f);
        ccord_text_draw(r, it->text, it->len, ex, ey, &es, 0.0f);
      }
    }
    cx += jumbo_sz + gap;
    line_has = true;
  }
  // Grouped/compact follow-up messages carry "(edited)" / "(tts)" inline at the end of the body.
  if (show_edited || show_tts) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float tag_lh = ui_line_h(ui, ts.size);
    const char *tags[2] = {show_edited ? "(edited)" : nullptr, show_tts ? "(tts)" : nullptr};
    for (int ti = 0; ti < 2; ti++) {
      if (!tags[ti]) continue;
      size_t tl = strlen(tags[ti]);
      float tw = ccord_text_measure(r, tags[ti], tl, &ts, 0.0f).x;
      if (line_has && cx + tw > wrap) {
        if (draw) {
          dc_chat_sel_line_end(ui, r, x + cx, y + cy, 6.0f, line_h, false);
          dc_record_text_hit(ui, (ccord_rect){x, y + cy, cx, line_h});
        }
        cx = 0.0f;
        cy += line_h;
        line_has = false;
      }
      if (draw) {
        float ty = y + cy + (line_h - tag_lh) - 4.0f;
        dc_chat_sel_process_run(ui, r, tags[ti], tl, &ts, x + cx, y + cy, tw, line_h, 0.0f);
        ccord_text_draw(r, tags[ti], tl, x + cx, ty, &ts, 0.0f);
      }
      cx += tw + 6.0f;
      line_has = true;
    }
  }
  if (draw) {
    dc_chat_sel_line_end(ui, r, x + cx, y + cy, 6.0f, line_h, true);
    dc_record_text_hit(ui, (ccord_rect){x, y + cy, cx, line_h});
  }
  return cy + line_h;
}

static float dc_measure_local_row(msg_ctx *c, size_t view_i, const ccord_local_message *lm, float width)
{
  ccord_ui *ui = c->ui;
  bool has_intro = dc_is_dm_intro_row(ui, c->channel, view_i);
  bool has_div = ui_view_has_date_divider(ui, c->channel, view_i);
  bool compact = ui_view_is_compact(ui, c->channel, view_i);
  float tpad = dc_tail_pad(ui, view_i);
  const char *content = lm->content ? lm->content : "";
  size_t content_len = strlen(content);

  struct ui_chat_caches *cc = dc_chat_caches_ensure(ui);
  int width_q = (int)lroundf(width * 2.0f);
  float zoom = c->r ? ccord_render_get_zoom(c->r) : 1.0f;
  int scale_q = (int)lroundf(zoom * 100.0f) * 256 + (int)lroundf(M(font_size_base) * 4.0f);
  uint32_t flags = (compact ? 1u : 0u) | (ui_messages_compact(ui) ? 2u : 0u) | (has_intro ? 4u : 0u) |
                   (has_div ? 8u : 0u) | (tpad > 0.0f ? (1u << 13) : 0u) | (ui->mobile_layout ? (1u << 18) : 0u) |
                   (1u << 30);
  uint32_t meta_sig = (uint32_t)(lm->revision * 2654435761u);
  if (ui->d.contrib) meta_sig ^= (uint32_t)(ccord_contrib_textures_revision(ui->d.contrib) * 40503u);
  ccord_id cache_id = lm->local_id | (1ULL << 62);
  uint64_t chash = hash_bytes_fnv(content, content_len);
  if (cc) {
    dc_rowh_cache_sync(cc, ui, c->r);
    for (size_t k = 0; k < CHAT_ROWH_CACHE_SLOTS; k++) {
      chat_rowh_entry *e = &cc->rowh[k];
      if (e->msg_id == cache_id && e->edit_rev == (uint32_t)lm->revision && e->content_len == content_len &&
          e->content_hash == chash && e->width_q == width_q && e->scale_q == scale_q &&
          e->flags == flags && e->meta_sig == meta_sig) {
        e->lru_tick = ++cc->rh_tick;
        return e->height;
      }
    }
  }

  float lh = ui_line_h(ui, M(font_size_base));
  float wrap = dc_content_wrap(ui, width);
  float intro_w = maxf(width - 2.0f * M(message_pad_x), 40.0f);
  float card_w = maxf(width - M(message_text_x) - M(message_pad_x), 40.0f);
  const local_msg_parsed *lp = dc_local_parsed_get(ui, lm);
  bool has_cards = lp && (lp->natts > 0 || lp->nembeds > 0);
  float body_h = (content_len == 0 && has_cards)
                   ? 0.0f
                   : maxf(dc_md_body_walk(ui, c->r, content, content_len, 0.0f, 0.0f, wrap, COL(text_normal), false), lh);
  float cards_h = has_cards ? (M(message_line_gap) * 0.5f + dc_local_cards_walk(ui, c->r, lm, 0.0f, 0.0f, card_w, false)) : 0.0f;
  float top = compact ? M(message_line_gap)
                      : (ui_messages_compact(ui)
                           ? M(message_line_gap) + (M(message_group_gap) - M(message_line_gap)) * 0.35f
                           : M(message_group_gap));
  float header = compact ? 0.0f : lh;
  float intro = has_intro
                  ? dc_dm_intro_walk(ui, c->r, ccord_store_channel(ui->d.store, c->channel),
                                     0.0f, 0.0f, intro_w, has_div, false)
                  : 0.0f;
  float div_h = has_div ? UI_CHAT_DIVIDER_H : 0.0f;
  float total_h = intro + div_h + top + header + body_h + cards_h + M(message_line_gap) + tpad;
  if (cc) {
    size_t victim = 0;
    for (size_t k = 0; k < CHAT_ROWH_CACHE_SLOTS; k++) {
      if (cc->rowh[k].msg_id == CCORD_ID_NONE) { victim = k; break; }
      if (cc->rowh[k].lru_tick < cc->rowh[victim].lru_tick) victim = k;
    }
    cc->rowh[victim] = (chat_rowh_entry){
      .content_hash = chash, .msg_id = cache_id, .edited_ts = 0,
      .edit_rev = (uint32_t)lm->revision, .content_len = content_len,
      .width_q = width_q, .scale_q = scale_q, .flags = flags, .meta_sig = meta_sig,
      .height = total_h, .lru_tick = ++cc->rh_tick,
    };
  }
  return total_h;
}

float dc_measure_row(size_t i, float width, void *ud)
{
  msg_ctx *c = ud;
  ccord_ui *ui = c->ui;
  const ccord_local_message *lm = dc_view_local_at(ui, c->channel, i);
  if (lm) return dc_measure_local_row(c, i, lm, width);
  size_t store_i = 0;
  const ccord_message *m = dc_view_message_at(ui, c->channel, i, &store_i);
  if (!m) return 1.0f;
  blocked_run br = dc_blocked_run_at(ui, c->channel, store_i, m);
  bool has_intro = dc_is_dm_intro_row(ui, c->channel, i);
  bool has_div = store_i != SIZE_MAX &&
                 (ui_view_has_date_divider(ui, c->channel, i) ||
                  ui_row_has_new_divider(ui, c->channel, store_i, m));
  float intro_w = maxf(width - 2.0f * M(message_pad_x), 40.0f);
  if (br.blocked) {
    float bintro = has_intro
                       ? dc_dm_intro_walk(ui, c->r, ccord_store_channel(ui->d.store, c->channel),
                                          0.0f, 0.0f, intro_w, has_div, false)
                       : 0.0f;
    float bdiv = has_div ? UI_CHAT_DIVIDER_H : 0.0f;
    if (!br.revealed) {
      return bintro + bdiv + (br.first ? UI_BLOCKED_ROW_H : UI_BLOCKED_HIDDEN_H) + dc_tail_pad(ui, i);
    }
    // revealed: the normal row below, plus the Hide strip above the first message of the run
  }
  bool is_system = m->rich && ccord_message_type_is_system(m->rich->type);
  bool has_wave = dc_message_has_wave_cta(ui, c->channel, store_i, m);
  bool compact = !is_system && store_i != SIZE_MAX && ui_view_is_compact(ui, c->channel, i);
  float tpad = dc_tail_pad(ui, i);
  const char *content = m->content ? m->content : "";
  size_t content_len = strlen(content);
  struct ui_chat_caches *cc = dc_chat_caches_ensure(ui);
  int width_q = (int)lroundf(width * 2.0f);
  float zoom = c->r ? ccord_render_get_zoom(c->r) : 1.0f;
  int scale_q = (int)lroundf(zoom * 100.0f) * 256 + (int)lroundf(M(font_size_base) * 4.0f);
  uint32_t flags = (compact ? 1u : 0u) | (ui_messages_compact(ui) ? 2u : 0u) | (has_intro ? 4u : 0u) |
                   (has_div ? 8u : 0u) | (br.blocked ? 16u : 0u) | (br.first ? 32u : 0u) |
                   (br.revealed ? 64u : 0u) | (ui->settings_embeds ? 128u : 0u) |
                   (ui->settings_attachments ? 256u : 0u) | (ui_streamer_invites_active(ui) ? 512u : 0u) |
                   (ccord_message_is_ephemeral(m) ? 1024u : 0u) | (((uint32_t)m->send_state & 3u) << 11) |
                   (tpad > 0.0f ? (1u << 13) : 0u) | (m->edited ? (1u << 14) : 0u) |
                   (m->reply_to_id != CCORD_ID_NONE ? (1u << 15) : 0u) | (m->tts ? (1u << 16) : 0u) |
                   (has_wave ? (1u << 17) : 0u) | (ui->mobile_layout ? (1u << 18) : 0u);
  uint32_t meta_sig = (uint32_t)(m->reaction_kind_count * 31u + m->reaction_count);
  if (ui->d.contrib) meta_sig ^= (uint32_t)(ccord_contrib_decorators_revision(ui->d.contrib) * 2654435761u);
  if (m->rich) {
    meta_sig ^= (uint32_t)((m->rich->attachment_count << 8) ^ (m->rich->embed_count << 12) ^
                           (m->rich->component_row_count << 16) ^ (m->rich->v2_component_count << 20) ^
                           (m->rich->sticker_count << 24) ^ (m->rich->poll.present ? (1u << 28) : 0u) ^
                           (m->rich->flags) ^ ((uint32_t)m->rich->type * 2654435761u));
  }
  if (has_intro) {
    const ccord_channel *ich = ccord_store_channel(ui->d.store, c->channel);
    bool is_grp = ui_channel_is_group_dm(ich);
    const ccord_user *du = !is_grp ? ui_dm_recipient(ui, ich) : nullptr;
    ccord_id duid = du ? du->id : (!is_grp && ich ? ich->user_id : CCORD_ID_NONE);
    uint32_t rk = duid != CCORD_ID_NONE ? (uint32_t)ccord_store_relationship(ui->d.store, duid) : 0u;
    size_t mg = duid != CCORD_ID_NONE ? dc_dm_intro_mutual_guilds(ui, duid, nullptr, 0u) : 0u;
    meta_sig ^= (rk << 3) ^ ((uint32_t)mg * 131u) ^ (is_grp ? 1u : 0u) ^
                (du && du->display_name ? (uint32_t)strlen(du->display_name) * 17u : 0u);
  }
  if (is_system) {
    const ccord_user *au = ccord_store_user(ui->d.store, m->author_id);
    if (au && au->name) meta_sig ^= (uint32_t)strlen(au->name) * 73u;
  }
  uint64_t chash = hash_bytes_fnv(content, content_len);
  if (cc && m->id != CCORD_ID_NONE) {
    dc_rowh_cache_sync(cc, ui, c->r);
    for (size_t k = 0; k < CHAT_ROWH_CACHE_SLOTS; k++) {
      chat_rowh_entry *e = &cc->rowh[k];
      if (e->msg_id == m->id && e->edited_ts == m->edited_timestamp_ms && e->edit_rev == m->edit_rev &&
          e->content_len == content_len &&
          e->content_hash == chash && e->width_q == width_q && e->scale_q == scale_q &&
          e->flags == flags && e->meta_sig == meta_sig) {
        e->lru_tick = ++cc->rh_tick;
        return e->height;
      }
    }
  }
  ui_msg_style_ov dec_st;
  bool has_dec_st = ui_decor_message_style_ex(ui, m->id, &dec_st);
  bool dec_collapsed = has_dec_st && dec_st.collapsed;
  decor_info d;
  dc_decor_scan(ui, m, compact, &d);
  float lh = ui_line_h(ui, M(font_size_base));
  float wrap = dc_content_wrap(ui, width);
  float body_h = 0.0f;
  if (is_system) {
    ccord_id guild = ccord_store_selected_guild(ui->d.store);
    body_h = dc_system_msg_walk(ui, c->r, m, guild, 0.0f, 0.0f, wrap, false, nullptr);
  } else {
    bool has_app_emb = has_dec_st && dec_st.embeds_append_json && dec_st.embeds_append_json[0];
    bool no_body = d.body_len == 0 && (dc_decor_has_body_cards(&d) || has_app_emb);
    ui_jumbo_item jumbo_items[UI_JUMBO_EMOJI_MAX];
    size_t jumbo_n = (no_body || (has_dec_st && dec_st.has_content_override)) ? 0u : dc_chat_message_jumbo(ui, m, jumbo_items);
    body_h = no_body
                 ? 0.0f
                 : (jumbo_n > 0u
                        ? dc_chat_jumbo_emoji_walk(ui, c->r, jumbo_items, jumbo_n, 0.0f, 0.0f, wrap,
                                                COL(text_normal), compact && dc_message_edited(m),
                                                compact && m->tts, false)
                        : maxf(dc_md_body_walk(ui, c->r, d.body, d.body_len, 0.0f, 0.0f, wrap, COL(text_normal), false), lh));
  }
  float top = is_system
                  ? M(message_group_gap) * 0.65f
                  : (compact ? M(message_line_gap)
                             : (ui_messages_compact(ui)
                                    ? M(message_line_gap) + (M(message_group_gap) - M(message_line_gap)) * 0.35f
                                    : M(message_group_gap)));
  float header = (compact || is_system) ? 0.0f : lh;
  float reactions = dc_message_has_reactions(m)
                        ? M(message_line_gap) + dc_reaction_pills_walk(ui, c->r, m, 0.0f, 0.0f, wrap, false)
                        : 0.0f;
  float ephem = ccord_message_is_ephemeral(m) ? UI_EPHEMERAL_FOOTER_H : 0.0f;
  float send_status = dc_send_status_extra_h(ui, m);
  float wave_h = has_wave ? dc_wave_cta_walk(ui, c->r, m, 0.0f, 0.0f, intro_w, false) : 0.0f;
  float intro = has_intro
                    ? dc_dm_intro_walk(ui, c->r, ccord_store_channel(ui->d.store, c->channel),
                                       0.0f, 0.0f, intro_w, has_div, false)
                    : 0.0f;
  float div_h = has_div ? UI_CHAT_DIVIDER_H : 0.0f;
  float card_w = maxf(width - M(message_text_x) - M(message_pad_x), 40.0f);
  float blocked_strip = (br.blocked && br.first) ? UI_BLOCKED_ROW_H : 0.0f;
  float app_emb_h = (!dec_collapsed && has_dec_st && dec_st.embeds_append_json && dec_st.embeds_append_json[0])
                      ? dc_decor_appended_embeds_walk(ui, c->r, m->id, dec_st.embeds_append_json, 0.0f, 0.0f, card_w, false)
                      : 0.0f;
  float acc_above_h = dec_collapsed
                        ? 0.0f
                        : ui_decor_measure_message_accessories_pos(ui, c->r, m->id, CCORD_DECOR_POS_ABOVE_BODY, card_w);
  float acc_below_h = dec_collapsed
                        ? 0.0f
                        : ui_decor_measure_message_accessories_pos(ui, c->r, m->id, CCORD_DECOR_POS_BELOW_BODY, card_w);
  float above_gap_top = (compact || is_system) ? 0.0f : 4.0f;
  float body_y_offset = acc_above_h > 0.0f ? (above_gap_top + acc_above_h + 4.0f) : 0.0f;
  float below_total = acc_below_h > 0.0f ? (4.0f + acc_below_h) : 0.0f;
  float total_h = dec_collapsed
                    ? (intro + div_h + blocked_strip + 28.0f + tpad)
                    : (intro + div_h + blocked_strip + top + dc_decor_reply_h(&d) + header + body_y_offset + body_h +
                       decor_total_h(ui, c->r, &d, card_w) + app_emb_h + reactions + below_total + send_status + ephem +
                       M(message_line_gap) + wave_h + tpad);
  if (cc && m->id != CCORD_ID_NONE) {
    size_t victim = 0;
    for (size_t k = 0; k < CHAT_ROWH_CACHE_SLOTS; k++) {
      if (cc->rowh[k].msg_id == CCORD_ID_NONE) {
        victim = k;
        break;
      }
      if (cc->rowh[k].lru_tick < cc->rowh[victim].lru_tick) victim = k;
    }
    cc->rowh[victim] = (chat_rowh_entry){
      .content_hash = chash,
      .msg_id = m->id,
      .edited_ts = m->edited_timestamp_ms,
      .content_len = content_len,
      .edit_rev = m->edit_rev,
      .width_q = width_q,
      .scale_q = scale_q,
      .flags = flags,
      .meta_sig = meta_sig,
      .height = total_h,
      .body_y_offset = body_y_offset,
      .lru_tick = ++cc->rh_tick,
    };
  }
  return total_h;
}

