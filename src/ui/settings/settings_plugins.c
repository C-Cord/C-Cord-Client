// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_internal.h"

#include <ccord/core/os.h>
#include <ccord/ipc/perm.h>

#include <ctype.h>

// ---------------------------------------------------------------- state

typedef enum sec_modal_mode : uint8_t {
  SEC_NONE = 0,
  SEC_CONSENT,   // local installed plugin in CCORD_PLUGIN_NEEDS_CONSENT
  SEC_INSTALL,   // catalog Class 2/3 install (atomic approve + install)
  SEC_UPDATE,    // catalog update requesting new Class 2/3 permissions (shows diff)
} sec_modal_mode;

typedef enum inst_filter : uint8_t {
  IFILT_ALL = 0,
  IFILT_ACTIVE,
  IFILT_ATTENTION,
  IFILT_UPDATES,
} inst_filter;

typedef enum plat_filter : uint8_t {
  PFILT_ALL = 0,
  PFILT_DESKTOP,
  PFILT_ANDROID,
} plat_filter;

typedef enum phit_act : uint8_t {
  ACT_NONE = 0,
  ACT_TAB,
  ACT_SYNC,
  ACT_INST_SEARCH_FOCUS,
  ACT_INST_SEARCH_CLEAR,
  ACT_INST_FILTER,
  ACT_DISC_SEARCH_FOCUS,
  ACT_DISC_SEARCH_CLEAR,
  ACT_DISC_SEARCH_GO,
  ACT_DISC_SORT_TOGGLE,
  ACT_DISC_SORT_PICK,
  ACT_DISC_TIER,
  ACT_DISC_CLASS1,
  ACT_DISC_PLAT,
  ACT_DISC_LOAD_MORE,
  ACT_CARD_TOGGLE,
  ACT_CARD_OVERFLOW,
  ACT_CARD_CONFIGURE,
  ACT_CARD_DETAILS,
  ACT_CARD_RELOAD,
  ACT_CARD_LOG,
  ACT_CARD_REVIEW_CONSENT,
  ACT_CARD_UPDATE_SAFE,
  ACT_CARD_UPDATE_REVIEW,
  ACT_CARD_UNINSTALL_ASK,
  ACT_CAT_DETAILS,
  ACT_CAT_INSTALL,
  ACT_UPD_RUN_SAFE,
  ACT_UPD_CHECK,
  ACT_UPD_POLICY_TOGGLE,
  ACT_UPD_POLICY_EXPAND,
  ACT_UPD_POLICY_TIER,
  ACT_DETAIL_BACK,
  ACT_DETAIL_RETRY,
  ACT_LOG_BACK,
  ACT_LOG_RELOAD,
  ACT_LOG_CLEAR,
  ACT_LOG_LEVEL,
  ACT_OV_DETAILS,
  ACT_OV_CONFIGURE,
  ACT_OV_AUTO_UPDATE,
  ACT_OV_RELOAD,
  ACT_OV_LOG,
  ACT_OV_EXPORT,
  ACT_OV_UNINSTALL_ASK,
  ACT_OV_CLOSE,
  ACT_SEC_APPROVE,
  ACT_SEC_CANCEL,
  ACT_UNINST_CONFIRM,
  ACT_UNINST_CANCEL,
} phit_act;

typedef struct phit_item {
  ccord_rect rc;
  phit_act act;
  int arg;
  char id[65];
} phit_item;

#define PHIT_MAIN_CAP    384u
#define PHIT_OVERLAY_CAP 48u

static phit_item *s_hits_main;
static size_t s_nhits_main;
static phit_item *s_hits_overlay;
static size_t s_nhits_overlay;

// Navigation & filters
static int s_subtab;              // 0=Installed, 1=Discover, 2=Updates
static inst_filter s_inst_filter; // Installed filter chip
static char s_inst_search[96];
static bool s_inst_search_focused;
static char s_disc_search[96];
static bool s_disc_search_focused;
static int s_disc_tier_filter;    // 0=All, 1=Official, 2=Verified, 3=Community
static int s_disc_sort_idx;       // 0=popular, 1=trending, 2=updated, 3=tier, 4=name
static bool s_disc_class1_only;
static plat_filter s_disc_plat_filter;
static bool s_policy_expanded;

// Sub-views & transient overlays
static char s_detail_id[65];
static char s_log_id[65];
static int s_log_level_filter;    // 0=All, 1=Info, 2=Warn, 3=Error
static float s_log_scroll;
static float s_log_content_h;
static char s_status_msg[192];

static bool s_sort_open;
static ccord_rect s_sort_anchor;
static ccord_rect s_sort_box;

static char s_overflow_id[65];
static ccord_rect s_overflow_anchor;
static ccord_rect s_overflow_box;

// Pending catalog detail fetch before popping Protected Security Modal
static char s_pending_sec_id[65];
static sec_modal_mode s_pending_sec_mode;

// Protected Security Modal state
static sec_modal_mode s_sec_mode;
static char s_sec_id[65];
static char s_sec_name[65];
static char s_sec_author[65];
static char s_sec_ver_old[33];
static char s_sec_ver_new[33];
static char s_sec_icon_path[CCORD_PATH_MAX];
static char s_sec_allowed_hosts[512];
static char s_sec_old_allowed_hosts[512];
static bool s_sec_has_old_hosts;
static char s_sec_changelog[512];
static char s_sec_warn_title[128];
static char s_sec_warn_msg[512];
static ccord_catalog_warn_sev s_sec_warn_sev;
static ccord_plugin_tier s_sec_tier;
static uint64_t s_sec_perms;
static uint64_t s_sec_old_perms;
static float s_sec_scroll;
static float s_sec_content_h;
static ccord_rect s_sec_modal_box;
static ccord_rect s_sec_body_rect;

// Uninstall confirmation modal state
static char s_uninst_id[65];
static char s_uninst_name[65];
static ccord_rect s_uninst_box;

// Cached geometry for body_y / scroll & visible scrollbars
static float s_body_top_cached;
static float s_page_view_h;
static ccord_rect s_page_sb_track;
static ccord_rect s_sec_sb_track;
static ccord_rect s_log_sb_track;

static void draw_simple_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect track,
                                  float scroll, float view_h, float content_h, ccord_rect *out_track)
{
  if (content_h <= view_h + 0.5f || track.h < 24.0f) {
    if (out_track) *out_track = (ccord_rect){0};
    return;
  }
  float max_s = content_h - view_h;
  float th = fmaxf(24.0f, track.h * (view_h / content_h));
  if (th > track.h) th = track.h;
  float frac = max_s > 0.0f ? (scroll / max_s) : 0.0f;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  float ty = track.y + (track.h - th) * frac;
  uis_scrollbar(ui, r, track, (ccord_rect){track.x, ty, track.w, th});
  if (out_track) *out_track = track;
}

static bool click_simple_scrollbar(ccord_ui *ui, float x, float y, ccord_rect track,
                                   float *scroll, float view_h, float content_h)
{
  if (track.w <= 0.0f || track.h < 24.0f || content_h <= view_h + 0.5f) return false;
  ccord_rect hit = {track.x - 5.0f, track.y, track.w + 10.0f, track.h};
  if (!ui_in_rect(x, y, hit)) return false;
  float max_s = content_h - view_h;
  float th = fmaxf(24.0f, track.h * (view_h / content_h));
  if (th > track.h) th = track.h;
  float span = track.h - th;
  float frac = span > 0.5f ? ((y - track.y - th * 0.5f) / span) : 0.0f;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  *scroll = frac * max_s;
  ui->dirty = true;
  return true;
}
static ccord_rect s_log_box_rect;

// Negative cache for invalid/oversized icon files so we never re-open them each frame
#define BAD_ICON_CAP 32u
static uint64_t s_bad_icons[BAD_ICON_CAP];
static size_t s_bad_icon_next;

// ---------------------------------------------------------------- tiny utilities

static void copy_str(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  if (!src) { dst[0] = '\0'; return; }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1u;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static bool contains_ci(const char *hay, const char *needle)
{
  if (!needle || !needle[0]) return true;
  if (!hay || !hay[0]) return false;
  size_t nlen = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t k = 0;
    while (k < nlen && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
    if (k == nlen) return true;
  }
  return false;
}

static uint64_t fnv64(const char *s)
{
  uint64_t h = 14695981039346656037ull;
  if (!s) return h;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= (uint64_t)(*p);
    h *= 1099511628211ull;
  }
  return h;
}

static ccord_color with_alpha(ccord_color c, uint8_t a)
{
  return (ccord_color){c.r, c.g, c.b, a};
}

static ccord_rect intersect_rect(ccord_rect a, ccord_rect b)
{
  float x0 = fmaxf(a.x, b.x);
  float y0 = fmaxf(a.y, b.y);
  float x1 = fminf(a.x + a.w, b.x + b.w);
  float y1 = fminf(a.y + a.h, b.y + b.h);
  if (x1 <= x0 || y1 <= y0) return (ccord_rect){0, 0, 0, 0};
  return (ccord_rect){x0, y0, x1 - x0, y1 - y0};
}

static void phit_add_list(phit_item *arr, size_t *cnt, size_t cap, ccord_rect rc, phit_act act, int arg, const char *id)
{
  if (*cnt >= cap || rc.w <= 0.0f || rc.h <= 0.0f) return;
  phit_item *it = &arr[(*cnt)++];
  it->rc = rc;
  it->act = act;
  it->arg = arg;
  copy_str(it->id, sizeof it->id, id);
}

static void hit_main(ccord_rect rc, phit_act act, int arg, const char *id)
{
  if (!s_hits_main) s_hits_main = ccord_alloc_array(PHIT_MAIN_CAP, sizeof *s_hits_main);
  if (!s_hits_main) return;
  phit_add_list(s_hits_main, &s_nhits_main, PHIT_MAIN_CAP, rc, act, arg, id);
}

static void hit_main_clipped(ccord_rect rc, ccord_rect clip, phit_act act, int arg, const char *id)
{
  ccord_rect cr = intersect_rect(rc, clip);
  if (cr.w > 0.0f && cr.h > 0.0f) hit_main(cr, act, arg, id);
}

static void hit_overlay(ccord_rect rc, phit_act act, int arg, const char *id)
{
  if (!s_hits_overlay) s_hits_overlay = ccord_alloc_array(PHIT_OVERLAY_CAP, sizeof *s_hits_overlay);
  if (!s_hits_overlay) return;
  phit_add_list(s_hits_overlay, &s_nhits_overlay, PHIT_OVERLAY_CAP, rc, act, arg, id);
}

void ui_settings_plugins_free_scratch(void)
{
  ccord_free(s_hits_main);
  s_hits_main = nullptr;
  s_nhits_main = 0;
  ccord_free(s_hits_overlay);
  s_hits_overlay = nullptr;
  s_nhits_overlay = 0;
}

static bool is_developer_mode(const ccord_ui *ui)
{
  if (!ui) return false;
  if (ui->settings_developer_mode) return true;
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  return us && us->developer_mode;
}

static bool is_running_mobile(const ccord_ui *ui)
{
#if CCORD_OS_ANDROID
  (void)ui;
  return true;
#else
  return ui && ui->mobile_layout;
#endif
}

static const ccord_plugin_info *find_plugin_info(const ccord_contrib *c, const char *id)
{
  if (!c || !id || !id[0]) return nullptr;
  size_t n = ccord_contrib_plugin_count(c);
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(c, i);
    if (p && strcmp(p->id, id) == 0) return p;
  }
  return nullptr;
}

static const ccord_catalog_entry *find_catalog_entry(const ccord_contrib *c, const char *id)
{
  if (!c || !id || !id[0]) return nullptr;
  const ccord_catalog_entry *det = ccord_contrib_catalog_detail(c);
  if (det && strcmp(det->id, id) == 0) return det;
  return ccord_contrib_catalog_find(c, id);
}

static const char *sort_key(int idx)
{
  static const char *const k[5] = {"popular", "trending", "updated", "tier", "name"};
  return (idx >= 0 && idx < 5) ? k[idx] : "popular";
}

static const char *sort_label(int idx)
{
  static const char *const k[5] = {"Popular", "Trending", "Recently Updated", "Trust Tier", "A–Z"};
  return (idx >= 0 && idx < 5) ? k[idx] : "Popular";
}

static const char *sort_short_label(int idx)
{
  static const char *const k[5] = {"Popular", "Trending", "Updated", "Trust Tier", "A–Z"};
  return (idx >= 0 && idx < 5) ? k[idx] : "Popular";
}

static const char *plat_filter_query(plat_filter pf)
{
  if (pf == PFILT_DESKTOP) return "desktop";
  if (pf == PFILT_ANDROID) return "android";
  return "";
}

static void trigger_discover_query(ccord_ui *ui, bool append_next_page)
{
  if (!ui || !ui->d.contrib) return;
  ccord_contrib_catalog_query_ex(ui->d.contrib, s_disc_search, s_disc_tier_filter,
                                 s_disc_class1_only ? 1 : 0, plat_filter_query(s_disc_plat_filter),
                                 sort_key(s_disc_sort_idx), append_next_page);
}

static void format_downloads(uint64_t n, char *out, size_t cap)
{
  if (n >= 1000000ull) snprintf(out, cap, "%.1fM downloads", (double)n / 1000000.0);
  else if (n >= 1000ull) snprintf(out, cap, "%.1fk downloads", (double)n / 1000.0);
  else snprintf(out, cap, "%llu download%s", (unsigned long long)n, n == 1 ? "" : "s");
}

static void format_bytes(uint64_t n, char *out, size_t cap)
{
  if (n >= 1024ull * 1024ull) snprintf(out, cap, "%.1f MiB", (double)n / (1024.0 * 1024.0));
  else if (n >= 1024ull) snprintf(out, cap, "%.1f KiB", (double)n / 1024.0);
  else snprintf(out, cap, "%llu B", (unsigned long long)n);
}

static uint32_t effective_platforms(uint32_t mask)
{
  return mask ? mask : CCORD_CONTRIB_PLAT_ALL;
}

static const char *platform_badge_label(uint32_t mask, bool mobile_host, bool *out_warn)
{
  uint32_t p = effective_platforms(mask);
  bool has_desk = (p & CCORD_CONTRIB_PLAT_DESKTOP) != 0;
  bool has_and = (p & CCORD_CONTRIB_PLAT_ANDROID) != 0;
  if (out_warn) *out_warn = mobile_host && !has_and;
  if (has_desk && has_and) return "Desktop + Android";
  if (has_and) return "Android Only";
  return "Desktop Only";
}

// ---------------------------------------------------------------- plugin icon tile (local/cached + monogram fallback)

static uint64_t bad_icon_hash(const char *path)
{
  uint64_t h = fnv64(path);
  return h == 0 ? 1u : h;
}

static bool icon_path_is_bad(const char *path)
{
  uint64_t h = bad_icon_hash(path);
  for (size_t i = 0; i < BAD_ICON_CAP; i++) {
    if (s_bad_icons[i] == h) return true;
  }
  return false;
}

static void mark_icon_path_bad(const char *path)
{
  s_bad_icons[s_bad_icon_next % BAD_ICON_CAP] = bad_icon_hash(path);
  s_bad_icon_next++;
}

static bool ensure_plugin_icon_texture(ccord_renderer *r, const char *icon_path, char tex_key_out[40])
{
  if (!r || !icon_path || !icon_path[0]) return false;
  uint64_t h = fnv64(icon_path);
  snprintf(tex_key_out, 40, "picon:%016llx", (unsigned long long)h);
  if (ccord_render_has_texture(r, tex_key_out, nullptr, nullptr)) return true;
  if (icon_path_is_bad(icon_path)) return false;

  uint64_t fsz = 0;
  if (!ccord_path_is_file(icon_path) || !ccord_file_size(icon_path, &fsz) || fsz == 0 || fsz > (256u * 1024u)) {
    mark_icon_path_bad(icon_path);
    return false;
  }
  FILE *fp = fopen(icon_path, "rb");
  if (!fp) {
    mark_icon_path_bad(icon_path);
    return false;
  }
  uint8_t *raw = ccord_malloc((size_t)fsz);
  if (!raw) {
    fclose(fp);
    return false;
  }
  size_t rd = fread(raw, 1, (size_t)fsz, fp);
  fclose(fp);
  if (rd != (size_t)fsz) {
    ccord_free(raw);
    mark_icon_path_bad(icon_path);
    return false;
  }
  uint8_t *rgba = nullptr;
  int iw = 0, ih = 0;
  ccord_error err = {0};
  ccord_status st = ccord_image_decode_rgba(raw, rd, &rgba, &iw, &ih, &err);
  ccord_free(raw);
  if (st != CCORD_OK || !rgba || iw <= 0 || ih <= 0 || iw > 512 || ih > 512) {
    ccord_free(rgba);
    mark_icon_path_bad(icon_path);
    return false;
  }
  st = ccord_render_upload_rgba(r, tex_key_out, rgba, iw, ih, &err);
  ccord_free(rgba);
  if (st != CCORD_OK) {
    mark_icon_path_bad(icon_path);
    return false;
  }
  return true;
}

static void plugin_initials(const char *name, char out[3])
{
  out[0] = 'P'; out[1] = '\0'; out[2] = '\0';
  if (!name || !name[0]) return;
  size_t k = 0;
  bool word_start = true;
  for (const unsigned char *p = (const unsigned char *)name; *p && k < 2; p++) {
    if (isspace(*p) || *p == '-' || *p == '_' || *p == '.') {
      word_start = true;
    } else if (word_start && isalnum(*p)) {
      out[k++] = (char)toupper(*p);
      word_start = false;
    }
  }
  if (k == 1) {
    for (const unsigned char *p = (const unsigned char *)name + 1; *p; p++) {
      if (isalnum(*p)) { out[k++] = (char)toupper(*p); break; }
    }
  }
  out[k] = '\0';
}

static ccord_color plugin_avatar_color(const ccord_ui *ui, const char *key)
{
  ccord_color pal[6] = {
    COL(avatar_0), COL(avatar_1), COL(avatar_2), COL(avatar_3), COL(avatar_4), COL(avatar_5),
  };
  return pal[fnv64(key ? key : "") % 6u];
}

static void draw_plugin_icon(ccord_ui *ui, ccord_renderer *r, ccord_rect tile,
                             const char *id, const char *name, const char *icon_path)
{
  uis_tokens tk = uis_tokens_of(ui);
  char tex_key[40];
  if (ensure_plugin_icon_texture(r, icon_path, tex_key)) {
    ccord_draw_texture_cover(r, tex_key, tile, tk.r_surface, 255, false);
    return;
  }
  ccord_color bg = plugin_avatar_color(ui, (id && id[0]) ? id : name);
  uis_plate(ui, r, tile, tk.r_surface, bg);
  char init[3];
  plugin_initials((name && name[0]) ? name : id, init);
  float fs = tile.w >= 44.0f ? tk.fs_body : tk.fs_control;
  ccord_text_style st = ui_style(ui, CCORD_FONT_BOLD, fs, COL(text_on_brand));
  ccord_vec2 sz = ccord_text_measure(r, init, strlen(init), &st, 0.0f);
  ccord_text_draw(r, init, strlen(init),
                  roundf(tile.x + (tile.w - sz.x) * 0.5f),
                  roundf(tile.y + (tile.h - sz.y) * 0.5f), &st, 0.0f);
}

// ---------------------------------------------------------------- badges, pills & buttons

static ccord_color tier_color(const ccord_ui *ui, ccord_plugin_tier t, bool bundled)
{
  if (bundled || t == CCORD_TIER_OFFICIAL) return COL(brand);
  if (t == CCORD_TIER_VERIFIED) return COL(status_online);
  return COL(text_muted);
}

static const char *tier_label(ccord_plugin_tier t, bool bundled)
{
  if (bundled) return "Bundled";
  switch (t) {
    case CCORD_TIER_OFFICIAL:   return "Official";
    case CCORD_TIER_VERIFIED:   return "Verified";
    case CCORD_TIER_UNVERIFIED: return "Community";
    default:                    return "Local";
  }
}

static ccord_color api_class_color(const ccord_ui *ui, uint8_t cls)
{
  if (cls >= 3) return COL(danger);
  if (cls == 2) return COL(status_idle);
  return COL(status_online);
}

static const char *api_class_label(uint8_t cls)
{
  if (cls >= 3) return "Class 3 · System";
  if (cls == 2) return "Class 2 · Internal";
  return "Class 1 · Safe";
}

static float measure_pill_w(ccord_ui *ui, ccord_renderer *r, const char *label)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, COL(text_header));
  return ceilf(ccord_text_measure(r, label, strlen(label), &ps, 0.0f).x) + 14.0f;
}

static float draw_pill(ccord_ui *ui, ccord_renderer *r, float x, float y, const char *label, ccord_color fg)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, fg);
  ccord_vec2 sz = ccord_text_measure(r, label, strlen(label), &ps, 0.0f);
  float pw = ceilf(sz.x) + 14.0f;
  ccord_rect rc = {x, y, pw, UIS_H_CHIP};
  uis_plate(ui, r, rc, UIS_H_CHIP * 0.5f, with_alpha(fg, 34));
  ccord_text_draw(r, label, strlen(label),
                  roundf(rc.x + 7.0f), roundf(rc.y + (rc.h - sz.y) * 0.5f), &ps, 0.0f);
  return pw;
}

typedef struct pill_flow {
  float x0, right, x, y;
  int lines;
} pill_flow;

static void pill_flow_init(pill_flow *f, float x0, float y0, float w)
{
  f->x0 = x0;
  f->right = x0 + fmaxf(w, 60.0f);
  f->x = x0;
  f->y = y0;
  f->lines = 1;
}

static void pill_flow_add(ccord_ui *ui, ccord_renderer *r, pill_flow *f, const char *label, ccord_color fg, bool paint)
{
  if (!label || !label[0]) return;
  float pw = measure_pill_w(ui, r, label);
  if (f->x > f->x0 && f->x + pw > f->right) {
    f->x = f->x0;
    f->y += UIS_H_CHIP + UIS_SP_1;
    f->lines++;
  }
  if (paint) (void)draw_pill(ui, r, f->x, f->y, label, fg);
  f->x += pw + UIS_SP_1 + 2.0f;
}

static float pill_flow_bottom(const pill_flow *f)
{
  return f->y + UIS_H_CHIP;
}

static float pl_btn_sm(const ccord_ui *ui)
{
  return uis_btn_h_for(ui, UIS_H_BTN_SM);
}

static float pl_btn(const ccord_ui *ui)
{
  return uis_btn_h_for(ui, UIS_H_BTN);
}

static uis_btn_size pl_sz_sm(const ccord_ui *ui)
{
  return (ui && ui->mobile_layout) ? UIS_BTN_S_MD : UIS_BTN_S_SM;
}

static float btn_w_sm(ccord_ui *ui, ccord_renderer *r, const char *label, float max_w)
{
  float w = ceilf(uis_label_width(ui, r, label)) + 4.0f;
  if (w < 54.0f) w = 54.0f;
  if (max_w > 0.0f && w > max_w) w = max_w;
  return w;
}

static void draw_btn(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label,
                     uis_btn_kind kind, uis_btn_size sz, bool disabled, const char *ns, uint64_t id)
{
  bool hov = !disabled && !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, rc);
  uis_state st = {.hover = hov, .pressed = !disabled && st_settings_pressed(ui, rc), .disabled = disabled};
  uis_button(ui, r, rc, label, kind, sz, st, ns, id);
}

static void draw_overlay_btn(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label,
                             uis_btn_kind kind, uis_btn_size sz, bool disabled, const char *ns, uint64_t id)
{
  bool hov = !disabled && ui_in_rect(ui->mx, ui->my, rc);
  uis_state st = {.hover = hov, .pressed = !disabled && st_settings_pressed(ui, rc), .disabled = disabled};
  uis_button(ui, r, rc, label, kind, sz, st, ns, id);
}

static void draw_filter_chip(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label,
                             bool selected, const char *ns, uint64_t id)
{
  bool hov = !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, rc);
  uis_state st = {.hover = hov, .pressed = st_settings_pressed(ui, rc), .selected = selected};
  uis_tab(ui, r, rc, label, st, ns, id);
}

static float draw_search_input(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const char *text,
                               const char *placeholder, bool focused, phit_act focus_act, phit_act clear_act,
                               ccord_rect clip, bool use_clip)
{
  uis_tokens tk = uis_tokens_of(ui);
  uis_plate(ui, r, box, tk.r_control, COL(bg_input));
  if (focused) {
    ccord_draw_rect(r, (ccord_rect){box.x, box.y + box.h - 2.0f, box.w, 2.0f}, COL(brand));
  }
  bool has_txt = text && text[0] != '\0';
  float clear_w = has_txt ? 24.0f : 0.0f;
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, has_txt ? COL(text_header) : COL(text_placeholder));
  const char *disp = has_txt ? text : placeholder;
  ccord_vec2 msz = ccord_text_measure(r, disp, strlen(disp), &ts, 0.0f);
  ccord_text_draw_ellipsis(r, disp, strlen(disp), box.x + UIS_SP_3,
                           roundf(box.y + (box.h - msz.y) * 0.5f),
                           fmaxf(box.w - UIS_SP_3 * 2.0f - clear_w, 20.0f), &ts);
  if (use_clip) hit_main_clipped(box, clip, focus_act, 0, "");
  else hit_main(box, focus_act, 0, "");
  if (has_txt) {
    ccord_rect cr = {box.x + box.w - 26.0f, box.y + (box.h - 22.0f) * 0.5f, 22.0f, 22.0f};
    bool chov = !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, cr);
    ccord_color cc = chov ? COL(text_header) : COL(text_muted);
    ccord_text_style xs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, cc);
    ccord_vec2 xz = ccord_text_measure(r, "x", 1, &xs, 0.0f);
    ccord_text_draw(r, "x", 1, roundf(cr.x + (cr.w - xz.x) * 0.5f), roundf(cr.y + (cr.h - xz.y) * 0.5f), &xs, 0.0f);
    if (use_clip) hit_main_clipped(cr, clip, clear_act, 0, "");
    else hit_main(cr, clear_act, 0, "");
  }
  return box.y + box.h;
}

// ---------------------------------------------------------------- security modal opening helpers

static void close_security_modal(void)
{
  s_sec_mode = SEC_NONE;
  s_sec_id[0] = '\0';
  s_sec_old_allowed_hosts[0] = '\0';
  s_sec_has_old_hosts = false;
  s_sec_perms = 0;
  s_sec_old_perms = 0;
  s_sec_scroll = 0.0f;
  s_sec_content_h = 0.0f;
  s_pending_sec_id[0] = '\0';
  s_pending_sec_mode = SEC_NONE;
}

static void open_sec_modal_for_installed_consent(const ccord_plugin_info *p)
{
  if (!p) return;
  close_security_modal();
  s_sec_mode = SEC_CONSENT;
  copy_str(s_sec_id, sizeof s_sec_id, p->id);
  copy_str(s_sec_name, sizeof s_sec_name, p->name);
  s_sec_author[0] = '\0';
  copy_str(s_sec_ver_old, sizeof s_sec_ver_old, "");
  copy_str(s_sec_ver_new, sizeof s_sec_ver_new, p->version);
  copy_str(s_sec_icon_path, sizeof s_sec_icon_path, p->icon_path);
  copy_str(s_sec_allowed_hosts, sizeof s_sec_allowed_hosts, p->allowed_hosts);
  s_sec_changelog[0] = '\0';
  s_sec_warn_sev = p->warning_sev;
  copy_str(s_sec_warn_title, sizeof s_sec_warn_title, p->warning_title);
  copy_str(s_sec_warn_msg, sizeof s_sec_warn_msg, p->warning_message);
  s_sec_tier = p->tier;
  s_sec_perms = p->perms_mask;
  s_sec_old_perms = 0;
}

static void open_sec_modal_for_catalog(const ccord_ui *ui, const ccord_catalog_entry *e, sec_modal_mode mode)
{
  if (!e) return;
  close_security_modal();
  s_sec_mode = mode;
  copy_str(s_sec_id, sizeof s_sec_id, e->id);
  copy_str(s_sec_name, sizeof s_sec_name, e->name);
  copy_str(s_sec_author, sizeof s_sec_author, e->author);
  copy_str(s_sec_ver_old, sizeof s_sec_ver_old, e->installed_version);
  copy_str(s_sec_ver_new, sizeof s_sec_ver_new, e->version);
  copy_str(s_sec_icon_path, sizeof s_sec_icon_path, e->icon_path);
  copy_str(s_sec_allowed_hosts, sizeof s_sec_allowed_hosts, e->allowed_hosts);
  copy_str(s_sec_changelog, sizeof s_sec_changelog, e->changelog);
  s_sec_warn_sev = e->warning_sev;
  copy_str(s_sec_warn_title, sizeof s_sec_warn_title, e->warning_title);
  copy_str(s_sec_warn_msg, sizeof s_sec_warn_msg, e->warning_message);
  s_sec_tier = e->tier;
  s_sec_perms = e->perms_mask;
  const ccord_plugin_info *inst = find_plugin_info(ui ? ui->d.contrib : nullptr, e->id);
  s_sec_old_perms = (mode == SEC_UPDATE && inst) ? inst->consented_perms : 0;
  if (mode == SEC_UPDATE && inst) {
    copy_str(s_sec_old_allowed_hosts, sizeof s_sec_old_allowed_hosts, inst->allowed_hosts);
    s_sec_has_old_hosts =
      (inst->consented_perms & ((uint64_t)CCORD_PERM_NETWORK_HTTP | (uint64_t)CCORD_PERM_NETWORK_WS)) != 0;
  }
  if (!s_sec_icon_path[0] && inst && inst->icon_path) {
    copy_str(s_sec_icon_path, sizeof s_sec_icon_path, inst->icon_path);
  }
}

static void open_sec_modal_for_installed_update(const ccord_ui *ui, const ccord_plugin_info *p)
{
  if (!p) return;
  const ccord_catalog_entry *ce = find_catalog_entry(ui ? ui->d.contrib : nullptr, p->id);
  if (ce && ce->has_detail) {
    open_sec_modal_for_catalog(ui, ce, SEC_UPDATE);
    return;
  }
  close_security_modal();
  s_sec_mode = SEC_UPDATE;
  copy_str(s_sec_id, sizeof s_sec_id, p->id);
  copy_str(s_sec_name, sizeof s_sec_name, p->name);
  s_sec_author[0] = '\0';
  copy_str(s_sec_ver_old, sizeof s_sec_ver_old, p->version);
  copy_str(s_sec_ver_new, sizeof s_sec_ver_new, p->update_version);
  copy_str(s_sec_icon_path, sizeof s_sec_icon_path, p->icon_path);
  copy_str(s_sec_allowed_hosts, sizeof s_sec_allowed_hosts, p->update_allowed_hosts);
  copy_str(s_sec_old_allowed_hosts, sizeof s_sec_old_allowed_hosts, p->allowed_hosts);
  s_sec_has_old_hosts =
    (p->consented_perms & ((uint64_t)CCORD_PERM_NETWORK_HTTP | (uint64_t)CCORD_PERM_NETWORK_WS)) != 0;
  copy_str(s_sec_changelog, sizeof s_sec_changelog, p->update_changelog);
  s_sec_warn_sev = p->warning_sev;
  copy_str(s_sec_warn_title, sizeof s_sec_warn_title, p->warning_title);
  copy_str(s_sec_warn_msg, sizeof s_sec_warn_msg, p->warning_message);
  s_sec_tier = p->tier;
  s_sec_perms = p->update_perms_mask;
  s_sec_old_perms = p->consented_perms;
}

static void request_catalog_install_or_review(ccord_ui *ui, const char *id)
{
  if (!ui || !ui->d.contrib || !id || !id[0]) return;
  const ccord_catalog_entry *e = find_catalog_entry(ui->d.contrib, id);
  if (!e || e->bundled || e->installing) return;
  if (is_running_mobile(ui) && (effective_platforms(e->platforms) & CCORD_CONTRIB_PLAT_ANDROID) == 0) {
    snprintf(s_status_msg, sizeof s_status_msg, "%s is desktop-only and cannot run on Android.", e->name);
    return;
  }
  // Safe 1-click path: Class 1 Safe install OR safe update without review needed.
  if (e->installed && e->update_available && !e->update_needs_review) {
    ccord_contrib_catalog_install(ui->d.contrib, e->id);
    return;
  }
  if (!e->installed && e->api_class <= 1 && e->warning_sev < CCORD_WARN_DANGER) {
    ccord_contrib_catalog_install(ui->d.contrib, e->id);
    return;
  }
  // Class 2/3 install or permission-expanding update -> open unified Protected Security Modal!
  sec_modal_mode mode = (e->installed && e->update_available) ? SEC_UPDATE : SEC_INSTALL;
  if (e->has_detail) {
    open_sec_modal_for_catalog(ui, e, mode);
    return;
  }
  // Slim Discover card: fetch full verified detail first, then auto-pop Protected Security Modal on arrival.
  copy_str(s_pending_sec_id, sizeof s_pending_sec_id, e->id);
  s_pending_sec_mode = mode;
  snprintf(s_status_msg, sizeof s_status_msg, "Loading verified permissions for %s...", e->name);
  ccord_contrib_catalog_fetch_detail(ui->d.contrib, e->id);
}

// ---------------------------------------------------------------- permissions inspector renderer (shared by Detail & Security Modal)

static size_t count_new_perms(uint64_t new_mask, uint64_t old_mask)
{
  uint64_t diff = new_mask & ~old_mask;
  size_t c = 0;
  size_t total = ccord_perm_count();
  for (size_t i = 0; i < total; i++) {
    ccord_perm p = ccord_perm_at(i);
    if ((diff & (uint64_t)p) != 0 && ccord_perm_class_of(p) > CCORD_PERM_CLASS_SAFE) c++;
  }
  return c;
}

static float draw_perm_card_row(ccord_ui *ui, ccord_renderer *r, float x, float y, float w,
                                ccord_perm p, bool is_new_diff, bool dimmed, bool paint)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_perm_class cls = ccord_perm_class_of(p);
  const char *desc = ccord_perm_description(p);
  const char *key = ccord_perm_name(p);
  ccord_color accent = cls == CCORD_PERM_CLASS_SYSTEM ? COL(danger)
                     : cls == CCORD_PERM_CLASS_UNSAFE ? COL(status_idle)
                                                      : COL(status_online);
  ccord_text_style ds = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, dimmed ? COL(text_muted) : COL(text_header));
  ccord_text_style ks = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
  float inner_w = fmaxf(w - 24.0f, 60.0f);
  float badge_w = is_new_diff ? (measure_pill_w(ui, r, "+ NEW") + 8.0f) : 0.0f;
  float dh = fmaxf(18.0f, ccord_text_measure(r, desc, strlen(desc), &ds, fmaxf(inner_w - badge_w, 40.0f)).y);
  float kh = fmaxf(14.0f, ccord_text_measure(r, key, strlen(key), &ks, inner_w).y);
  float rh = dimmed ? (ui_line_h(ui, tk.fs_caption) + 12.0f) : (dh + kh + 18.0f);

  if (paint) {
    ccord_rect row = {x, y, w, rh};
    if (dimmed) {
      uis_plate(ui, r, row, tk.r_control, COL(bg_tertiary));
      char line[256];
      snprintf(line, sizeof line, "✓  %s  (%s)", desc, key);
      ccord_text_draw_ellipsis(r, line, strlen(line), row.x + 10.0f, row.y + 6.0f, inner_w, &ks);
    } else {
      ccord_color bg = is_new_diff ? with_alpha(accent, 26) : COL(bg_tertiary);
      uis_plate(ui, r, row, tk.r_control, bg);
      ccord_draw_rect(r, (ccord_rect){row.x, row.y, 3.0f, row.h}, accent);
      float tx = row.x + 12.0f;
      if (is_new_diff) {
        float pw = draw_pill(ui, r, row.x + row.w - measure_pill_w(ui, r, "+ NEW") - 8.0f, row.y + 8.0f, "+ NEW", accent);
        (void)pw;
      }
      ccord_text_draw(r, desc, strlen(desc), tx, row.y + 8.0f, &ds, fmaxf(inner_w - badge_w, 40.0f));
      ccord_text_draw_ellipsis(r, key, strlen(key), tx, row.y + 8.0f + dh + 2.0f, inner_w, &ks);
    }
  }
  return rh + 6.0f;
}

static bool ui_host_in_csv(const char *csv, const char *tok, size_t tok_len)
{
  if (!csv || !tok || tok_len == 0) return false;
  const char *p = csv;
  while (*p) {
    while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
    const char *s = p;
    while (*p && *p != ',') p++;
    const char *e = p;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t len = (size_t)(e - s);
    if (len == tok_len) {
      bool eq = true;
      for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)tok[i])) {
          eq = false;
          break;
        }
      }
      if (eq) return true;
    }
  }
  return false;
}

static size_t ui_count_csv_tokens(const char *csv)
{
  if (!csv) return 0;
  size_t n = 0;
  const char *p = csv;
  while (*p) {
    while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
    const char *s = p;
    while (*p && *p != ',') p++;
    const char *e = p;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e > s) n++;
  }
  return n;
}

static bool ui_hosts_changed_or_expanded(const char *old_hosts, bool had_old_hosts, const char *new_hosts,
                                         bool *out_became_unrestricted, bool *out_expanded)
{
  if (out_became_unrestricted) *out_became_unrestricted = false;
  if (out_expanded) *out_expanded = false;
  if (!had_old_hosts) return false;
  size_t old_n = ui_count_csv_tokens(old_hosts);
  size_t new_n = ui_count_csv_tokens(new_hosts);
  if (old_n > 0 && new_n == 0) {
    if (out_became_unrestricted) *out_became_unrestricted = true;
    if (out_expanded) *out_expanded = true;
    return true;
  }
  if (old_n > 0 && new_n > 0) {
    const char *p = new_hosts;
    while (*p) {
      while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
      const char *s = p;
      while (*p && *p != ',') p++;
      const char *e = p;
      while (e > s && isspace((unsigned char)e[-1])) e--;
      if (e > s && !ui_host_in_csv(old_hosts, s, (size_t)(e - s))) {
        if (out_expanded) *out_expanded = true;
        return true;
      }
    }
  }
  if (old_n != new_n) return true;
  if (old_n > 0) {
    const char *p = old_hosts;
    while (*p) {
      while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
      const char *s = p;
      while (*p && *p != ',') p++;
      const char *e = p;
      while (e > s && isspace((unsigned char)e[-1])) e--;
      if (e > s && !ui_host_in_csv(new_hosts, s, (size_t)(e - s))) return true;
    }
  }
  return false;
}

static float draw_permissions_section(ccord_ui *ui, ccord_renderer *r, float x, float y, float w,
                                      uint64_t perms_mask, uint64_t old_mask, bool diff_mode,
                                      const char *allowed_hosts, const char *old_allowed_hosts,
                                      bool had_old_hosts, bool paint)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style hdr_s = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  float cy = y;
  size_t total = ccord_perm_count();

  if (perms_mask == 0 || ccord_perm_max_class(perms_mask) == CCORD_PERM_CLASS_SAFE) {
    const char *safe_txt = "No elevated permissions required — uses Class 1 Safe sandbox APIs only.";
    float th = fmaxf(20.0f, ccord_text_measure(r, safe_txt, strlen(safe_txt), &ms, fmaxf(w - 24.0f, 60.0f)).y);
    float bh = th + 18.0f;
    if (paint) {
      uis_plate(ui, r, (ccord_rect){x, cy, w, bh}, tk.r_control, with_alpha(COL(status_online), 24));
      ccord_text_style gs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(status_online));
      ccord_text_draw(r, safe_txt, strlen(safe_txt), x + 12.0f, cy + 9.0f, &gs, fmaxf(w - 24.0f, 60.0f));
    }
    return bh + 8.0f;
  }

  if (diff_mode) {
    uint64_t added = perms_mask & ~old_mask;
    uint64_t kept = perms_mask & old_mask;
    bool has_added = false;
    for (size_t i = 0; i < total; i++) {
      ccord_perm p = ccord_perm_at(i);
      if ((added & (uint64_t)p) != 0 && ccord_perm_class_of(p) > CCORD_PERM_CLASS_SAFE) { has_added = true; break; }
    }
    if (has_added) {
      if (paint) {
        ccord_text_style nh = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(status_idle));
        const char *t = "NEWLY REQUESTED PERMISSIONS";
        ccord_text_draw(r, t, strlen(t), x, cy, &nh, 0.0f);
      }
      cy += 18.0f;
      for (int cls = (int)CCORD_PERM_CLASS_SYSTEM; cls >= (int)CCORD_PERM_CLASS_UNSAFE; cls--) {
        for (size_t i = 0; i < total; i++) {
          ccord_perm p = ccord_perm_at(i);
          if ((added & (uint64_t)p) != 0 && (int)ccord_perm_class_of(p) == cls) {
            cy += draw_perm_card_row(ui, r, x, cy, w, p, true, false, paint);
          }
        }
      }
      cy += 4.0f;
    }
    bool has_kept = false;
    for (size_t i = 0; i < total; i++) {
      ccord_perm p = ccord_perm_at(i);
      if ((kept & (uint64_t)p) != 0 && ccord_perm_class_of(p) > CCORD_PERM_CLASS_SAFE) { has_kept = true; break; }
    }
    if (has_kept) {
      if (paint) {
        const char *t = "ALREADY APPROVED";
        ccord_text_draw(r, t, strlen(t), x, cy, &hdr_s, 0.0f);
      }
      cy += 18.0f;
      for (int cls = (int)CCORD_PERM_CLASS_SYSTEM; cls >= (int)CCORD_PERM_CLASS_UNSAFE; cls--) {
        for (size_t i = 0; i < total; i++) {
          ccord_perm p = ccord_perm_at(i);
          if ((kept & (uint64_t)p) != 0 && (int)ccord_perm_class_of(p) == cls) {
            cy += draw_perm_card_row(ui, r, x, cy, w, p, false, true, paint);
          }
        }
      }
      cy += 4.0f;
    }
  } else {
    for (int cls = (int)CCORD_PERM_CLASS_SYSTEM; cls >= (int)CCORD_PERM_CLASS_UNSAFE; cls--) {
      bool any = false;
      for (size_t i = 0; i < total; i++) {
        ccord_perm p = ccord_perm_at(i);
        if ((perms_mask & (uint64_t)p) != 0 && (int)ccord_perm_class_of(p) == cls) { any = true; break; }
      }
      if (!any) continue;
      if (paint) {
        ccord_color hc = cls == (int)CCORD_PERM_CLASS_SYSTEM ? COL(danger) : COL(status_idle);
        ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, hc);
        const char *t = cls == (int)CCORD_PERM_CLASS_SYSTEM ? "CLASS 3 · SYSTEM & NETWORK ACCESS"
                                                            : "CLASS 2 · C-CORD INTERNAL ACCESS";
        ccord_text_draw(r, t, strlen(t), x, cy, &hs, 0.0f);
      }
      cy += 18.0f;
      for (size_t i = 0; i < total; i++) {
        ccord_perm p = ccord_perm_at(i);
        if ((perms_mask & (uint64_t)p) != 0 && (int)ccord_perm_class_of(p) == cls) {
          cy += draw_perm_card_row(ui, r, x, cy, w, p, false, false, paint);
        }
      }
      cy += 4.0f;
    }
  }

  // Network Host Scope callout whenever HTTP or WebSocket network permissions are requested
  bool uses_net = (perms_mask & ((uint64_t)CCORD_PERM_NETWORK_HTTP | (uint64_t)CCORD_PERM_NETWORK_WS)) != 0;
  if (uses_net) {
    bool restricted = allowed_hosts && allowed_hosts[0] != '\0';
    bool became_unrestricted = false;
    bool hosts_expanded = false;
    bool hosts_changed = diff_mode &&
                         ui_hosts_changed_or_expanded(old_allowed_hosts, had_old_hosts, allowed_hosts,
                                                      &became_unrestricted, &hosts_expanded);
    char scope_txt[1280];
    ccord_color sc = restricted ? COL(brand) : COL(danger);
    uint8_t bg_alpha = 26;
    if (hosts_changed && became_unrestricted) {
      snprintf(scope_txt, sizeof scope_txt,
               "UPDATED NETWORK SCOPE (EXPANDED TO UNRESTRICTED): Previously restricted to [%.240s]. "
               "This update removes allowed_hosts and can contact ANY external host.",
               (old_allowed_hosts && old_allowed_hosts[0]) ? old_allowed_hosts : "specific hosts");
      sc = COL(danger);
      bg_alpha = 38;
    } else if (hosts_changed && hosts_expanded) {
      snprintf(scope_txt, sizeof scope_txt,
               "UPDATED NETWORK SCOPE (EXPANDED): Allowed hosts expanded from [%.240s] to [%.240s]. "
               "Review the newly added external domains carefully.",
               (old_allowed_hosts && old_allowed_hosts[0]) ? old_allowed_hosts : "unrestricted",
               restricted ? allowed_hosts : "unrestricted");
      sc = COL(danger);
      bg_alpha = 34;
    } else if (hosts_changed) {
      snprintf(scope_txt, sizeof scope_txt,
               "UPDATED NETWORK SCOPE: Allowed hosts changed from [%.240s] to [%.240s].",
               (old_allowed_hosts && old_allowed_hosts[0]) ? old_allowed_hosts : "unrestricted (any host)",
               restricted ? allowed_hosts : "unrestricted (any host)");
      sc = restricted ? COL(brand) : COL(danger);
      bg_alpha = 28;
    } else if (restricted) {
      snprintf(scope_txt, sizeof scope_txt, "Network scope pinned to allowed hosts: %.512s", allowed_hosts);
    } else {
      snprintf(scope_txt, sizeof scope_txt,
               "Unrestricted network access: no allowed_hosts allow-list declared; this plugin may contact any external host.");
    }
    ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, sc);
    float tw = fmaxf(w - 24.0f, 60.0f);
    float sh = fmaxf(16.0f, ccord_text_measure(r, scope_txt, strlen(scope_txt), &ss, tw).y);
    float bh = sh + 16.0f;
    if (paint) {
      uis_plate(ui, r, (ccord_rect){x, cy, w, bh}, tk.r_control, with_alpha(sc, bg_alpha));
      ccord_draw_rect(r, (ccord_rect){x, cy, 3.0f, bh}, sc);
      ccord_text_draw(r, scope_txt, strlen(scope_txt), x + 12.0f, cy + 8.0f, &ss, tw);
    }
    cy += bh + 8.0f;
  }

  // Class 3 Broad Host-User Access warning for system.exec, fs.write, or network.local
  bool has_sys_exec = (perms_mask & (uint64_t)CCORD_PERM_SYSTEM_EXEC) != 0;
  bool has_fs_write = (perms_mask & (uint64_t)CCORD_PERM_FS_WRITE) != 0;
  bool has_net_local = (perms_mask & (uint64_t)CCORD_PERM_NETWORK_LOCAL) != 0;
  if (has_sys_exec || has_fs_write || has_net_local) {
    char host_txt[640];
    snprintf(host_txt, sizeof host_txt,
             "HIGH RISK · BROAD HOST ACCESS: Requests%s%s%s. "
             "These Class 3 permissions grant broad host-user access (external processes, file modification, "
             "or local sockets) and can reach files or credentials outside C-Cord's sandbox. "
             "Only approve plugins from authors you fully trust.",
             has_sys_exec ? " system.exec (run external processes)" : "",
             has_fs_write ? (has_sys_exec ? ", fs.write (modify local files)" : " fs.write (modify local files)") : "",
             has_net_local ? ((has_sys_exec || has_fs_write) ? ", network.local (connect to local sockets/services)"
                                                             : " network.local (connect to local sockets/services)") : "");
    ccord_color sc = COL(danger);
    ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, sc);
    float tw = fmaxf(w - 24.0f, 60.0f);
    float sh = fmaxf(16.0f, ccord_text_measure(r, host_txt, strlen(host_txt), &ss, tw).y);
    float bh = sh + 16.0f;
    if (paint) {
      uis_plate(ui, r, (ccord_rect){x, cy, w, bh}, tk.r_control, with_alpha(sc, 38));
      ccord_draw_rect(r, (ccord_rect){x, cy, 3.0f, bh}, sc);
      ccord_text_draw(r, host_txt, strlen(host_txt), x + 12.0f, cy + 8.0f, &ss, tw);
    }
    cy += bh + 8.0f;
  }

  // Class 3 Account Privacy & Exfiltration callouts whenever account.* permissions are declared
  bool has_acct_read = (perms_mask & (uint64_t)CCORD_PERM_ACCOUNT_READ) != 0;
  bool has_acct_prof = (perms_mask & (uint64_t)CCORD_PERM_PROFILE_WRITE) != 0;
  bool has_acct_write = (perms_mask & (uint64_t)CCORD_PERM_ACCOUNT_WRITE) != 0;
  if (has_acct_read || has_acct_prof || has_acct_write) {
    char acct_txt[480];
    snprintf(acct_txt, sizeof acct_txt,
             "Class 3 Account Access:%s%s%s Passwords, 2FA secrets, backup codes, and Discord tokens are never exposed to plugins.",
             has_acct_read ? " reads private account details (email, phone, 2FA status, standing, devices);" : "",
             has_acct_prof ? " modifies your Discord profile;" : "",
             has_acct_write ? " can prompt you for account changes via native confirmation;" : "");
    ccord_color sc = COL(danger);
    ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, sc);
    float tw = fmaxf(w - 24.0f, 60.0f);
    float sh = fmaxf(16.0f, ccord_text_measure(r, acct_txt, strlen(acct_txt), &ss, tw).y);
    float bh = sh + 16.0f;
    if (paint) {
      uis_plate(ui, r, (ccord_rect){x, cy, w, bh}, tk.r_control, with_alpha(sc, 26));
      ccord_draw_rect(r, (ccord_rect){x, cy, 3.0f, bh}, sc);
      ccord_text_draw(r, acct_txt, strlen(acct_txt), x + 12.0f, cy + 8.0f, &ss, tw);
    }
    cy += bh + 8.0f;
  }

  bool uses_any_net = (perms_mask & ((uint64_t)CCORD_PERM_NETWORK_HTTP | (uint64_t)CCORD_PERM_NETWORK_WS |
                                     (uint64_t)CCORD_PERM_NETWORK_LOCAL)) != 0;
  if (has_acct_read && uses_any_net) {
    const char *combo_txt =
        "HIGH RISK COMBINATION: This plugin requests both account.read and network access. "
        "It could transmit your private account metadata (email, phone, standing, devices) to an external server.";
    ccord_color sc = COL(danger);
    ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, sc);
    float tw = fmaxf(w - 24.0f, 60.0f);
    float sh = fmaxf(16.0f, ccord_text_measure(r, combo_txt, strlen(combo_txt), &ss, tw).y);
    float bh = sh + 16.0f;
    if (paint) {
      uis_plate(ui, r, (ccord_rect){x, cy, w, bh}, tk.r_control, with_alpha(sc, 38));
      ccord_draw_rect(r, (ccord_rect){x, cy, 3.0f, bh}, sc);
      ccord_text_draw(r, combo_txt, strlen(combo_txt), x + 12.0f, cy + 8.0f, &ss, tw);
    }
    cy += bh + 8.0f;
  }

  return cy - y;
}

// ---------------------------------------------------------------- public hooks

bool ui_settings_security_modal_open(const ccord_ui *ui)
{
  if (s_sec_mode != SEC_NONE) return true;
  if (ui && (ui->login_screen || ui->account_mfa_required)) return true;
  if (ui && ui->d.contrib && ccord_contrib_active_account_hitl(ui->d.contrib) != nullptr) return true;
  if (st_account_security_active(ui)) return true;
  return false;
}

bool st_plugins_has_overlay(void)
{
  return s_uninst_id[0] != '\0' || s_sec_mode != SEC_NONE || s_overflow_id[0] != '\0' || s_sort_open;
}

float st_plugin_body_y(const sgeom *g)
{
  if (s_body_top_cached > g->content.y + 80.0f) return s_body_top_cached;
  return g->content.y + (g->content.w < 490.0f ? 156.0f : 124.0f);
}

bool ui_settings_plugins_wants_text(const ccord_ui *ui)
{
  if (!ui || !ui->settings_open || ui->settings_page != SP_PLUGINS) return false;
  if (st_plugins_has_overlay() || s_detail_id[0] || s_log_id[0]) return false;
  if (s_subtab == 0 && s_inst_search_focused) return true;
  if (s_subtab == 1 && s_disc_search_focused) return true;
  return false;
}

bool ui_settings_plugins_dismiss_overlay(ccord_ui *ui)
{
  if (!ui || ui->settings_page != SP_PLUGINS) return false;
  if (s_uninst_id[0]) { s_uninst_id[0] = '\0'; ui->dirty = true; return true; }
  if (s_sort_open) { s_sort_open = false; ui->dirty = true; return true; }
  if (s_overflow_id[0]) { s_overflow_id[0] = '\0'; ui->dirty = true; return true; }
  if (s_sec_mode != SEC_NONE) { close_security_modal(); ui->dirty = true; return true; }
  if (s_inst_search_focused || s_disc_search_focused) {
    s_inst_search_focused = false;
    s_disc_search_focused = false;
    if (ui->touch_mode) ui->touch_text_focused = false;
    ui->dirty = true;
    return true;
  }
  return false;
}

bool st_plugins_handle_back(ccord_ui *ui)
{
  if (!ui || ui->settings_page != SP_PLUGINS) return false;
  if (ui_settings_plugins_dismiss_overlay(ui)) return true;
  if (s_log_id[0]) { s_log_id[0] = '\0'; s_log_scroll = 0.0f; ui->dirty = true; return true; }
  if (s_detail_id[0]) { s_detail_id[0] = '\0'; st_s_page_scroll = 0.0f; ui->dirty = true; return true; }
  return false;
}

bool ui_open_plugin_install_prompt(ccord_ui *ui, const char *plugin_id)
{
  if (!ui || !plugin_id || !plugin_id[0]) return false;
  size_t len = strlen(plugin_id);
  if (len == 0 || len > 64) return false;
  if (!((plugin_id[0] >= 'a' && plugin_id[0] <= 'z') || (plugin_id[0] >= '0' && plugin_id[0] <= '9'))) return false;
  for (size_t i = 0; i < len; i++) {
    char c = plugin_id[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
    if (c == '.' && i + 1 < len && plugin_id[i + 1] == '.') return false;
  }
  st_settings_init_defaults(ui);
  ui_open_settings(ui, true);
  ui->settings_page = SP_PLUGINS;
  ui->settings_category = st_k_pages[SP_PLUGINS].category;
  if (ui->mobile_layout) ui->mobile_settings_detail = true;
  s_subtab = 1;
  s_log_id[0] = '\0';
  s_overflow_id[0] = '\0';
  s_uninst_id[0] = '\0';
  s_sort_open = false;
  close_security_modal();
  copy_str(s_detail_id, sizeof s_detail_id, plugin_id);
  st_s_page_scroll = 0.0f;
  if (ui->d.contrib) {
    ccord_contrib_catalog_fetch_detail(ui->d.contrib, plugin_id);
  }
  ui_notice(ui, "Review plugin before installing: %.48s", plugin_id);
  ui->dirty = true;
  return true;
}

// ---------------------------------------------------------------- Sub-view renderers

static void draw_debug_log_view(ccord_ui *ui, ccord_renderer *r, float x, float y0, float w, float body_bottom)
{
  const ccord_plugin_info *p = find_plugin_info(ui->d.contrib, s_log_id);
  if (!p) { s_log_id[0] = '\0'; return; }

  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  st_s_plugins_content_h = 0.0f;
  st_s_page_scroll = 0.0f;

  float ly = y0;
  float bw_back = btn_w_sm(ui, r, "Back", 96.0f);
  float bw_clr = btn_w_sm(ui, r, "Clear", 88.0f);
  float bw_rel = btn_w_sm(ui, r, "Reload", 96.0f);
  float btns_w = bw_back + bw_clr + bw_rel + 12.0f;

  draw_plugin_icon(ui, r, (ccord_rect){x, ly, 28.0f, 28.0f}, p->id, p->name, p->icon_path);
  char hdr[128];
  snprintf(hdr, sizeof hdr, "Debug Log · %s", p->name);
  float bh = pl_btn_sm(ui);
  uis_btn_size bsz = pl_sz_sm(ui);
  if (w >= btns_w + 180.0f) {
    ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x + 36.0f, ly + 4.0f, w - btns_w - 44.0f, &ns);
    float rx = x + w - bw_back;
    ccord_rect rb = {rx, ly, bw_back, bh};
    rx -= bw_clr + 6.0f;
    ccord_rect rc = {rx, ly, bw_clr, bh};
    rx -= bw_rel + 6.0f;
    ccord_rect rr = {rx, ly, bw_rel, bh};
    draw_btn(ui, r, rr, "Reload", UIS_BTN_SECONDARY, bsz, false, "plog", 1);
    draw_btn(ui, r, rc, "Clear", UIS_BTN_SECONDARY, bsz, false, "plog", 2);
    draw_btn(ui, r, rb, "Back", UIS_BTN_PRIMARY, bsz, false, "plog", 3);
    hit_main(rr, ACT_LOG_RELOAD, 0, p->id);
    hit_main(rc, ACT_LOG_CLEAR, 0, p->id);
    hit_main(rb, ACT_LOG_BACK, 0, "");
    ly += bh + 8.0f;
  } else {
    ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x + 36.0f, ly + 4.0f, w - 40.0f, &ns);
    ly += 34.0f;
    ccord_rect rr = {x, ly, bw_rel, bh};
    ccord_rect rc = {x + bw_rel + 6.0f, ly, bw_clr, bh};
    ccord_rect rb = {x + bw_rel + bw_clr + 12.0f, ly, bw_back, bh};
    draw_btn(ui, r, rr, "Reload", UIS_BTN_SECONDARY, bsz, false, "plog", 1);
    draw_btn(ui, r, rc, "Clear", UIS_BTN_SECONDARY, bsz, false, "plog", 2);
    draw_btn(ui, r, rb, "Back", UIS_BTN_PRIMARY, bsz, false, "plog", 3);
    hit_main(rr, ACT_LOG_RELOAD, 0, p->id);
    hit_main(rc, ACT_LOG_CLEAR, 0, p->id);
    hit_main(rb, ACT_LOG_BACK, 0, "");
    ly += bh + 8.0f;
  }

  // Count per severity and draw level filter chips
  size_t lc = ccord_contrib_plugin_log_count(ui->d.contrib, p->id);
  size_t c_info = 0, c_warn = 0, c_err = 0;
  for (size_t i = 0; i < lc; i++) {
    const ccord_plugin_log_entry *e = ccord_contrib_plugin_log_at(ui->d.contrib, p->id, i);
    if (!e) continue;
    if (e->level >= 3) c_err++;
    else if (e->level == 2) c_warn++;
    else c_info++;
  }
  char flbl[4][32];
  snprintf(flbl[0], sizeof flbl[0], "All (%zu)", lc);
  snprintf(flbl[1], sizeof flbl[1], "Info (%zu)", c_info);
  snprintf(flbl[2], sizeof flbl[2], "Warn (%zu)", c_warn);
  snprintf(flbl[3], sizeof flbl[3], "Error (%zu)", c_err);
  float fx = x;
  for (int k = 0; k < 4; k++) {
    float cw = btn_w_sm(ui, r, flbl[k], w);
    if (fx > x && fx + cw > x + w) { fx = x; ly += bh + 4.0f; }
    ccord_rect cr = {fx, ly, cw, bh};
    draw_filter_chip(ui, r, cr, flbl[k], s_log_level_filter == k, "plogf", (uint64_t)k);
    hit_main(cr, ACT_LOG_LEVEL, k, "");
    fx += cw + 6.0f;
  }
  ly += bh + 8.0f;

  ccord_rect box = {x, ly, w, fmaxf(body_bottom - ly, 96.0f)};
  s_log_box_rect = box;
  uis_card(ui, r, box);

  // Count matching lines & compute scroll range
  size_t match_n = 0;
  for (size_t i = 0; i < lc; i++) {
    const ccord_plugin_log_entry *e = ccord_contrib_plugin_log_at(ui->d.contrib, p->id, i);
    if (!e) continue;
    if (s_log_level_filter == 1 && e->level > 1) continue;
    if (s_log_level_filter == 2 && e->level != 2) continue;
    if (s_log_level_filter == 3 && e->level < 3) continue;
    match_n++;
  }
  s_log_content_h = (float)match_n * 22.0f + 16.0f;
  float max_ls = s_log_content_h > box.h ? (s_log_content_h - box.h) : 0.0f;
  if (s_log_scroll < 0.0f) s_log_scroll = 0.0f;
  if (s_log_scroll > max_ls) s_log_scroll = max_ls;

  if (match_n == 0) {
    s_log_sb_track = (ccord_rect){0};
    const char *empty = lc == 0 ? "No debug log entries recorded for this plugin yet."
                                : "No log entries match the selected severity filter.";
    ccord_text_draw(r, empty, strlen(empty), box.x + 14.0f, box.y + 14.0f, &ms, box.w - 28.0f);
    return;
  }

  float log_right_pad = max_ls > 0.0f ? 14.0f : 4.0f;
  ccord_push_clip(r, (ccord_rect){box.x + 4.0f, box.y + 4.0f, box.w - 4.0f - log_right_pad, box.h - 8.0f});
  float row_y = box.y + 8.0f - s_log_scroll;
  for (size_t i = 0; i < lc; i++) {
    const ccord_plugin_log_entry *e = ccord_contrib_plugin_log_at(ui->d.contrib, p->id, i);
    if (!e) continue;
    if (s_log_level_filter == 1 && e->level > 1) continue;
    if (s_log_level_filter == 2 && e->level != 2) continue;
    if (s_log_level_filter == 3 && e->level < 3) continue;
    if (row_y + 22.0f >= box.y && row_y <= box.y + box.h) {
      const char *tag = e->level >= 3 ? "ERR" : e->level == 2 ? "WARN" : e->level == 0 ? "DBG" : "INFO";
      ccord_color tc = e->level >= 3 ? COL(danger) : e->level == 2 ? COL(status_idle)
                     : e->level == 0 ? COL(text_muted) : COL(brand);
      float pw = draw_pill(ui, r, box.x + 10.0f, row_y, tag, tc);
      ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control,
                                     e->level >= 3 ? COL(danger) : COL(text_normal));
      const char *msg = e->message ? e->message : "";
      ccord_text_draw_ellipsis(r, msg, strlen(msg), box.x + 10.0f + pw + 8.0f, row_y + 1.0f,
                               fmaxf(box.w - 24.0f - log_right_pad - pw, 40.0f), &ls);
    }
    row_y += 22.0f;
  }
  ccord_pop_clip(r);
  draw_simple_scrollbar(ui, r, (ccord_rect){box.x + box.w - 10.0f, box.y + 6.0f, 6.0f, box.h - 12.0f},
                        s_log_scroll, box.h, s_log_content_h, &s_log_sb_track);
}

static void draw_detail_view(ccord_ui *ui, ccord_renderer *r, float x, float y, float w, ccord_rect body_clip)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
  ccord_text_style ovs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
  const ccord_catalog_entry *e = find_catalog_entry(ui->d.contrib, s_detail_id);
  const ccord_plugin_info *inst = find_plugin_info(ui->d.contrib, s_detail_id);
  bool have_cat = e && e->has_detail;

  ccord_push_clip(r, body_clip);
  float start_y = y;

  float bh_sm = pl_btn_sm(ui);
  float bh = pl_btn(ui);
  uis_btn_size bsz_sm = pl_sz_sm(ui);
  float bw_back = btn_w_sm(ui, r, "< Back", 104.0f);
  ccord_rect r_back = {x, y, bw_back, bh_sm};
  draw_btn(ui, r, r_back, "< Back", UIS_BTN_SECONDARY, bsz_sm, false, "pdet", 1);
  hit_main_clipped(r_back, body_clip, ACT_DETAIL_BACK, 0, "");
  y += bh_sm + 10.0f;

  if (!have_cat && !inst) {
    ccord_rect box = {x, y, w, ui->mobile_layout ? 142.0f : 136.0f};
    uis_card(ui, r, box);
    bool busy = cst && cst->detail_loading;
    char hdr[140];
    if (busy) snprintf(hdr, sizeof hdr, "Loading %s from catalog...", s_detail_id);
    else snprintf(hdr, sizeof hdr, "Plugin not found in catalog: %s", s_detail_id);
    ccord_text_draw_ellipsis(r, hdr, strlen(hdr), box.x + 16.0f, box.y + 18.0f, box.w - 32.0f, &ns);
    const char *sub = busy ? "Fetching verified metadata, permissions, and SHA-256 digest..."
                           : (cst && cst->detail_error[0]) ? cst->detail_error
                                                           : "Verify the plugin ID on plugins.c-cord.org or click Retry.";
    ccord_text_draw(r, sub, strlen(sub), box.x + 16.0f, box.y + 44.0f, &ms, box.w - 32.0f);
    ccord_rect r_ret = {box.x + 16.0f, box.y + box.h - bh - 10.0f, btn_w_sm(ui, r, "Retry", 120.0f), bh};
    draw_btn(ui, r, r_ret, "Retry", UIS_BTN_PRIMARY, UIS_BTN_S_MD, busy, "pdet", 2);
    if (!busy) hit_main_clipped(r_ret, body_clip, ACT_DETAIL_RETRY, 0, s_detail_id);
    st_s_plugins_content_h = (box.y + box.h + 12.0f) - start_y;
    ccord_pop_clip(r);
    return;
  }

  // Merge catalog & installed info so local-only plugins work offline too!
  const char *d_id = have_cat ? e->id : inst->id;
  const char *d_name = have_cat ? e->name : inst->name;
  const char *d_ver = have_cat ? e->version : inst->version;
  const char *d_desc = (have_cat && e->description && e->description[0]) ? e->description
                     : (inst && inst->description && inst->description[0]) ? inst->description
                     : "No description provided.";
  const char *d_icon = (inst && inst->icon_path && inst->icon_path[0]) ? inst->icon_path
                     : (have_cat ? e->icon_path : "");
  const char *d_hosts = (have_cat && e->allowed_hosts && e->allowed_hosts[0]) ? e->allowed_hosts
                      : (inst ? inst->allowed_hosts : "");
  uint64_t d_perms = have_cat ? e->perms_mask : inst->perms_mask;
  uint8_t d_cls = have_cat ? e->api_class : (uint8_t)(inst->risk_class + 1);
  ccord_plugin_tier d_tier = have_cat ? e->tier : inst->tier;
  bool d_bundled = (have_cat && e->bundled) || (inst && inst->is_bundled);
  uint32_t d_plats = have_cat ? e->platforms : inst->platforms;
  ccord_catalog_warn_sev d_wsev = have_cat ? e->warning_sev : inst->warning_sev;
  const char *d_wtitle = have_cat ? e->warning_title : inst->warning_title;
  const char *d_wmsg = have_cat ? e->warning_message : inst->warning_message;

  float inner_w = fmaxf(w - 32.0f, 80.0f);
  bool mob = is_running_mobile(ui);
  bool plat_warn = false;
  const char *plat_lbl = platform_badge_label(d_plats, mob, &plat_warn);

  // Pass 1: measure total card height accurately so ZERO overflow or clipping occurs
  float icon_sz = w < 440.0f ? 40.0f : 48.0f;
  pill_flow pf;
  pill_flow_init(&pf, x + 16.0f + icon_sz + 12.0f, y + 40.0f, fmaxf(inner_w - icon_sz - 12.0f, 80.0f));
  pill_flow_add(ui, r, &pf, tier_label(d_tier, d_bundled), tier_color(ui, d_tier, d_bundled), false);
  pill_flow_add(ui, r, &pf, api_class_label(d_cls), api_class_color(ui, d_cls), false);
  pill_flow_add(ui, r, &pf, plat_lbl, plat_warn ? COL(status_idle) : COL(text_muted), false);
  float hdr_bottom = fmaxf(y + 16.0f + icon_sz, pill_flow_bottom(&pf));

  float cur_h = (hdr_bottom - y) + 12.0f;
  float meta_h = 18.0f;
  cur_h += meta_h + 10.0f;
  float desc_h = fmaxf(18.0f, ccord_text_measure(r, d_desc, strlen(d_desc), &ms, inner_w).y);
  cur_h += desc_h + 14.0f;

  float warn_h = 0.0f;
  char wfull[640] = "";
  if (d_wsev != CCORD_WARN_NONE && d_wtitle && d_wtitle[0]) {
    snprintf(wfull, sizeof wfull, "Moderator Warning: %s%s%s", d_wtitle,
             (d_wmsg && d_wmsg[0]) ? " — " : "", (d_wmsg && d_wmsg[0]) ? d_wmsg : "");
    warn_h = fmaxf(18.0f, ccord_text_measure(r, wfull, strlen(wfull), &ms, fmaxf(inner_w - 24.0f, 60.0f)).y) + 20.0f;
    cur_h += warn_h + 10.0f;
  }

  float perms_h = draw_permissions_section(ui, r, x + 16.0f, 0.0f, inner_w, d_perms, 0, false,
                                           d_hosts, nullptr, false, false);
  cur_h += 20.0f + perms_h + 8.0f;

  float cl_h = 0.0f;
  if (have_cat && e->changelog && e->changelog[0]) {
    cl_h = fmaxf(18.0f, ccord_text_measure(r, e->changelog, strlen(e->changelog), &ms, inner_w).y);
    cur_h += 20.0f + cl_h + 12.0f;
  }
  if (have_cat && e->sha256 && e->sha256[0]) {
    cur_h += 28.0f + 10.0f;
  }
  cur_h += bh + 22.0f;

  // Pass 2: draw detail card
  ccord_rect box = {x, y, w, cur_h};
  uis_card(ui, r, box);

  draw_plugin_icon(ui, r, (ccord_rect){box.x + 16.0f, box.y + 16.0f, icon_sz, icon_sz}, d_id, d_name, d_icon);
  float tx = box.x + 16.0f + icon_sz + 12.0f;
  float tw = fmaxf(box.x + box.w - 16.0f - tx, 60.0f);
  char title_ver[128];
  snprintf(title_ver, sizeof title_ver, "%s  v%s", d_name, d_ver);
  ccord_text_draw_ellipsis(r, title_ver, strlen(title_ver), tx, box.y + 15.0f, tw, &ns);

  pill_flow_init(&pf, tx, box.y + 40.0f, tw);
  pill_flow_add(ui, r, &pf, tier_label(d_tier, d_bundled), tier_color(ui, d_tier, d_bundled), true);
  pill_flow_add(ui, r, &pf, api_class_label(d_cls), api_class_color(ui, d_cls), true);
  pill_flow_add(ui, r, &pf, plat_lbl, plat_warn ? COL(status_idle) : COL(text_muted), true);

  float cy = fmaxf(box.y + 16.0f + icon_sz, pill_flow_bottom(&pf)) + 12.0f;
  char meta[360];
  if (have_cat) {
    char dls[32], szs[32];
    format_downloads(e->downloads, dls, sizeof dls);
    format_bytes(e->size_bytes, szs, sizeof szs);
    snprintf(meta, sizeof meta, "by @%s · %s · %s · %s · %s",
             e->author[0] ? e->author : "unknown", d_id,
             (e->license && e->license[0]) ? e->license : "Unspecified", dls, szs);
  } else {
    snprintf(meta, sizeof meta, "ID: %s · Installed locally", d_id);
  }
  ccord_text_draw_ellipsis(r, meta, strlen(meta), box.x + 16.0f, cy, inner_w, &cs);
  cy += meta_h + 10.0f;

  ccord_text_draw(r, d_desc, strlen(d_desc), box.x + 16.0f, cy, &ms, inner_w);
  cy += desc_h + 14.0f;

  if (warn_h > 0.0f) {
    ccord_color wc = d_wsev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
    uis_plate(ui, r, (ccord_rect){box.x + 16.0f, cy, inner_w, warn_h}, tk.r_control, with_alpha(wc, 28));
    ccord_draw_rect(r, (ccord_rect){box.x + 16.0f, cy, 3.0f, warn_h}, wc);
    ccord_text_style ws = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, wc);
    ccord_text_draw(r, wfull, strlen(wfull), box.x + 28.0f, cy + 10.0f, &ws, fmaxf(inner_w - 24.0f, 60.0f));
    cy += warn_h + 10.0f;
  }

  ccord_text_draw(r, "PERMISSIONS & CAPABILITIES", 26, box.x + 16.0f, cy, &ovs, 0.0f);
  cy += 20.0f;
  cy += draw_permissions_section(ui, r, box.x + 16.0f, cy, inner_w, d_perms, 0, false,
                                 d_hosts, nullptr, false, true);
  cy += 8.0f;

  if (cl_h > 0.0f) {
    ccord_text_draw(r, "RELEASE NOTES", 13, box.x + 16.0f, cy, &ovs, 0.0f);
    cy += 20.0f;
    ccord_text_draw(r, e->changelog, strlen(e->changelog), box.x + 16.0f, cy, &ms, inner_w);
    cy += cl_h + 12.0f;
  }

  if (have_cat && e->sha256 && e->sha256[0]) {
    ccord_rect sbox = {box.x + 16.0f, cy, inner_w, 28.0f};
    uis_plate(ui, r, sbox, tk.r_control, COL(bg_tertiary));
    char sha[128];
    snprintf(sha, sizeof sha, "SHA-256: %s", e->sha256);
    ccord_text_draw_ellipsis(r, sha, strlen(sha), sbox.x + 10.0f, sbox.y + 7.0f, sbox.w - 20.0f, &cs);
    cy += 38.0f;
  }

  // Action footer buttons (dynamically measured, zero cutoff)
  float fx = box.x + 16.0f;
  float fy = box.y + box.h - bh - 14.0f;
  if (have_cat && !d_bundled) {
    bool blocked = plat_warn;
    if (e->installing) {
      float bw = btn_w_sm(ui, r, "Verifying SHA-256...", inner_w);
      draw_btn(ui, r, (ccord_rect){fx, fy, bw, bh}, "Verifying SHA-256...", UIS_BTN_PRIMARY, UIS_BTN_S_MD, true, "pdet", 10);
      fx += bw + 8.0f;
    } else if (!e->installed) {
      const char *lbl = blocked ? "Desktop Only" : (e->api_class >= 2 ? "Review & Install" : "Install");
      float bw = btn_w_sm(ui, r, lbl, inner_w);
      ccord_rect rb = {fx, fy, bw, bh};
      draw_btn(ui, r, rb, lbl, UIS_BTN_PRIMARY, UIS_BTN_S_MD, blocked, "pdet", 11);
      if (!blocked) hit_main_clipped(rb, body_clip, ACT_CAT_INSTALL, 0, e->id);
      fx += bw + 8.0f;
    } else if (e->update_available) {
      const char *lbl = e->update_needs_review ? "Review & Update" : "Update Now";
      float bw = btn_w_sm(ui, r, lbl, inner_w);
      ccord_rect rb = {fx, fy, bw, bh};
      draw_btn(ui, r, rb, lbl, UIS_BTN_PRIMARY, UIS_BTN_S_MD, false, "pdet", 12);
      hit_main_clipped(rb, body_clip, ACT_CAT_INSTALL, 0, e->id);
      fx += bw + 8.0f;
    }
  }
  if (inst && ui->d.contrib && ccord_contrib_plugin_settings(ui->d.contrib, inst->id)) {
    float bw = btn_w_sm(ui, r, "Configure", inner_w);
    ccord_rect rb = {fx, fy, bw, bh};
    draw_btn(ui, r, rb, "Configure", UIS_BTN_SECONDARY, UIS_BTN_S_MD, false, "pdet", 13);
    hit_main_clipped(rb, body_clip, ACT_CARD_CONFIGURE, 0, inst->id);
    fx += bw + 8.0f;
  }
  if (inst && !d_bundled) {
    float bw = btn_w_sm(ui, r, "Uninstall...", inner_w);
    ccord_rect rb = {fx, fy, bw, bh};
    draw_btn(ui, r, rb, "Uninstall...", UIS_BTN_DANGER_SOFT, UIS_BTN_S_MD, false, "pdet", 14);
    hit_main_clipped(rb, body_clip, ACT_CARD_UNINSTALL_ASK, 0, inst->id);
  }

  st_s_plugins_content_h = (box.y + box.h + 14.0f) - start_y;
  ccord_pop_clip(r);
}

// ---------------------------------------------------------------- Tab 0: Installed

static bool plugin_needs_attention(const ccord_plugin_info *p)
{
  return p->revoked || p->state == CCORD_PLUGIN_NEEDS_CONSENT || p->state == CCORD_PLUGIN_FAILED ||
         (p->warning_sev != CCORD_WARN_NONE && p->warning_title && p->warning_title[0]);
}

static float layout_or_draw_installed_card(ccord_ui *ui, ccord_renderer *r, const ccord_plugin_info *p,
                                           size_t idx, float x, float y, float w, ccord_rect body_clip, bool paint)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));

  bool narrow = w < 440.0f;
  float pad = narrow ? 12.0f : 14.0f;
  float icon_sz = narrow ? 36.0f : 40.0f;
  float inner_w = fmaxf(w - pad * 2.0f, 80.0f);
  bool has_cfg = ui->d.contrib && ccord_contrib_plugin_settings(ui->d.contrib, p->id) != nullptr;
  bool dev_mode = is_developer_mode(ui);
  bool has_err = !p->revoked && p->error && p->error[0];
  bool has_warn = !p->revoked && p->warning_sev != CCORD_WARN_NONE && p->warning_title && p->warning_title[0];
  bool has_upd = !p->revoked && p->update_version && p->update_version[0];
  bool needs_consent = !p->revoked && p->state == CCORD_PLUGIN_NEEDS_CONSENT;
  bool mob = is_running_mobile(ui);
  bool plat_warn = false;
  const char *plat_lbl = platform_badge_label(p->platforms, mob, &plat_warn);

  // Measure header right controls: [Toggle or Review] + [...]
  float bh_sm = pl_btn_sm(ui);
  float bh = pl_btn(ui);
  uis_btn_size bsz_sm = pl_sz_sm(ui);
  float ov_w = ui->mobile_layout ? 40.0f : 32.0f;
  float right_ctrl_w = ov_w;
  if (!p->revoked && !needs_consent) right_ctrl_w += UIS_TOGGLE_W + 8.0f;

  // Pills flow beneath title
  float pills_x = x + pad + icon_sz + 10.0f;
  float pills_w = fmaxf(x + w - pad - pills_x, 80.0f);
  float pills_off = ui->mobile_layout ? 32.0f : 22.0f;
  pill_flow pf;
  pill_flow_init(&pf, pills_x, y + pad + pills_off, pills_w);
  pill_flow_add(ui, r, &pf, tier_label(p->tier, p->is_bundled), tier_color(ui, p->tier, p->is_bundled), false);
  uint8_t api_cls = (uint8_t)(p->risk_class + 1);
  pill_flow_add(ui, r, &pf, api_class_label(api_cls), api_class_color(ui, api_cls), false);
  ccord_color sc = p->revoked ? COL(danger)
                 : p->state == CCORD_PLUGIN_READY ? COL(status_online)
                 : p->state == CCORD_PLUGIN_FAILED ? COL(danger)
                 : (p->state == CCORD_PLUGIN_STARTING || p->state == CCORD_PLUGIN_NEEDS_CONSENT) ? COL(status_idle)
                 : COL(text_muted);
  const char *sn = p->revoked ? "Revoked" : (p->state == CCORD_PLUGIN_READY ? "Active" : st_state_name(p->state));
  pill_flow_add(ui, r, &pf, sn, sc, false);
  if (!narrow) pill_flow_add(ui, r, &pf, plat_lbl, plat_warn ? COL(status_idle) : COL(text_muted), false);

  float cy = fmaxf(y + pad + icon_sz, pill_flow_bottom(&pf)) + 8.0f;

  // Description (single or 2-line wrapped)
  const char *desc = (p->description && p->description[0]) ? p->description : "No description provided.";
  float line_h = ui_line_h(ui, tk.fs_control);
  float desc_h = fminf(line_h * 2.0f, fmaxf(line_h, ccord_text_measure(r, desc, strlen(desc), &ms, inner_w).y));
  cy += desc_h + 10.0f;

  // Contextual banners
  float rev_h = 0.0f, consent_h = 0.0f, warn_h = 0.0f, err_h = 0.0f, upd_h = 0.0f;
  char rev_txt[300] = "";
  if (p->revoked) {
    snprintf(rev_txt, sizeof rev_txt, "Quarantined: %s", (p->error && p->error[0]) ? p->error : "Revoked by C-Cord moderators.");
    float btnw = btn_w_sm(ui, r, "Uninstall", 104.0f);
    float tw = narrow ? (inner_w - 20.0f) : fmaxf(inner_w - btnw - 30.0f, 80.0f);
    float th = fmaxf(line_h, ccord_text_measure(r, rev_txt, strlen(rev_txt), &ms, tw).y);
    rev_h = narrow ? (th + bh_sm + 22.0f) : fmaxf(bh_sm + 16.0f, th + 16.0f);
    cy += rev_h + 8.0f;
  }
  if (needs_consent) {
    const char *ctxt = "Waiting for your permission approval before running.";
    float btnw = btn_w_sm(ui, r, "Review & Approve", 156.0f);
    float tw = narrow ? (inner_w - 20.0f) : fmaxf(inner_w - btnw - 30.0f, 80.0f);
    float th = fmaxf(line_h, ccord_text_measure(r, ctxt, strlen(ctxt), &ms, tw).y);
    consent_h = narrow ? (th + bh_sm + 22.0f) : fmaxf(bh_sm + 16.0f, th + 16.0f);
    cy += consent_h + 8.0f;
  }
  char wtxt[320] = "";
  if (has_warn) {
    snprintf(wtxt, sizeof wtxt, "Warning: %s%s%s", p->warning_title,
             (p->warning_message && p->warning_message[0]) ? " — " : "",
             (p->warning_message && p->warning_message[0]) ? p->warning_message : "");
    float th = fmaxf(line_h, ccord_text_measure(r, wtxt, strlen(wtxt), &cs, fmaxf(inner_w - 22.0f, 60.0f)).y);
    warn_h = th + 14.0f;
    cy += warn_h + 8.0f;
  }
  if (has_err) {
    float th = fmaxf(line_h, ccord_text_measure(r, p->error, strlen(p->error), &cs, fmaxf(inner_w - 22.0f, 60.0f)).y);
    err_h = th + 14.0f;
    cy += err_h + 8.0f;
  }
  char utxt[192] = "";
  if (has_upd) {
    if (p->update_needs_review) {
      size_t nnew = count_new_perms(p->update_perms_mask, p->consented_perms);
      if (nnew > 0) snprintf(utxt, sizeof utxt, "Update v%s available · +%zu new permission%s", p->update_version, nnew, nnew == 1 ? "" : "s");
      else snprintf(utxt, sizeof utxt, "Update v%s available · Review required", p->update_version);
    } else {
      snprintf(utxt, sizeof utxt, "Update v%s ready · Same permissions", p->update_version);
    }
    const char *ulbl = p->update_needs_review ? "Review & Update" : "Update";
    float btnw = btn_w_sm(ui, r, ulbl, 152.0f);
    float tw = narrow ? (inner_w - 20.0f) : fmaxf(inner_w - btnw - 30.0f, 80.0f);
    float th = fmaxf(line_h, ccord_text_measure(r, utxt, strlen(utxt), &cs, tw).y);
    upd_h = narrow ? (th + bh_sm + 20.0f) : fmaxf(bh_sm + 14.0f, th + 14.0f);
    cy += upd_h + 8.0f;
  }

  // Footer row (Configure?, Details, plus dev-mode Reload / Log)
  size_t ecnt = ui->d.contrib ? ccord_contrib_plugin_error_count(ui->d.contrib, p->id) : 0;
  char log_lbl[32];
  if (ecnt > 0) snprintf(log_lbl, sizeof log_lbl, "Log (%zu)", ecnt);
  else snprintf(log_lbl, sizeof log_lbl, "Log");

  struct { const char *lbl; phit_act act; bool show; } foot[4] = {
    {"Configure", ACT_CARD_CONFIGURE, has_cfg},
    {"Details",   ACT_CARD_DETAILS,   true},
    {"Reload",    ACT_CARD_RELOAD,    dev_mode || has_err},
    {log_lbl,     ACT_CARD_LOG,       dev_mode || has_err || ecnt > 0},
  };
  float fx = x + pad, fy = cy;
  for (int i = 0; i < 4; i++) {
    if (!foot[i].show) continue;
    float bw = btn_w_sm(ui, r, foot[i].lbl, inner_w);
    if (fx > x + pad && fx + bw > x + w - pad) {
      fx = x + pad;
      fy += bh_sm + 6.0f;
    }
    fx += bw + 6.0f;
  }
  float total_h = (fy + bh_sm + pad) - y;

  if (!paint) return total_h;
  if (y + total_h < body_clip.y || y > body_clip.y + body_clip.h) return total_h;

  ccord_rect card = {x, y, w, total_h};
  uis_card(ui, r, card);

  // Header row: icon + name + version + right switch + overflow button
  draw_plugin_icon(ui, r, (ccord_rect){x + pad, y + pad, icon_sz, icon_sz}, p->id, p->name, p->icon_path);

  float title_x = x + pad + icon_sz + 10.0f;
  float title_avail = fmaxf((x + w - pad - right_ctrl_w - 8.0f) - title_x, 40.0f);
  char ver_s[40];
  snprintf(ver_s, sizeof ver_s, "v%s", p->version);
  float vw = ccord_text_measure(r, ver_s, strlen(ver_s), &cs, 0.0f).x;
  float nw = ccord_text_draw_ellipsis(r, p->name, strlen(p->name), title_x, y + pad,
                                      fmaxf(title_avail - vw - 8.0f, 32.0f), &ns);
  if (title_x + nw + 6.0f + vw <= title_x + title_avail + 2.0f) {
    ccord_text_draw(r, ver_s, strlen(ver_s), title_x + nw + 6.0f, y + pad + 2.0f, &cs, 0.0f);
  }

  // Right controls: overflow '...' button + native toggle switch
  ccord_rect r_ov = {x + w - pad - ov_w, y + pad, ov_w, bh_sm};
  draw_btn(ui, r, r_ov, "...", UIS_BTN_SECONDARY, bsz_sm, false, "pov", (uint64_t)idx);
  hit_main_clipped(r_ov, body_clip, ACT_CARD_OVERFLOW, (int)idx, p->id);
  if (s_overflow_id[0] && strcmp(s_overflow_id, p->id) == 0) {
    s_overflow_anchor = r_ov;
  }

  if (!p->revoked && !needs_consent) {
    bool is_on = (p->state == CCORD_PLUGIN_READY || p->state == CCORD_PLUGIN_STARTING || p->enabled);
    ccord_rect sw_row = {r_ov.x - 8.0f - UIS_TOGGLE_W, y + pad, UIS_TOGGLE_W, bh_sm};
    ccord_rect sw_hit = uis_toggle_rect(sw_row);
    if (ui->mobile_layout) sw_hit = uis_hit_padded(sw_hit, 56.0f, UIS_MOB_TOUCH_H);
    uis_state swst = {.hover = !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, sw_hit), .disabled = !ui->extensions_on};
    uis_toggle(ui, r, sw_row, is_on && ui->extensions_on, swst, "psw", (uint64_t)idx);
    if (ui->extensions_on) hit_main_clipped(sw_hit, body_clip, ACT_CARD_TOGGLE, (int)idx, p->id);
  }

  // Pills row
  pill_flow_init(&pf, pills_x, y + pad + pills_off, pills_w);
  pill_flow_add(ui, r, &pf, tier_label(p->tier, p->is_bundled), tier_color(ui, p->tier, p->is_bundled), true);
  pill_flow_add(ui, r, &pf, api_class_label(api_cls), api_class_color(ui, api_cls), true);
  pill_flow_add(ui, r, &pf, sn, sc, true);
  if (!narrow) pill_flow_add(ui, r, &pf, plat_lbl, plat_warn ? COL(status_idle) : COL(text_muted), true);

  cy = fmaxf(y + pad + icon_sz, pill_flow_bottom(&pf)) + 8.0f;

  // Human description (clipped cleanly to 2 lines max)
  ccord_push_clip(r, intersect_rect((ccord_rect){x + pad, cy, inner_w, desc_h + 2.0f}, body_clip));
  ccord_text_draw(r, desc, strlen(desc), x + pad, cy, &ms, inner_w);
  ccord_pop_clip(r);
  cy += desc_h + 10.0f;

  // Banners
  if (rev_h > 0.0f) {
    ccord_rect b = {x + pad, cy, inner_w, rev_h};
    uis_plate(ui, r, b, tk.r_control, with_alpha(COL(danger), 28));
    ccord_draw_rect(r, (ccord_rect){b.x, b.y, 3.0f, b.h}, COL(danger));
    float btnw = btn_w_sm(ui, r, "Uninstall", 104.0f);
    ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(danger));
    float tw = narrow ? (b.w - 20.0f) : fmaxf(b.w - btnw - 30.0f, 80.0f);
    ccord_text_draw(r, rev_txt, strlen(rev_txt), b.x + 12.0f, b.y + 8.0f, &rs, tw);
    ccord_rect rb = narrow ? (ccord_rect){b.x + 12.0f, b.y + b.h - bh_sm - 8.0f, btnw, bh_sm}
                           : (ccord_rect){b.x + b.w - btnw - 8.0f, b.y + (b.h - bh_sm) * 0.5f, btnw, bh_sm};
    draw_btn(ui, r, rb, "Uninstall", UIS_BTN_DANGER, bsz_sm, false, "prev", (uint64_t)idx);
    hit_main_clipped(rb, body_clip, ACT_CARD_UNINSTALL_ASK, (int)idx, p->id);
    cy += rev_h + 8.0f;
  }
  if (consent_h > 0.0f) {
    ccord_rect b = {x + pad, cy, inner_w, consent_h};
    uis_plate(ui, r, b, tk.r_control, with_alpha(COL(status_idle), 28));
    ccord_draw_rect(r, (ccord_rect){b.x, b.y, 3.0f, b.h}, COL(status_idle));
    const char *ctxt = "Waiting for your permission approval before running.";
    float btnw = btn_w_sm(ui, r, "Review & Approve", 156.0f);
    ccord_text_style as = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(status_idle));
    float tw = narrow ? (b.w - 20.0f) : fmaxf(b.w - btnw - 30.0f, 80.0f);
    ccord_text_draw(r, ctxt, strlen(ctxt), b.x + 12.0f, b.y + 8.0f, &as, tw);
    ccord_rect rb = narrow ? (ccord_rect){b.x + 12.0f, b.y + b.h - bh_sm - 8.0f, btnw, bh_sm}
                           : (ccord_rect){b.x + b.w - btnw - 8.0f, b.y + (b.h - bh_sm) * 0.5f, btnw, bh_sm};
    draw_btn(ui, r, rb, "Review & Approve", UIS_BTN_PRIMARY, bsz_sm, false, "pcon", (uint64_t)idx);
    hit_main_clipped(rb, body_clip, ACT_CARD_REVIEW_CONSENT, (int)idx, p->id);
    cy += consent_h + 8.0f;
  }
  if (warn_h > 0.0f) {
    ccord_color wc = p->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
    ccord_rect b = {x + pad, cy, inner_w, warn_h};
    uis_plate(ui, r, b, tk.r_control, with_alpha(wc, 24));
    ccord_draw_rect(r, (ccord_rect){b.x, b.y, 3.0f, b.h}, wc);
    ccord_text_style ws = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, wc);
    ccord_text_draw(r, wtxt, strlen(wtxt), b.x + 12.0f, b.y + 7.0f, &ws, fmaxf(b.w - 22.0f, 60.0f));
    cy += warn_h + 8.0f;
  }
  if (err_h > 0.0f) {
    ccord_rect b = {x + pad, cy, inner_w, err_h};
    uis_plate(ui, r, b, tk.r_control, with_alpha(COL(danger), 24));
    ccord_draw_rect(r, (ccord_rect){b.x, b.y, 3.0f, b.h}, COL(danger));
    ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(danger));
    ccord_text_draw(r, p->error, strlen(p->error), b.x + 12.0f, b.y + 7.0f, &es, fmaxf(b.w - 22.0f, 60.0f));
    cy += err_h + 8.0f;
  }
  if (upd_h > 0.0f) {
    ccord_color uc = p->update_needs_review ? COL(status_idle) : COL(brand);
    ccord_rect b = {x + pad, cy, inner_w, upd_h};
    uis_plate(ui, r, b, tk.r_control, with_alpha(uc, 24));
    ccord_draw_rect(r, (ccord_rect){b.x, b.y, 3.0f, b.h}, uc);
    const char *ulbl = p->update_needs_review ? "Review & Update" : "Update";
    float btnw = btn_w_sm(ui, r, ulbl, 152.0f);
    ccord_text_style us = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, uc);
    float tw = narrow ? (b.w - 20.0f) : fmaxf(b.w - btnw - 30.0f, 80.0f);
    ccord_text_draw(r, utxt, strlen(utxt), b.x + 12.0f, b.y + 7.0f, &us, tw);
    ccord_rect rb = narrow ? (ccord_rect){b.x + 12.0f, b.y + b.h - bh_sm - 7.0f, btnw, bh_sm}
                           : (ccord_rect){b.x + b.w - btnw - 8.0f, b.y + (b.h - bh_sm) * 0.5f, btnw, bh_sm};
    draw_btn(ui, r, rb, ulbl, UIS_BTN_PRIMARY, bsz_sm, false, "pupd", (uint64_t)idx);
    hit_main_clipped(rb, body_clip, p->update_needs_review ? ACT_CARD_UPDATE_REVIEW : ACT_CARD_UPDATE_SAFE, (int)idx, p->id);
    cy += upd_h + 8.0f;
  }

  // Clean Footer row
  fx = x + pad;
  fy = cy;
  for (int i = 0; i < 4; i++) {
    if (!foot[i].show) continue;
    float bw = btn_w_sm(ui, r, foot[i].lbl, inner_w);
    if (fx > x + pad && fx + bw > x + w - pad) {
      fx = x + pad;
      fy += bh_sm + 6.0f;
    }
    ccord_rect rb = {fx, fy, bw, bh_sm};
    draw_btn(ui, r, rb, foot[i].lbl, UIS_BTN_SECONDARY, bsz_sm, false, "pft", (uint64_t)idx * 8u + (uint64_t)i);
    hit_main_clipped(rb, body_clip, foot[i].act, (int)idx, p->id);
    fx += bw + 6.0f;
  }
  (void)bh;
  return total_h;
}

static void draw_installed_tab(ccord_ui *ui, ccord_renderer *r, float x, float y, float w, ccord_rect body_clip)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  float start_y = y;

  size_t n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
  size_t c_active = 0, c_attn = 0, c_upd = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(ui->d.contrib, i);
    if (!p) continue;
    if (p->state == CCORD_PLUGIN_READY) c_active++;
    if (plugin_needs_attention(p)) c_attn++;
    if (p->update_version && p->update_version[0]) c_upd++;
  }

  // Search box + filter chips
  float bh_sm = pl_btn_sm(ui);
  ccord_rect r_q = {x, y, w, pl_btn(ui)};
  y = draw_search_input(ui, r, r_q, s_inst_search, "Filter installed plugins...",
                        s_inst_search_focused, ACT_INST_SEARCH_FOCUS, ACT_INST_SEARCH_CLEAR, body_clip, true) + 8.0f;

  char flbl[4][36];
  snprintf(flbl[0], sizeof flbl[0], "All (%zu)", n);
  snprintf(flbl[1], sizeof flbl[1], "Active (%zu)", c_active);
  snprintf(flbl[2], sizeof flbl[2], "Attention (%zu)", c_attn);
  snprintf(flbl[3], sizeof flbl[3], "Updates (%zu)", c_upd);
  float fx = x;
  for (int k = 0; k < 4; k++) {
    float bw = btn_w_sm(ui, r, flbl[k], w);
    if (fx > x && fx + bw > x + w) { fx = x; y += bh_sm + 6.0f; }
    ccord_rect cr = {fx, y, bw, bh_sm};
    draw_filter_chip(ui, r, cr, flbl[k], (int)s_inst_filter == k, "pifilt", (uint64_t)k);
    hit_main_clipped(cr, body_clip, ACT_INST_FILTER, k, "");
    fx += bw + 6.0f;
  }
  y += bh_sm + 10.0f;

  if (s_status_msg[0]) {
    ccord_text_draw_ellipsis(r, s_status_msg, strlen(s_status_msg), x, y, w, &ms);
    y += 22.0f;
  }

  if (n == 0) {
    ccord_rect empty = {x, y, w, 86.0f};
    uis_card(ui, r, empty);
    const char *t1 = "No plugins installed yet.";
    const char *t2 = "Drop a .cce package onto C-Cord or switch to Discover to browse verified plugins.";
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
    ccord_text_draw(r, t1, strlen(t1), empty.x + 16.0f, empty.y + 16.0f, &ns, empty.w - 32.0f);
    ccord_text_draw(r, t2, strlen(t2), empty.x + 16.0f, empty.y + 42.0f, &ms, empty.w - 32.0f);
    st_s_plugins_content_h = (y + empty.h + 12.0f) - start_y;
    return;
  }

  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(ui->d.contrib, i);
    if (!p) continue;
    if (s_inst_filter == IFILT_ACTIVE && p->state != CCORD_PLUGIN_READY) continue;
    if (s_inst_filter == IFILT_ATTENTION && !plugin_needs_attention(p)) continue;
    if (s_inst_filter == IFILT_UPDATES && !(p->update_version && p->update_version[0])) continue;
    if (s_inst_search[0] &&
        !contains_ci(p->name, s_inst_search) &&
        !contains_ci(p->id, s_inst_search) &&
        !contains_ci(p->description, s_inst_search)) {
      continue;
    }
    float h = layout_or_draw_installed_card(ui, r, p, i, x, y, w, body_clip, true);
    y += h + 10.0f;
    shown++;
  }
  if (shown == 0) {
    const char *none = "No installed plugins match your current filter.";
    ccord_text_draw(r, none, strlen(none), x, y + 6.0f, &ms, w);
    y += 32.0f;
  }
  st_s_plugins_content_h = (y + 12.0f) - start_y;
}

// ---------------------------------------------------------------- Tab 1: Discover

static float layout_or_draw_discover_card(ccord_ui *ui, ccord_renderer *r, const ccord_catalog_entry *e,
                                          size_t idx, float x, float y, float w, float fixed_h,
                                          ccord_rect body_clip, bool paint)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));

  float pad = w < 380.0f ? 12.0f : 14.0f;
  float icon_sz = 40.0f;
  float inner_w = fmaxf(w - pad * 2.0f, 80.0f);
  bool mob = is_running_mobile(ui);
  bool plat_warn = false;
  const char *plat_lbl = platform_badge_label(e->platforms, mob, &plat_warn);
  bool has_w = e->warning_sev != CCORD_WARN_NONE && e->warning_title && e->warning_title[0];

  float cy = y + pad + icon_sz + 8.0f;
  pill_flow pf;
  pill_flow_init(&pf, x + pad, cy, inner_w);
  pill_flow_add(ui, r, &pf, tier_label(e->tier, e->bundled), tier_color(ui, e->tier, e->bundled), false);
  pill_flow_add(ui, r, &pf, api_class_label(e->api_class), api_class_color(ui, e->api_class), false);
  pill_flow_add(ui, r, &pf, plat_lbl, plat_warn ? COL(status_idle) : COL(text_muted), false);
  cy = pill_flow_bottom(&pf) + 8.0f;

  float line_h = ui_line_h(ui, tk.fs_control);
  float desc_h = line_h * 2.0f;
  cy += desc_h + 8.0f;
  float bh_sm = pl_btn_sm(ui);
  uis_btn_size bsz_sm = pl_sz_sm(ui);
  if (has_w) cy += 22.0f;
  float calc_h = (cy + bh_sm + pad + 4.0f) - y;
  float h = fixed_h > calc_h ? fixed_h : calc_h;

  if (!paint) return h;
  if (y + h < body_clip.y || y > body_clip.y + body_clip.h) return h;

  ccord_rect card = {x, y, w, h};
  uis_card(ui, r, card);
  // Subtle hover feedback on full-card clickable area
  bool card_hov = !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, intersect_rect(card, body_clip));
  if (card_hov) {
    uis_plate(ui, r, card, tk.r_surface, with_alpha(COL(brand), 14));
  }
  // Register full-card hit first so the CTA button registered after it takes precedence!
  hit_main_clipped(card, body_clip, ACT_CAT_DETAILS, (int)idx, e->id);

  // Header: Icon + Name + Author & Version
  draw_plugin_icon(ui, r, (ccord_rect){x + pad, y + pad, icon_sz, icon_sz}, e->id, e->name, e->icon_path);
  float tx = x + pad + icon_sz + 10.0f;
  float tw = fmaxf(x + w - pad - tx, 40.0f);
  ccord_text_draw_ellipsis(r, e->name, strlen(e->name), tx, y + pad + 1.0f, tw, &ns);
  char sub[128];
  snprintf(sub, sizeof sub, "by @%s · v%s", e->author && e->author[0] ? e->author : "unknown", e->version);
  ccord_text_draw_ellipsis(r, sub, strlen(sub), tx, y + pad + 22.0f, tw, &cs);

  // Pills
  cy = y + pad + icon_sz + 8.0f;
  pill_flow_init(&pf, x + pad, cy, inner_w);
  pill_flow_add(ui, r, &pf, tier_label(e->tier, e->bundled), tier_color(ui, e->tier, e->bundled), true);
  pill_flow_add(ui, r, &pf, api_class_label(e->api_class), api_class_color(ui, e->api_class), true);
  pill_flow_add(ui, r, &pf, plat_lbl, plat_warn ? COL(status_idle) : COL(text_muted), true);
  cy = pill_flow_bottom(&pf) + 8.0f;

  // 2-line description clipped cleanly
  const char *desc = (e->description && e->description[0]) ? e->description : "No description provided.";
  ccord_push_clip(r, intersect_rect((ccord_rect){x + pad, cy, inner_w, desc_h + 2.0f}, body_clip));
  ccord_text_draw(r, desc, strlen(desc), x + pad, cy, &ms, inner_w);
  ccord_pop_clip(r);
  cy += desc_h + 8.0f;

  if (has_w) {
    ccord_color wc = e->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
    ccord_text_style ws = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, wc);
    char wl[160];
    snprintf(wl, sizeof wl, "Warning: %s", e->warning_title);
    ccord_text_draw_ellipsis(r, wl, strlen(wl), x + pad, cy, inner_w, &ws);
  }

  // Bottom footer pinned cleanly to card bottom
  float fy = y + h - pad - bh_sm;
  const char *cta = e->installing ? "Installing..."
                  : e->bundled ? "Bundled"
                  : plat_warn && !e->installed ? "Desktop Only"
                  : !e->installed ? (e->api_class >= 2 ? "Review & Install" : "Install")
                  : e->update_available ? (e->update_needs_review ? "Review Update" : "Update")
                  : "Installed";
  bool cta_disabled = e->installing || e->bundled || (plat_warn && !e->installed) ||
                      (e->installed && !e->update_available);
  uis_btn_kind kind = (!cta_disabled && (!e->installed || e->update_available)) ? UIS_BTN_PRIMARY : UIS_BTN_SECONDARY;
  float bw = btn_w_sm(ui, r, cta, inner_w * 0.62f);
  ccord_rect r_cta = {x + w - pad - bw, fy, bw, bh_sm};
  draw_btn(ui, r, r_cta, cta, kind, bsz_sm, cta_disabled, "pdact", (uint64_t)idx);
  if (!cta_disabled) hit_main_clipped(r_cta, body_clip, ACT_CAT_INSTALL, (int)idx, e->id);

  char dls[32];
  format_downloads(e->downloads, dls, sizeof dls);
  ccord_text_draw_ellipsis(r, dls, strlen(dls), x + pad, fy + (ui->mobile_layout ? 10.0f : 6.0f),
                           fmaxf(r_cta.x - (x + pad) - 8.0f, 30.0f), &cs);
  return h;
}

static void draw_discover_tab(ccord_ui *ui, ccord_renderer *r, float x, float y, float w, ccord_rect body_clip)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
  const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
  float start_y = y;

  // Row 1: Search bar + Search button + Sort dropdown (measured at body font to match uis_select!)
  char slbl[48];
  snprintf(slbl, sizeof slbl, "Sort: %s", sort_short_label(s_disc_sort_idx));
  ccord_text_style sel_st = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_body, COL(text_header));
  float go_w = btn_w_sm(ui, r, "Search", 96.0f);
  float sort_w = fmaxf(ceilf(ccord_text_measure(r, slbl, strlen(slbl), &sel_st, 0.0f).x) + 60.0f, 148.0f);
  if (sort_w > 224.0f) sort_w = 224.0f;
  float bh_sm = pl_btn_sm(ui);
  float bh = pl_btn(ui);
  uis_btn_size bsz_sm = pl_sz_sm(ui);
  if (w < 460.0f) {
    ccord_rect r_q = {x, y, w, bh};
    y = draw_search_input(ui, r, r_q, s_disc_search, "Search catalog plugins...",
                          s_disc_search_focused, ACT_DISC_SEARCH_FOCUS, ACT_DISC_SEARCH_CLEAR, body_clip, true) + 6.0f;
    ccord_rect r_go = {x, y, go_w, bh_sm};
    float sw = fminf(sort_w, fmaxf(w - go_w - 6.0f, 84.0f));
    ccord_rect r_sort = {x + go_w + 6.0f, y, sw, bh_sm};
    draw_btn(ui, r, r_go, "Search", UIS_BTN_PRIMARY, bsz_sm, false, "pds", 1);
    uis_state sst = {.hover = !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, r_sort), .selected = s_sort_open};
    uis_select(ui, r, r_sort, slbl, sst, "pdsort", 0);
    s_sort_anchor = r_sort;
    hit_main_clipped(r_go, body_clip, ACT_DISC_SEARCH_GO, 0, "");
    hit_main_clipped(r_sort, body_clip, ACT_DISC_SORT_TOGGLE, 0, "");
    y += bh_sm + 8.0f;
  } else {
    float q_w = fmaxf(w - go_w - sort_w - 12.0f, 120.0f);
    ccord_rect r_q = {x, y, q_w, bh};
    (void)draw_search_input(ui, r, r_q, s_disc_search, "Search plugins by name, author, or description...",
                            s_disc_search_focused, ACT_DISC_SEARCH_FOCUS, ACT_DISC_SEARCH_CLEAR, body_clip, true);
    ccord_rect r_go = {x + q_w + 6.0f, y + 2.0f, go_w, bh_sm};
    ccord_rect r_sort = {x + q_w + go_w + 12.0f, y + 2.0f, sort_w, bh_sm};
    draw_btn(ui, r, r_go, "Search", UIS_BTN_PRIMARY, bsz_sm, false, "pds", 1);
    uis_state sst = {.hover = !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, r_sort), .selected = s_sort_open};
    uis_select(ui, r, r_sort, slbl, sst, "pdsort", 0);
    s_sort_anchor = r_sort;
    hit_main_clipped(r_go, body_clip, ACT_DISC_SEARCH_GO, 0, "");
    hit_main_clipped(r_sort, body_clip, ACT_DISC_SORT_TOGGLE, 0, "");
    y += bh + 8.0f;
  }

  // Row 2: Dynamic wrapping filter chips (Trust + Class 1 Safe + Platform)
  float fx = x;
  static const char *const k_tiers[4] = {"All", "Official", "Verified", "Community"};
  for (int t = 0; t < 4; t++) {
    float bw = btn_w_sm(ui, r, k_tiers[t], w);
    if (fx > x && fx + bw > x + w) { fx = x; y += bh_sm + 6.0f; }
    ccord_rect cr = {fx, y, bw, bh_sm};
    draw_filter_chip(ui, r, cr, k_tiers[t], s_disc_tier_filter == t, "pdt", (uint64_t)t);
    hit_main_clipped(cr, body_clip, ACT_DISC_TIER, t, "");
    fx += bw + 6.0f;
  }
  {
    const char *c1 = "Class 1 Safe";
    float bw = btn_w_sm(ui, r, c1, w);
    if (fx > x && fx + bw > x + w) { fx = x; y += bh_sm + 6.0f; }
    ccord_rect cr = {fx, y, bw, bh_sm};
    draw_filter_chip(ui, r, cr, c1, s_disc_class1_only, "pdc1", 0);
    hit_main_clipped(cr, body_clip, ACT_DISC_CLASS1, 0, "");
    fx += bw + 6.0f;
  }
  static const char *const k_plats[3] = {"All Platforms", "Desktop", "Android"};
  for (int p = 0; p < 3; p++) {
    float bw = btn_w_sm(ui, r, k_plats[p], w);
    if (fx > x && fx + bw > x + w) { fx = x; y += bh_sm + 6.0f; }
    ccord_rect cr = {fx, y, bw, bh_sm};
    draw_filter_chip(ui, r, cr, k_plats[p], (int)s_disc_plat_filter == p, "pdp", (uint64_t)p);
    hit_main_clipped(cr, body_clip, ACT_DISC_PLAT, p, "");
    fx += bw + 6.0f;
  }
  y += bh_sm + 12.0f;

  size_t cn = ui->d.contrib ? ccord_contrib_catalog_count(ui->d.contrib) : 0;
  if (cn == 0) {
    ccord_rect empty = {x, y, w, 82.0f};
    uis_card(ui, r, empty);
    const char *msg = (cst && cst->status_msg[0]) ? cst->status_msg
                    : "No plugins loaded yet. Click Sync or Search to browse plugins.c-cord.org.";
    ccord_text_draw(r, msg, strlen(msg), empty.x + 16.0f, empty.y + 18.0f, &ms, empty.w - 32.0f);
    st_s_plugins_content_h = (y + empty.h + 12.0f) - start_y;
    return;
  }

  // Responsive 2-column grid on desktop (w >= 680px), 1-column on narrow/mobile
  int cols = (w >= 680.0f) ? 2 : 1;
  float col_gap = 12.0f;
  float col_w = cols == 2 ? floorf((w - col_gap) * 0.5f) : w;
  for (size_t i = 0; i < cn; i += (size_t)cols) {
    const ccord_catalog_entry *e0 = ccord_contrib_catalog_at(ui->d.contrib, i);
    const ccord_catalog_entry *e1 = (cols == 2 && i + 1 < cn) ? ccord_contrib_catalog_at(ui->d.contrib, i + 1) : nullptr;
    float h0 = e0 ? layout_or_draw_discover_card(ui, r, e0, i, x, y, col_w, 0.0f, body_clip, false) : 0.0f;
    float h1 = e1 ? layout_or_draw_discover_card(ui, r, e1, i + 1, x + col_w + col_gap, y, col_w, 0.0f, body_clip, false) : 0.0f;
    float row_h = fmaxf(h0, h1);
    if (e0) (void)layout_or_draw_discover_card(ui, r, e0, i, x, y, col_w, row_h, body_clip, true);
    if (e1) (void)layout_or_draw_discover_card(ui, r, e1, i + 1, x + col_w + col_gap, y, col_w, row_h, body_clip, true);
    y += row_h + 10.0f;
  }

  if (cst && cst->has_more) {
    const char *mlbl = cst->loading_more ? "Loading..." : "Load More";
    float bw = btn_w_sm(ui, r, mlbl, 148.0f);
    ccord_rect r_more = {x, y + 2.0f, bw, bh_sm};
    draw_btn(ui, r, r_more, mlbl, UIS_BTN_SECONDARY, bsz_sm, cst->loading_more, "pdmore", 0);
    if (!cst->loading_more) hit_main_clipped(r_more, body_clip, ACT_DISC_LOAD_MORE, 0, "");
    char pinfo[96];
    snprintf(pinfo, sizeof pinfo, "Loaded %zu of %zu plugins", cn, cst->total_count);
    if (w >= bw + 140.0f) {
      ccord_text_draw_ellipsis(r, pinfo, strlen(pinfo), x + bw + 12.0f,
                               r_more.y + (bh_sm - ui_line_h(ui, cs.size)) * 0.5f, w - bw - 16.0f, &cs);
    }
    y += bh_sm + 16.0f;
  }
  st_s_plugins_content_h = (y + 12.0f) - start_y;
}

// ---------------------------------------------------------------- Tab 2: Updates

static float draw_update_card(ccord_ui *ui, ccord_renderer *r, const ccord_catalog_entry *e,
                              size_t idx, float x, float y, float w, ccord_rect body_clip)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
  bool narrow = w < 440.0f;
  float pad = narrow ? 12.0f : 14.0f;
  float icon_sz = 36.0f;
  float inner_w = fmaxf(w - pad * 2.0f, 80.0f);
  const ccord_plugin_info *inst = find_plugin_info(ui->d.contrib, e->id);
  const char *icon_p = (inst && inst->icon_path && inst->icon_path[0]) ? inst->icon_path : e->icon_path;

  const char *blbl = e->installing ? "Updating..."
                   : e->update_needs_review ? "Review & Update" : "Update";
  float bw = btn_w_sm(ui, r, blbl, 152.0f);
  bool has_cl = e->changelog && e->changelog[0];
  float ch = (narrow ? (ui->mobile_layout ? 136.0f : 124.0f) : 78.0f) + (has_cl ? 20.0f : 0.0f);
  float bh_sm = pl_btn_sm(ui);
  uis_btn_size bsz_sm = pl_sz_sm(ui);

  if (y + ch >= body_clip.y && y <= body_clip.y + body_clip.h) {
    ccord_rect uc = {x, y, w, ch};
    uis_card(ui, r, uc);
    ccord_color accent = e->update_needs_review ? COL(status_idle) : COL(status_online);
    ccord_draw_rect(r, (ccord_rect){uc.x, uc.y + 8.0f, 3.0f, uc.h - 16.0f}, accent);

    draw_plugin_icon(ui, r, (ccord_rect){uc.x + pad + 4.0f, uc.y + pad, icon_sz, icon_sz}, e->id, e->name, icon_p);
    float tx = uc.x + pad + icon_sz + 14.0f;
    float tw = narrow ? fmaxf(uc.x + uc.w - pad - tx, 60.0f) : fmaxf(uc.x + uc.w - pad - bw - 12.0f - tx, 60.0f);

    float msg_y = uc.y + pad + 21.0f;
    if (narrow) {
      ccord_text_draw_ellipsis(r, e->name, strlen(e->name), tx, uc.y + pad, tw, &ns);
      char vv[96];
      snprintf(vv, sizeof vv, "v%s -> v%s", e->installed_version, e->version);
      ccord_text_draw_ellipsis(r, vv, strlen(vv), tx, uc.y + pad + 20.0f, tw, &cs);
      msg_y = uc.y + pad + icon_sz + 6.0f;
    } else {
      char ut[192];
      snprintf(ut, sizeof ut, "%s  v%s -> v%s", e->name, e->installed_version, e->version);
      ccord_text_draw_ellipsis(r, ut, strlen(ut), tx, uc.y + pad, tw, &ns);
    }

    char msg[192];
    if (e->update_needs_review) {
      size_t nnew = count_new_perms(e->perms_mask, inst ? inst->consented_perms : 0);
      bool had_net = inst && ((inst->consented_perms & ((uint64_t)CCORD_PERM_NETWORK_HTTP | (uint64_t)CCORD_PERM_NETWORK_WS)) != 0);
      bool host_exp = false;
      (void)ui_hosts_changed_or_expanded(inst ? inst->allowed_hosts : nullptr, had_net,
                                         e->allowed_hosts, nullptr, &host_exp);
      if (nnew > 0) snprintf(msg, sizeof msg, "+%zu new permission%s — review required", nnew, nnew == 1 ? "" : "s");
      else if (host_exp) snprintf(msg, sizeof msg, "Network allowed_hosts expanded — review required");
      else if (e->warning_sev >= CCORD_WARN_DANGER) snprintf(msg, sizeof msg, "Moderator warning attached — review required");
      else snprintf(msg, sizeof msg, "Trust or scope review required before updating");
    } else {
      snprintf(msg, sizeof msg, "Same API permissions · SHA-256 verified");
    }
    ccord_text_style st_s = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, accent);
    float msg_x = narrow ? (uc.x + pad + 4.0f) : tx;
    float msg_w = narrow ? inner_w : tw;
    ccord_text_draw_ellipsis(r, msg, strlen(msg), msg_x, msg_y, msg_w, &st_s);

    if (has_cl) {
      float cl_y = narrow ? (msg_y + 18.0f) : (uc.y + pad + icon_sz + 8.0f);
      ccord_text_draw_ellipsis(r, e->changelog, strlen(e->changelog), uc.x + pad + 4.0f,
                               cl_y, narrow ? inner_w : fmaxf(inner_w - bw - 12.0f, 60.0f), &cs);
    }

    ccord_rect r_btn = narrow ? (ccord_rect){uc.x + pad + 4.0f, uc.y + uc.h - pad - bh_sm, bw, bh_sm}
                              : (ccord_rect){uc.x + uc.w - pad - bw, uc.y + (uc.h - bh_sm) * 0.5f, bw, bh_sm};
    draw_btn(ui, r, r_btn, blbl, UIS_BTN_PRIMARY, bsz_sm, e->installing, "puact", (uint64_t)idx);
    if (!e->installing) {
      hit_main_clipped(r_btn, body_clip, ACT_CAT_INSTALL, (int)idx, e->id);
    }
  }
  return ch;
}

static void draw_updates_tab(ccord_ui *ui, ccord_renderer *r, float x, float y, float w, ccord_rect body_clip)
{
  uis_tokens tk = uis_tokens_of(ui);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  ccord_text_style ovs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
  float start_y = y;
  float bh_sm = pl_btn_sm(ui);
  uis_btn_size bsz_sm = pl_sz_sm(ui);

  // Count pending safe vs review-needed updates
  size_t un = ui->d.contrib ? ccord_contrib_catalog_update_count(ui->d.contrib) : 0;
  size_t n_review = 0, n_safe = 0;
  for (size_t i = 0; i < un; i++) {
    const ccord_catalog_entry *e = ccord_contrib_catalog_update_at(ui->d.contrib, i);
    if (!e || !e->update_available) continue;
    if (e->update_needs_review) n_review++;
    else n_safe++;
  }
  size_t n_total = n_review + n_safe;

  // 1. Pending Updates FIRST!
  char hdr[64];
  snprintf(hdr, sizeof hdr, "PENDING UPDATES (%zu)", n_total);
  char safe_lbl[48];
  snprintf(safe_lbl, sizeof safe_lbl, "Update All Safe (%zu)", n_safe);
  float bw_safe = btn_w_sm(ui, r, safe_lbl, w);

  if (w >= bw_safe + 190.0f) {
    ccord_text_draw(r, hdr, strlen(hdr), x, y + 6.0f, &ovs, 0.0f);
    if (n_safe > 0) {
      ccord_rect r_all = {x + w - bw_safe, y, bw_safe, bh_sm};
      draw_btn(ui, r, r_all, safe_lbl, UIS_BTN_PRIMARY, bsz_sm, false, "puall", 0);
      hit_main_clipped(r_all, body_clip, ACT_UPD_RUN_SAFE, 0, "");
    }
    y += bh_sm + 10.0f;
  } else {
    ccord_text_draw(r, hdr, strlen(hdr), x, y, &ovs, 0.0f);
    y += 18.0f;
    if (n_safe > 0) {
      ccord_rect r_all = {x, y, bw_safe, bh_sm};
      draw_btn(ui, r, r_all, safe_lbl, UIS_BTN_PRIMARY, bsz_sm, false, "puall", 0);
      hit_main_clipped(r_all, body_clip, ACT_UPD_RUN_SAFE, 0, "");
      y += bh_sm + 10.0f;
    }
  }

  if (n_total == 0) {
    ccord_rect ok_card = {x, y, w, 78.0f};
    uis_card(ui, r, ok_card);
    float chk_w = btn_w_sm(ui, r, "Check Now", 118.0f);
    bool stack = w < 380.0f;
    if (stack) ok_card.h = ui->mobile_layout ? 116.0f : 104.0f;
    ccord_text_style gs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(status_online));
    const char *ok1 = "Everything is up to date";
    const char *ok2 = "All installed catalog plugins are on their latest verified release.";
    ccord_text_draw_ellipsis(r, ok1, strlen(ok1), ok_card.x + 14.0f, ok_card.y + 14.0f,
                             stack ? (ok_card.w - 28.0f) : (ok_card.w - chk_w - 36.0f), &gs);
    ccord_text_draw_ellipsis(r, ok2, strlen(ok2), ok_card.x + 14.0f, ok_card.y + 38.0f,
                             stack ? (ok_card.w - 28.0f) : (ok_card.w - chk_w - 36.0f), &ms);
    ccord_rect r_chk = stack ? (ccord_rect){ok_card.x + 14.0f, ok_card.y + ok_card.h - bh_sm - 12.0f, chk_w, bh_sm}
                             : (ccord_rect){ok_card.x + ok_card.w - chk_w - 14.0f, ok_card.y + (ok_card.h - bh_sm) * 0.5f, chk_w, bh_sm};
    draw_btn(ui, r, r_chk, "Check Now", UIS_BTN_SECONDARY, bsz_sm, false, "puchk", 0);
    hit_main_clipped(r_chk, body_clip, ACT_UPD_CHECK, 0, "");
    y += ok_card.h + 14.0f;
  } else {
    if (n_review > 0) {
      ccord_text_style rh = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(status_idle));
      char rl[64];
      snprintf(rl, sizeof rl, "NEEDS YOUR REVIEW (%zu)", n_review);
      ccord_text_draw(r, rl, strlen(rl), x, y, &rh, 0.0f);
      y += 18.0f;
      for (size_t i = 0; i < un; i++) {
        const ccord_catalog_entry *e = ccord_contrib_catalog_update_at(ui->d.contrib, i);
        if (!e || !e->update_available || !e->update_needs_review) continue;
        y += draw_update_card(ui, r, e, i, x, y, w, body_clip) + 8.0f;
      }
      y += 6.0f;
    }
    if (n_safe > 0) {
      ccord_text_style sh = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(status_online));
      char sl[64];
      snprintf(sl, sizeof sl, "READY TO UPDATE · SAFE (%zu)", n_safe);
      ccord_text_draw(r, sl, strlen(sl), x, y, &sh, 0.0f);
      y += 18.0f;
      for (size_t i = 0; i < un; i++) {
        const ccord_catalog_entry *e = ccord_contrib_catalog_update_at(ui->d.contrib, i);
        if (!e || !e->update_available || e->update_needs_review) continue;
        y += draw_update_card(ui, r, e, i, x, y, w, body_clip) + 8.0f;
      }
      y += 6.0f;
    }
  }

  // 2. Collapsible Auto-Update Policy Bar
  const ccord_catalog_prefs *pr_cur = ccord_contrib_catalog_prefs(ui->d.contrib);
  ccord_catalog_prefs pr = pr_cur ? *pr_cur : (ccord_catalog_prefs){.auto_update_enabled = false, .auto_update_min_tier = 2};
  static const char *const k_tlbl[3] = {"Official Only", "Official & Verified", "All Catalog"};
  const char *cur_tier_lbl = (pr.auto_update_min_tier >= 1 && pr.auto_update_min_tier <= 3)
                               ? k_tlbl[pr.auto_update_min_tier - 1] : k_tlbl[1];

  float pad = 14.0f;
  float inner_w = fmaxf(w - pad * 2.0f, 80.0f);
  bool narrow_pol = w < 440.0f;
  float hdr_h = narrow_pol ? (ui->mobile_layout ? 68.0f : 62.0f) : 54.0f;
  float policy_h = hdr_h;
  float chips_end_y = 0.0f;
  if (s_policy_expanded) {
    const char *anote = "Verifies SHA-256 checksums and pauses whenever an update adds new Class 2/3 API permissions.";
    float nh = fmaxf(18.0f, ccord_text_measure(r, anote, strlen(anote), &ms, inner_w).y);
    float tx = x + pad, ty = y + hdr_h + nh + 10.0f;
    for (int k = 0; k < 3; k++) {
      float cw = btn_w_sm(ui, r, k_tlbl[k], inner_w);
      if (tx > x + pad && tx + cw > x + w - pad) { tx = x + pad; ty += bh_sm + 6.0f; }
      tx += cw + 8.0f;
    }
    chips_end_y = ty + bh_sm + 14.0f;
    policy_h = chips_end_y - y;
  }

  ccord_rect pbox = {x, y, w, policy_h};
  uis_card(ui, r, pbox);
  const char *exp_lbl = s_policy_expanded ? "Hide" : "Configure";
  float exp_w = btn_w_sm(ui, r, exp_lbl, 96.0f);
  ccord_rect r_exp = {pbox.x + pbox.w - pad - exp_w, pbox.y + (hdr_h - bh_sm) * 0.5f, exp_w, bh_sm};
  ccord_rect sw_row = {r_exp.x - 10.0f - UIS_TOGGLE_W, pbox.y + (hdr_h - bh_sm) * 0.5f, UIS_TOGGLE_W, bh_sm};
  ccord_rect sw_hit = uis_toggle_rect(sw_row);
  if (ui->mobile_layout) sw_hit = uis_hit_padded(sw_hit, 56.0f, UIS_MOB_TOUCH_H);
  float pl_avail = fmaxf(sw_row.x - (pbox.x + pad) - 8.0f, 60.0f);

  if (narrow_pol) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, COL(text_header));
    ccord_text_style pcs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
    ccord_text_draw_ellipsis(r, "Auto-Updates", 12, pbox.x + pad, pbox.y + 13.0f, pl_avail, &ps);
    char sub[64];
    if (pr.auto_update_enabled) snprintf(sub, sizeof sub, "On · %s", cur_tier_lbl);
    else snprintf(sub, sizeof sub, "Off");
    ccord_text_draw_ellipsis(r, sub, strlen(sub), pbox.x + pad, pbox.y + 32.0f, pl_avail, &pcs);
  } else {
    char pline[128];
    if (pr.auto_update_enabled) snprintf(pline, sizeof pline, "Auto-Updates: On (%s)", cur_tier_lbl);
    else snprintf(pline, sizeof pline, "Auto-Updates: Off");
    ccord_text_draw_ellipsis(r, pline, strlen(pline), pbox.x + pad, pbox.y + 17.0f, pl_avail, &ns);
  }
  uis_state swst = {.hover = !st_plugins_has_overlay() && ui_in_rect(ui->mx, ui->my, sw_hit)};
  uis_toggle(ui, r, sw_row, pr.auto_update_enabled, swst, "swautoupd", 0);
  hit_main_clipped(sw_hit, body_clip, ACT_UPD_POLICY_TOGGLE, 0, "");
  draw_btn(ui, r, r_exp, exp_lbl, UIS_BTN_SECONDARY, bsz_sm, false, "pupexp", 0);
  hit_main_clipped(r_exp, body_clip, ACT_UPD_POLICY_EXPAND, 0, "");

  if (s_policy_expanded) {
    const char *anote = "Verifies SHA-256 checksums and pauses whenever an update adds new Class 2/3 API permissions.";
    float nh = fmaxf(18.0f, ccord_text_measure(r, anote, strlen(anote), &ms, inner_w).y);
    ccord_text_draw(r, anote, strlen(anote), pbox.x + pad, pbox.y + hdr_h - 2.0f, &ms, inner_w);
    float tx = pbox.x + pad, ty = pbox.y + hdr_h + nh + 10.0f;
    for (int k = 0; k < 3; k++) {
      float cw = btn_w_sm(ui, r, k_tlbl[k], inner_w);
      if (tx > pbox.x + pad && tx + cw > pbox.x + pbox.w - pad) { tx = pbox.x + pad; ty += bh_sm + 6.0f; }
      ccord_rect tr = {tx, ty, cw, bh_sm};
      draw_filter_chip(ui, r, tr, k_tlbl[k], pr.auto_update_min_tier == (uint8_t)(k + 1), "putier", (uint64_t)k);
      hit_main_clipped(tr, body_clip, ACT_UPD_POLICY_TIER, k + 1, "");
      tx += cw + 8.0f;
    }
  }
  y += policy_h + 14.0f;
  st_s_plugins_content_h = y - start_y;
}

// ---------------------------------------------------------------- main draw entry

void st_draw_plugins(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  s_nhits_main = 0;
  uis_tokens tk = uis_tokens_of(ui);

  // Auto-open Protected Security Modal when a pending detail fetch finishes!
  if (s_pending_sec_id[0] && ui->d.contrib) {
    const ccord_catalog_entry *e = find_catalog_entry(ui->d.contrib, s_pending_sec_id);
    const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
    if (e && e->has_detail) {
      sec_modal_mode m = s_pending_sec_mode;
      s_pending_sec_id[0] = '\0';
      s_pending_sec_mode = SEC_NONE;
      s_status_msg[0] = '\0';
      open_sec_modal_for_catalog(ui, e, m);
    } else if (cst && !cst->detail_loading && cst->detail_error[0]) {
      snprintf(s_status_msg, sizeof s_status_msg, "Could not load permissions: %.140s", cst->detail_error);
      s_pending_sec_id[0] = '\0';
      s_pending_sec_mode = SEC_NONE;
    }
  }

  float x = g->content.x, w = g->content.w;
  float tab_y = st_row_y(g, 0) + (ui->mobile_layout ? 48.0f : 46.0f);

  size_t inst_n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
  const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
  size_t pend_upd = cst ? cst->pending_updates : 0;

  char l_inst[32], l_upd[32];
  snprintf(l_inst, sizeof l_inst, "Installed (%zu)", inst_n);
  if (pend_upd > 0) snprintf(l_upd, sizeof l_upd, "Updates (%zu)", pend_upd);
  else snprintf(l_upd, sizeof l_upd, "Updates");
  const char *l_sync = (cst && cst->syncing) ? "Syncing..." : "Sync";

  // Dynamically measure segmented tabs + Sync button so ZERO labels ever truncate!
  float w_inst = btn_w_sm(ui, r, l_inst, w);
  float w_disc = btn_w_sm(ui, r, "Discover", w);
  float w_upd = btn_w_sm(ui, r, l_upd, w);
  float w_sync = btn_w_sm(ui, r, l_sync, w);

  float bh_sm = pl_btn_sm(ui);
  uis_btn_size bsz_sm = pl_sz_sm(ui);
  float tx = x, ty = tab_y;
  bool on_sub = !s_detail_id[0] && !s_log_id[0];
  ccord_rect r_inst = {tx, ty, w_inst, bh_sm};
  tx += w_inst + 6.0f;
  if (tx + w_disc > x + w) { tx = x; ty += bh_sm + 6.0f; }
  ccord_rect r_disc = {tx, ty, w_disc, bh_sm};
  tx += w_disc + 6.0f;
  if (tx + w_upd > x + w) { tx = x; ty += bh_sm + 6.0f; }
  ccord_rect r_upd = {tx, ty, w_upd, bh_sm};
  tx += w_upd + 6.0f;
  ccord_rect r_sync;
  if (tx + w_sync <= x + w) {
    r_sync = (ccord_rect){x + w - w_sync, ty, w_sync, bh_sm};
  } else {
    ty += bh_sm + 6.0f;
    r_sync = (ccord_rect){x, ty, w_sync, bh_sm};
  }

  draw_filter_chip(ui, r, r_inst, l_inst, on_sub && s_subtab == 0, "ptab", 0);
  draw_filter_chip(ui, r, r_disc, "Discover", on_sub && s_subtab == 1, "ptab", 1);
  draw_filter_chip(ui, r, r_upd, l_upd, on_sub && s_subtab == 2, "ptab", 2);
  draw_btn(ui, r, r_sync, l_sync, UIS_BTN_SECONDARY, bsz_sm, cst && cst->syncing, "psync", 0);
  hit_main(r_inst, ACT_TAB, 0, "");
  hit_main(r_disc, ACT_TAB, 1, "");
  hit_main(r_upd, ACT_TAB, 2, "");
  if (!(cst && cst->syncing)) hit_main(r_sync, ACT_SYNC, 0, "");

  float body_top = ty + bh_sm + 10.0f;

  // Global paused banner when master Extensions switch is off
  if (!ui->extensions_on) {
    const char *pmsg = "All extensions are paused — toggle Extensions above to resume.";
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, tk.fs_caption, COL(status_idle));
    float ph = fmaxf(16.0f, ccord_text_measure(r, pmsg, strlen(pmsg), &ps, fmaxf(w - 24.0f, 60.0f)).y) + 14.0f;
    ccord_rect pb = {x, body_top, w, ph};
    uis_plate(ui, r, pb, tk.r_control, with_alpha(COL(status_idle), 28));
    ccord_draw_rect(r, (ccord_rect){pb.x, pb.y, 3.0f, pb.h}, COL(status_idle));
    ccord_text_draw(r, pmsg, strlen(pmsg), pb.x + 12.0f, pb.y + 7.0f, &ps, fmaxf(w - 24.0f, 60.0f));
    body_top += ph + 8.0f;
  }

  s_body_top_cached = body_top;
  float body_bottom = g->content.y + g->content.h;
  float view_h = fmaxf(body_bottom - body_top, 40.0f);
  s_page_view_h = view_h;
  ccord_rect body_clip = {x - 2.0f, body_top - 2.0f, w + 4.0f, view_h + 4.0f};
  float max_s = st_s_plugins_content_h > view_h ? (st_s_plugins_content_h - view_h) : 0.0f;
  if (st_s_page_scroll < 0.0f) st_s_page_scroll = 0.0f;
  if (st_s_page_scroll > max_s) st_s_page_scroll = max_s;
  float y = body_top - st_s_page_scroll;

  if (s_log_id[0]) {
    s_page_sb_track = (ccord_rect){0};
    draw_debug_log_view(ui, r, x, body_top, w, body_bottom);
    return;
  }
  s_log_sb_track = (ccord_rect){0};
  if (s_detail_id[0]) {
    draw_detail_view(ui, r, x, y, w, body_clip);
  } else {
    ccord_push_clip(r, body_clip);
    if (s_subtab == 1) draw_discover_tab(ui, r, x, y, w, body_clip);
    else if (s_subtab == 2) draw_updates_tab(ui, r, x, y, w, body_clip);
    else draw_installed_tab(ui, r, x, y, w, body_clip);
    ccord_pop_clip(r);
  }

  // Visible scrollbar in the right margin whenever content exceeds the viewport (tabs + Detail view!)
  float sb_x = x + w + 8.0f;
  float max_sb_x = g->panel.x + g->panel.w - 10.0f;
  if (sb_x > max_sb_x) sb_x = max_sb_x;
  draw_simple_scrollbar(ui, r, (ccord_rect){sb_x, body_top + 2.0f, 6.0f, view_h - 4.0f},
                        st_s_page_scroll, view_h, st_s_plugins_content_h, &s_page_sb_track);
}

// ---------------------------------------------------------------- Overlays (Sort Popover, Overflow/Sheet, Protected Security Modal, Uninstall Modal)

static void draw_sort_popover(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  uis_tokens tk = uis_tokens_of(ui);
  float pw = 188.0f;
  float row_h = tk.menu_row_h;
  float pad = tk.menu_pad;
  float ph = pad * 2.0f + 5.0f * row_h;
  float px = s_sort_anchor.x + s_sort_anchor.w - pw;
  float py = s_sort_anchor.y + s_sort_anchor.h + 4.0f;
  if (px < g->panel.x + 8.0f) px = g->panel.x + 8.0f;
  if (px + pw > g->panel.x + g->panel.w - 8.0f) px = g->panel.x + g->panel.w - pw - 8.0f;
  if (py + ph > g->panel.y + g->panel.h - 8.0f) py = s_sort_anchor.y - ph - 4.0f;

  s_sort_box = (ccord_rect){px, py, pw, ph};
  uis_popover_frame(ui, r, s_sort_box);
  for (int i = 0; i < 5; i++) {
    ccord_rect row = {px + pad, py + pad + (float)i * row_h, pw - pad * 2.0f, row_h};
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, row)};
    uis_menu_row(ui, r, row, sort_label(i), st, false, s_disc_sort_idx == i, false, "psortm", (uint64_t)i);
    hit_overlay(row, ACT_DISC_SORT_PICK, i, "");
  }
}

static void draw_overflow_menu(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  const ccord_plugin_info *p = find_plugin_info(ui->d.contrib, s_overflow_id);
  if (!p) { s_overflow_id[0] = '\0'; return; }

  uis_tokens tk = uis_tokens_of(ui);
  bool has_cfg = ui->d.contrib && ccord_contrib_plugin_settings(ui->d.contrib, p->id) != nullptr;
  size_t ecnt = ui->d.contrib ? ccord_contrib_plugin_error_count(ui->d.contrib, p->id) : 0;
  char log_lbl[48];
  if (ecnt > 0) snprintf(log_lbl, sizeof log_lbl, "Debug Log (%zu)", ecnt);
  else snprintf(log_lbl, sizeof log_lbl, "Debug Log");

  struct { const char *lbl; phit_act act; bool checked; bool danger; bool show; bool sep_before; } items[7] = {
    {"Details & Permissions", ACT_OV_DETAILS,      false,          false, true,           false},
    {"Configure Settings",    ACT_OV_CONFIGURE,    false,          false, has_cfg,        false},
    {"Auto-Update",           ACT_OV_AUTO_UPDATE,  p->auto_update, false, !p->is_bundled, false},
    {"Reload Plugin",         ACT_OV_RELOAD,       false,          false, true,           false},
    {log_lbl,                 ACT_OV_LOG,          false,          false, true,           false},
    {"Export .cce Package",   ACT_OV_EXPORT,       false,          false, true,           false},
    {"Uninstall...",          ACT_OV_UNINSTALL_ASK,false,          true,  !p->is_bundled, true},
  };
  int vis = 0;
  bool has_sep = false;
  for (int i = 0; i < 7; i++) {
    if (!items[i].show) continue;
    vis++;
    if (items[i].sep_before) has_sep = true;
  }

  if (ui->mobile_layout) {
    // Mobile Slide-Up Bottom Sheet
    float row_h = 44.0f;
    float can_h = pl_btn(ui);
    float sh = 74.0f + (float)vis * row_h + (has_sep ? 10.0f : 0.0f) + can_h + 24.0f;
    float max_h = g->panel.h - 48.0f;
    if (sh > max_h) sh = max_h;
    float sx = g->panel.x + 8.0f;
    float sw = g->panel.w - 16.0f;
    float sy = g->panel.y + g->panel.h - sh - 8.0f;
    s_overflow_box = (ccord_rect){sx, sy, sw, sh};
    uis_modal_frame(ui, r, s_overflow_box);

    // Drag handle pill + plugin header
    ccord_rect handle = {sx + (sw - 36.0f) * 0.5f, sy + 8.0f, 36.0f, 4.0f};
    uis_plate(ui, r, handle, handle.h * 0.5f, COL(border_subtle));
    draw_plugin_icon(ui, r, (ccord_rect){sx + 16.0f, sy + 20.0f, 36.0f, 36.0f}, p->id, p->name, p->icon_path);
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
    ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
    ccord_text_draw_ellipsis(r, p->name, strlen(p->name), sx + 62.0f, sy + 21.0f, sw - 78.0f, &ns);
    ccord_text_draw_ellipsis(r, p->id, strlen(p->id), sx + 62.0f, sy + 41.0f, sw - 78.0f, &cs);
    uis_divider(ui, r, sx + 12.0f, sy + 64.0f, sw - 24.0f);

    float ry = sy + 70.0f;
    for (int i = 0; i < 7; i++) {
      if (!items[i].show) continue;
      if (items[i].sep_before) {
        uis_divider(ui, r, sx + 12.0f, ry + 4.0f, sw - 24.0f);
        ry += 10.0f;
      }
      ccord_rect row = {sx + 10.0f, ry, sw - 20.0f, row_h};
      uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, row)};
      uis_menu_row(ui, r, row, items[i].lbl, st, false, items[i].checked, items[i].danger, "pmobs", (uint64_t)i);
      hit_overlay(row, items[i].act, 0, p->id);
      ry += row_h;
    }
    ccord_rect rc_cancel = {sx + 16.0f, sy + sh - can_h - 12.0f, sw - 32.0f, can_h};
    draw_overlay_btn(ui, r, rc_cancel, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_MD, false, "pmobs", 99);
    hit_overlay(rc_cancel, ACT_OV_CLOSE, 0, "");
    return;
  }

  // Desktop Popover Menu
  float pw = 212.0f;
  float row_h = tk.menu_row_h;
  float pad = tk.menu_pad;
  float ph = pad * 2.0f + (float)vis * row_h + (has_sep ? 9.0f : 0.0f);
  float px = s_overflow_anchor.x + s_overflow_anchor.w - pw;
  float py = s_overflow_anchor.y + s_overflow_anchor.h + 4.0f;
  if (px < g->panel.x + 8.0f) px = g->panel.x + 8.0f;
  if (px + pw > g->panel.x + g->panel.w - 8.0f) px = g->panel.x + g->panel.w - pw - 8.0f;
  if (py + ph > g->panel.y + g->panel.h - 8.0f) py = s_overflow_anchor.y - ph - 4.0f;
  if (py < g->panel.y + 8.0f) py = g->panel.y + 8.0f;

  s_overflow_box = (ccord_rect){px, py, pw, ph};
  uis_popover_frame(ui, r, s_overflow_box);
  float ry = py + pad;
  for (int i = 0; i < 7; i++) {
    if (!items[i].show) continue;
    if (items[i].sep_before) {
      uis_divider(ui, r, px + pad, ry + 4.0f, pw - pad * 2.0f);
      ry += 9.0f;
    }
    ccord_rect row = {px + pad, ry, pw - pad * 2.0f, row_h};
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, row)};
    uis_menu_row(ui, r, row, items[i].lbl, st, false, items[i].checked, items[i].danger, "povm", (uint64_t)i);
    hit_overlay(row, items[i].act, 0, p->id);
    ry += row_h;
  }
}

static void draw_protected_security_modal(ccord_ui *ui, ccord_renderer *r)
{
  // TOCTOU re-validation for local consent mode
  if (s_sec_mode == SEC_CONSENT) {
    const ccord_plugin_info *p = find_plugin_info(ui->d.contrib, s_sec_id);
    if (!p || p->perms_mask != s_sec_perms) {
      if (p) snprintf(s_status_msg, sizeof s_status_msg, "Permissions changed for %s; review again before enabling.", p->name);
      close_security_modal();
      return;
    }
  }

  // Protected security surface: ALWAYS render with unmutated protected_theme!
  const ccord_theme *saved_theme = ui->theme;
  if (ui->d.protected_theme) ui->theme = ui->d.protected_theme;

  uis_tokens tk = uis_tokens_of(ui);
  uint8_t api_cls = (uint8_t)(ccord_perm_max_class(s_sec_perms) + 1);
  bool is_class3 = api_cls >= 3;
  ccord_color accent = is_class3 ? COL(danger) : COL(status_idle);

  float mw = fminf(560.0f, ui->win_w - 24.0f);
  if (mw < 280.0f) mw = ui->win_w - 12.0f;
  float max_mh = fminf(640.0f, ui->win_h - 32.0f);
  if (max_mh < 260.0f) max_mh = ui->win_h - 16.0f;

  // Measure scrollable body content height (reserve 12px right gutter when scrolling so scrollbar never touches cards)
  float inner_w = fmaxf(mw - 40.0f, 120.0f);
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  ccord_text_style cs = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, COL(text_muted));
  char wfull[704] = "";
  if (s_sec_warn_sev != CCORD_WARN_NONE && s_sec_warn_title[0]) {
    snprintf(wfull, sizeof wfull, "Moderator Warning: %s%s%s", s_sec_warn_title,
             s_sec_warn_msg[0] ? " — " : "", s_sec_warn_msg);
  }
  float fixed_chrome = 128.0f + 64.0f; // top banner + identity + sticky footer
  float warn_h = 0.0f, cl_h = 0.0f, body_content = 0.0f;
  for (int pass = 0; pass < 2; pass++) {
    body_content = 0.0f;
    warn_h = 0.0f;
    if (wfull[0]) {
      warn_h = fmaxf(18.0f, ccord_text_measure(r, wfull, strlen(wfull), &ms, fmaxf(inner_w - 22.0f, 60.0f)).y) + 18.0f;
      body_content += warn_h + 10.0f;
    }
    body_content += draw_permissions_section(ui, r, 0.0f, 0.0f, inner_w, s_sec_perms, s_sec_old_perms,
                                             s_sec_mode == SEC_UPDATE, s_sec_allowed_hosts,
                                             s_sec_old_allowed_hosts, s_sec_has_old_hosts, false);
    cl_h = 0.0f;
    if (s_sec_mode == SEC_UPDATE && s_sec_changelog[0]) {
      cl_h = fmaxf(18.0f, ccord_text_measure(r, s_sec_changelog, strlen(s_sec_changelog), &ms, inner_w).y);
      body_content += 24.0f + cl_h + 8.0f;
    }
    const char *pnote_m = "Protected C-Cord security screen · Plugin themes & overlays disabled · Undeclared Class 2/3 calls terminate the host.";
    body_content += fmaxf(18.0f, ccord_text_measure(r, pnote_m, strlen(pnote_m), &cs, inner_w).y) + 8.0f;
    if (pass == 0 && fixed_chrome + body_content + 16.0f > max_mh) {
      inner_w = fmaxf(mw - 52.0f, 108.0f);
    } else {
      break;
    }
  }
  s_sec_content_h = body_content;

  float mh = fminf(max_mh, fmaxf(310.0f, fixed_chrome + body_content + 16.0f));
  ccord_rect box = uis_modal_box(ui, mw, mh);
  s_sec_modal_box = box;
  uis_modal_frame(ui, r, box);

  // Top color-coded security class header strip
  ccord_rect banner = {box.x, box.y, box.w, 40.0f};
  uis_plate(ui, r, banner, tk.r_surface, with_alpha(accent, 34));
  ccord_draw_rect(r, (ccord_rect){banner.x, banner.y + banner.h - 2.0f, banner.w, 2.0f}, accent);
  bool narrow_sec = box.w < 430.0f;
  const char *btitle = is_class3
                         ? (s_sec_mode == SEC_UPDATE
                              ? (narrow_sec ? "API CLASS 3 · Updated Permissions"
                                            : "API CLASS 3 · Review Updated System & Network Permissions")
                              : (narrow_sec ? "API CLASS 3 · System Permissions"
                                            : "API CLASS 3 · System & Network Permissions Requested"))
                         : (s_sec_mode == SEC_UPDATE
                              ? (narrow_sec ? "API CLASS 2 · Updated Permissions"
                                            : "API CLASS 2 · Review Updated Internal Permissions")
                              : (narrow_sec ? "API CLASS 2 · Internal Permissions"
                                            : "API CLASS 2 · C-Cord Internal Permissions Requested"));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, accent);
  ccord_text_draw_ellipsis(r, btitle, strlen(btitle), box.x + 20.0f, box.y + 11.0f, box.w - 40.0f, &bs);

  // Plugin identity row
  float iy = box.y + 52.0f;
  draw_plugin_icon(ui, r, (ccord_rect){box.x + 20.0f, iy, 40.0f, 40.0f}, s_sec_id, s_sec_name, s_sec_icon_path);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk.fs_body, COL(text_header));
  char title_l[140];
  if (s_sec_mode == SEC_UPDATE && s_sec_ver_old[0]) {
    snprintf(title_l, sizeof title_l, "%s  v%s -> v%s", s_sec_name, s_sec_ver_old, s_sec_ver_new);
  } else {
    snprintf(title_l, sizeof title_l, "%s  v%s", s_sec_name, s_sec_ver_new);
  }
  ccord_text_draw_ellipsis(r, title_l, strlen(title_l), box.x + 72.0f, iy + 1.0f, box.w - 92.0f, &ns);
  float px = box.x + 72.0f;
  px += draw_pill(ui, r, px, iy + 22.0f, tier_label(s_sec_tier, false), tier_color(ui, s_sec_tier, false)) + 8.0f;
  char sub_id[160];
  if (s_sec_author[0]) snprintf(sub_id, sizeof sub_id, "by @%s · %s", s_sec_author, s_sec_id);
  else snprintf(sub_id, sizeof sub_id, "%s", s_sec_id);
  ccord_text_draw_ellipsis(r, sub_id, strlen(sub_id), px, iy + 24.0f, fmaxf(box.x + box.w - 20.0f - px, 40.0f), &cs);

  uis_divider(ui, r, box.x + 20.0f, box.y + 104.0f, box.w - 40.0f);

  // Scrollable body viewport
  float body_y = box.y + 114.0f;
  float bh = pl_btn(ui);
  float footer_y = box.y + box.h - (ui->mobile_layout ? 68.0f : 60.0f);
  float body_h = fmaxf(footer_y - body_y - 8.0f, 60.0f);
  ccord_rect body_vp = {box.x + 16.0f, body_y, box.w - 32.0f, body_h};
  s_sec_body_rect = body_vp;
  float max_s = s_sec_content_h > body_h ? (s_sec_content_h - body_h) : 0.0f;
  if (s_sec_scroll < 0.0f) s_sec_scroll = 0.0f;
  if (s_sec_scroll > max_s) s_sec_scroll = max_s;

  ccord_push_clip(r, body_vp);
  float cy = body_y - s_sec_scroll;
  if (warn_h > 0.0f) {
    ccord_color wc = s_sec_warn_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
    uis_plate(ui, r, (ccord_rect){box.x + 20.0f, cy, inner_w, warn_h}, tk.r_control, with_alpha(wc, 28));
    ccord_draw_rect(r, (ccord_rect){box.x + 20.0f, cy, 3.0f, warn_h}, wc);
    ccord_text_style ws = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, wc);
    ccord_text_draw(r, wfull, strlen(wfull), box.x + 32.0f, cy + 9.0f, &ws, fmaxf(inner_w - 22.0f, 60.0f));
    cy += warn_h + 10.0f;
  }

  cy += draw_permissions_section(ui, r, box.x + 20.0f, cy, inner_w, s_sec_perms, s_sec_old_perms,
                                 s_sec_mode == SEC_UPDATE, s_sec_allowed_hosts,
                                 s_sec_old_allowed_hosts, s_sec_has_old_hosts, true);

  if (cl_h > 0.0f) {
    ccord_text_style ovs = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
    ccord_text_draw(r, "WHAT CHANGED", 12, box.x + 20.0f, cy + 4.0f, &ovs, 0.0f);
    ccord_text_draw(r, s_sec_changelog, strlen(s_sec_changelog), box.x + 20.0f, cy + 22.0f, &ms, inner_w);
    cy += 24.0f + cl_h + 8.0f;
  }

  const char *pnote = "Protected C-Cord security screen · Plugin themes & overlays disabled · Undeclared Class 2/3 calls terminate the host.";
  ccord_text_draw(r, pnote, strlen(pnote), box.x + 20.0f, cy + 2.0f, &cs, inner_w);
  ccord_pop_clip(r);

  // Visible scrollbar inside the security modal whenever permissions overflow!
  draw_simple_scrollbar(ui, r, (ccord_rect){box.x + box.w - 14.0f, body_y + 2.0f, 6.0f, body_h - 4.0f},
                        s_sec_scroll, body_h, s_sec_content_h, &s_sec_sb_track);

  // Sticky footer (always visible!)
  uis_divider(ui, r, box.x + 20.0f, footer_y, box.w - 40.0f);
  const char *ok_lbl = s_sec_mode == SEC_CONSENT ? "Approve & Enable"
                     : s_sec_mode == SEC_UPDATE  ? "Approve & Update"
                                                 : "Approve & Install";
  float ok_w = btn_w_sm(ui, r, ok_lbl, box.w * 0.56f);
  float can_w = btn_w_sm(ui, r, "Cancel", 104.0f);
  ccord_rect r_ok = {box.x + box.w - 20.0f - ok_w, footer_y + 14.0f, ok_w, bh};
  ccord_rect r_can = {r_ok.x - 10.0f - can_w, footer_y + 14.0f, can_w, bh};
  draw_overlay_btn(ui, r, r_can, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_MD, false, "psec", 1);
  draw_overlay_btn(ui, r, r_ok, ok_lbl, is_class3 ? UIS_BTN_DANGER : UIS_BTN_PRIMARY, UIS_BTN_S_MD, false, "psec", 2);
  hit_overlay(r_can, ACT_SEC_CANCEL, 0, "");
  hit_overlay(r_ok, ACT_SEC_APPROVE, 0, s_sec_id);

  ui->theme = saved_theme;
}

static void draw_uninstall_modal(ccord_ui *ui, ccord_renderer *r)
{
  uis_tokens tk = uis_tokens_of(ui);
  float mw = fminf(UIS_MODAL_W_SM, ui->win_w - 28.0f);
  float bh = pl_btn(ui);
  ccord_rect box = uis_modal_box(ui, mw, ui->mobile_layout ? 196.0f : 184.0f);
  s_uninst_box = box;
  uis_modal_frame(ui, r, box);

  char title[128];
  snprintf(title, sizeof title, "Uninstall %s?", s_uninst_name[0] ? s_uninst_name : s_uninst_id);
  float by = uis_modal_title(ui, r, box, title);
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_muted));
  const char *msg = "This stops the extension immediately and removes its installed files from C-Cord.";
  ccord_text_draw(r, msg, strlen(msg), box.x + UIS_SP_5, by + 4.0f, &ms, box.w - UIS_SP_5 * 2.0f);

  float u_w = btn_w_sm(ui, r, "Uninstall", 124.0f);
  float c_w = btn_w_sm(ui, r, "Cancel", 96.0f);
  ccord_rect r_del = {box.x + box.w - UIS_SP_5 - u_w, box.y + box.h - UIS_SP_5 - bh, u_w, bh};
  ccord_rect r_can = {r_del.x - UIS_SP_2 - c_w, r_del.y, c_w, bh};
  draw_overlay_btn(ui, r, r_can, "Cancel", UIS_BTN_SECONDARY, UIS_BTN_S_MD, false, "puni", 1);
  draw_overlay_btn(ui, r, r_del, "Uninstall", UIS_BTN_DANGER, UIS_BTN_S_MD, false, "puni", 2);
  hit_overlay(r_can, ACT_UNINST_CANCEL, 0, "");
  hit_overlay(r_del, ACT_UNINST_CONFIRM, 0, s_uninst_id);
}

void st_draw_plugins_overlays(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  s_nhits_overlay = 0;
  if (s_sort_open) draw_sort_popover(ui, r, g);
  else if (s_overflow_id[0]) draw_overflow_menu(ui, r, g);
  else if (s_sec_mode != SEC_NONE) draw_protected_security_modal(ui, r);
  else if (s_uninst_id[0]) draw_uninstall_modal(ui, r);
}

// ---------------------------------------------------------------- Action dispatcher & input handlers

static void ask_uninstall(const ccord_ui *ui, const char *id)
{
  const ccord_plugin_info *p = find_plugin_info(ui ? ui->d.contrib : nullptr, id);
  copy_str(s_uninst_id, sizeof s_uninst_id, id);
  copy_str(s_uninst_name, sizeof s_uninst_name, (p && p->name[0]) ? p->name : id);
  s_overflow_id[0] = '\0';
}

static bool dispatch_action(ccord_ui *ui, const phit_item *it)
{
  if (!ui || !it) return false;
  ccord_contrib *c = ui->d.contrib;
  switch (it->act) {
    case ACT_TAB:
      s_subtab = it->arg;
      s_inst_search_focused = false;
      s_disc_search_focused = false;
      if (ui->touch_mode) ui->touch_text_focused = false;
      s_detail_id[0] = '\0';
      s_log_id[0] = '\0';
      s_overflow_id[0] = '\0';
      s_sort_open = false;
      st_s_page_scroll = 0.0f;
      if (s_subtab == 1 && c) {
        const ccord_catalog_status *cst = ccord_contrib_catalog_status(c);
        if (cst && !cst->has_loaded && !cst->syncing) trigger_discover_query(ui, false);
      } else if (s_subtab == 2 && c) {
        ccord_contrib_catalog_check_updates(c);
      }
      return true;
    case ACT_SYNC:
      if (c) ccord_contrib_catalog_refresh(c);
      return true;
    case ACT_INST_SEARCH_FOCUS:
      s_inst_search_focused = true;
      s_disc_search_focused = false;
      if (ui->touch_mode) ui->touch_text_focused = true;
      return true;
    case ACT_INST_SEARCH_CLEAR:
      s_inst_search[0] = '\0';
      st_s_page_scroll = 0.0f;
      return true;
    case ACT_INST_FILTER:
      s_inst_filter = (inst_filter)it->arg;
      st_s_page_scroll = 0.0f;
      return true;
    case ACT_DISC_SEARCH_FOCUS:
      s_disc_search_focused = true;
      s_inst_search_focused = false;
      if (ui->touch_mode) ui->touch_text_focused = true;
      return true;
    case ACT_DISC_SEARCH_CLEAR:
      s_disc_search[0] = '\0';
      trigger_discover_query(ui, false);
      return true;
    case ACT_DISC_SEARCH_GO:
      s_disc_search_focused = false;
      if (ui->touch_mode) ui->touch_text_focused = false;
      trigger_discover_query(ui, false);
      return true;
    case ACT_DISC_SORT_TOGGLE:
      s_sort_open = !s_sort_open;
      return true;
    case ACT_DISC_SORT_PICK:
      s_disc_sort_idx = it->arg;
      s_sort_open = false;
      trigger_discover_query(ui, false);
      return true;
    case ACT_DISC_TIER:
      s_disc_tier_filter = it->arg;
      st_s_page_scroll = 0.0f;
      trigger_discover_query(ui, false);
      return true;
    case ACT_DISC_CLASS1:
      s_disc_class1_only = !s_disc_class1_only;
      st_s_page_scroll = 0.0f;
      trigger_discover_query(ui, false);
      return true;
    case ACT_DISC_PLAT:
      s_disc_plat_filter = (plat_filter)it->arg;
      st_s_page_scroll = 0.0f;
      trigger_discover_query(ui, false);
      return true;
    case ACT_DISC_LOAD_MORE:
      trigger_discover_query(ui, true);
      return true;
    case ACT_CARD_TOGGLE: {
      const ccord_plugin_info *p = find_plugin_info(c, it->id);
      if (p && !p->revoked) {
        if (p->state == CCORD_PLUGIN_NEEDS_CONSENT) {
          open_sec_modal_for_installed_consent(p);
        } else {
          bool on = !(p->state == CCORD_PLUGIN_READY || p->state == CCORD_PLUGIN_STARTING);
          ccord_contrib_plugin_set_enabled(c, p->id, on);
        }
      }
      return true;
    }
    case ACT_CARD_OVERFLOW:
      if (strcmp(s_overflow_id, it->id) == 0) s_overflow_id[0] = '\0';
      else {
        copy_str(s_overflow_id, sizeof s_overflow_id, it->id);
        s_overflow_anchor = it->rc;
      }
      return true;
    case ACT_CARD_CONFIGURE:
    case ACT_OV_CONFIGURE:
      s_overflow_id[0] = '\0';
      ui_plugin_settings_open(ui, it->id);
      return true;
    case ACT_CARD_DETAILS:
    case ACT_OV_DETAILS:
    case ACT_CAT_DETAILS:
      s_overflow_id[0] = '\0';
      copy_str(s_detail_id, sizeof s_detail_id, it->id);
      st_s_page_scroll = 0.0f;
      if (c) ccord_contrib_catalog_fetch_detail(c, it->id);
      return true;
    case ACT_CARD_RELOAD:
    case ACT_OV_RELOAD:
      s_overflow_id[0] = '\0';
      if (c) ccord_contrib_plugin_reload(c, it->id);
      return true;
    case ACT_CARD_LOG:
    case ACT_OV_LOG:
      s_overflow_id[0] = '\0';
      copy_str(s_log_id, sizeof s_log_id, it->id);
      s_log_level_filter = 0;
      s_log_scroll = 0.0f;
      return true;
    case ACT_CARD_REVIEW_CONSENT: {
      const ccord_plugin_info *p = find_plugin_info(c, it->id);
      if (p) open_sec_modal_for_installed_consent(p);
      return true;
    }
    case ACT_CARD_UPDATE_SAFE:
      if (c) ccord_contrib_catalog_install(c, it->id);
      return true;
    case ACT_CARD_UPDATE_REVIEW: {
      const ccord_plugin_info *p = find_plugin_info(c, it->id);
      if (p) open_sec_modal_for_installed_update(ui, p);
      return true;
    }
    case ACT_CARD_UNINSTALL_ASK:
    case ACT_OV_UNINSTALL_ASK:
      ask_uninstall(ui, it->id);
      return true;
    case ACT_CAT_INSTALL:
      request_catalog_install_or_review(ui, it->id);
      return true;
    case ACT_UPD_RUN_SAFE:
      if (c) ccord_contrib_catalog_run_auto_updates(c);
      return true;
    case ACT_UPD_CHECK:
      if (c) ccord_contrib_catalog_check_updates(c);
      return true;
    case ACT_UPD_POLICY_TOGGLE: {
      const ccord_catalog_prefs *cur = ccord_contrib_catalog_prefs(c);
      ccord_catalog_prefs pr = cur ? *cur : (ccord_catalog_prefs){.auto_update_enabled = false, .auto_update_min_tier = 2};
      pr.auto_update_enabled = !pr.auto_update_enabled;
      ccord_contrib_catalog_set_prefs(c, &pr);
      return true;
    }
    case ACT_UPD_POLICY_EXPAND:
      s_policy_expanded = !s_policy_expanded;
      return true;
    case ACT_UPD_POLICY_TIER: {
      const ccord_catalog_prefs *cur = ccord_contrib_catalog_prefs(c);
      ccord_catalog_prefs pr = cur ? *cur : (ccord_catalog_prefs){.auto_update_enabled = false, .auto_update_min_tier = 2};
      pr.auto_update_min_tier = (uint8_t)it->arg;
      ccord_contrib_catalog_set_prefs(c, &pr);
      return true;
    }
    case ACT_DETAIL_BACK:
      s_detail_id[0] = '\0';
      st_s_page_scroll = 0.0f;
      return true;
    case ACT_DETAIL_RETRY:
      if (c && it->id[0]) ccord_contrib_catalog_fetch_detail(c, it->id);
      return true;
    case ACT_LOG_BACK:
      s_log_id[0] = '\0';
      s_log_scroll = 0.0f;
      return true;
    case ACT_LOG_RELOAD:
      if (c && it->id[0]) ccord_contrib_plugin_reload(c, it->id);
      return true;
    case ACT_LOG_CLEAR:
      if (c && it->id[0]) ccord_contrib_clear_plugin_log(c, it->id);
      s_log_scroll = 0.0f;
      return true;
    case ACT_LOG_LEVEL:
      s_log_level_filter = it->arg;
      s_log_scroll = 0.0f;
      return true;
    case ACT_OV_AUTO_UPDATE: {
      const ccord_plugin_info *p = find_plugin_info(c, it->id);
      if (p) ccord_contrib_catalog_set_plugin_auto_update(c, p->id, !p->auto_update);
      s_overflow_id[0] = '\0';
      return true;
    }
    case ACT_OV_EXPORT: {
      const ccord_plugin_info *p = find_plugin_info(c, it->id);
      s_overflow_id[0] = '\0';
      if (p && c) {
        const char *home = getenv("HOME");
        char dest[512], ebuf[128] = "";
        snprintf(dest, sizeof dest, "%s/%s-%s.cce", (home && home[0]) ? home : ".", p->id, p->version);
        if (ccord_contrib_plugin_export_cce(c, p->id, dest, ebuf, sizeof ebuf)) {
          snprintf(s_status_msg, sizeof s_status_msg, "Exported: %.175s", dest);
        } else {
          snprintf(s_status_msg, sizeof s_status_msg, "Export failed: %.170s", ebuf);
        }
      }
      return true;
    }
    case ACT_OV_CLOSE:
      s_overflow_id[0] = '\0';
      return true;
    case ACT_SEC_CANCEL:
      close_security_modal();
      return true;
    case ACT_SEC_APPROVE: {
      char id_copy[65];
      copy_str(id_copy, sizeof id_copy, s_sec_id);
      uint64_t approved = s_sec_perms;
      sec_modal_mode mode = s_sec_mode;
      close_security_modal();
      if (c && id_copy[0]) {
        if (mode == SEC_CONSENT) {
          ccord_contrib_plugin_consent_accept_for(c, id_copy, approved);
        } else {
          ccord_contrib_catalog_install_approved(c, id_copy, approved);
        }
      }
      return true;
    }
    case ACT_UNINST_CANCEL:
      s_uninst_id[0] = '\0';
      return true;
    case ACT_UNINST_CONFIRM: {
      char id_copy[65], name_copy[65], ebuf[128] = "";
      copy_str(id_copy, sizeof id_copy, s_uninst_id);
      copy_str(name_copy, sizeof name_copy, s_uninst_name);
      s_uninst_id[0] = '\0';
      if (c && id_copy[0]) {
        if (ccord_contrib_plugin_uninstall(c, id_copy, ebuf, sizeof ebuf)) {
          snprintf(s_status_msg, sizeof s_status_msg, "Uninstalled %s", name_copy[0] ? name_copy : id_copy);
          if (strcmp(s_detail_id, id_copy) == 0) s_detail_id[0] = '\0';
          if (strcmp(s_log_id, id_copy) == 0) s_log_id[0] = '\0';
        } else {
          snprintf(s_status_msg, sizeof s_status_msg, "Uninstall failed: %.150s", ebuf);
        }
      }
      return true;
    }
    default:
      return false;
  }
}

bool st_plugins_handle_click(ccord_ui *ui, const sgeom *g, float x, float y)
{
  if (!ui) return false;
  // 1. Active overlay captures every click (hitting inside runs action; outside dismisses overlay!)
  if (st_plugins_has_overlay()) {
    if (s_sec_mode != SEC_NONE &&
        click_simple_scrollbar(ui, x, y, s_sec_sb_track, &s_sec_scroll, s_sec_body_rect.h, s_sec_content_h)) {
      return true;
    }
    if (s_hits_overlay) {
      for (size_t i = s_nhits_overlay; i > 0; i--) {
        if (ui_in_rect(x, y, s_hits_overlay[i - 1].rc)) {
          return dispatch_action(ui, &s_hits_overlay[i - 1]);
        }
      }
    }
    if (s_uninst_id[0] && !ui_in_rect(x, y, s_uninst_box)) { s_uninst_id[0] = '\0'; return true; }
    if (s_sort_open && !ui_in_rect(x, y, s_sort_box)) { s_sort_open = false; return true; }
    if (s_overflow_id[0] && !ui_in_rect(x, y, s_overflow_box)) { s_overflow_id[0] = '\0'; return true; }
    if (s_sec_mode != SEC_NONE && !ui_in_rect(x, y, s_sec_modal_box)) { close_security_modal(); return true; }
    return true;
  }

  // 1b. Click on visible Debug Log or Main Plugins scrollbar track
  if (s_log_id[0] &&
      click_simple_scrollbar(ui, x, y, s_log_sb_track, &s_log_scroll, s_log_box_rect.h, s_log_content_h)) {
    return true;
  }
  if (!s_log_id[0] &&
      click_simple_scrollbar(ui, x, y, s_page_sb_track, &st_s_page_scroll, s_page_view_h, st_s_plugins_content_h)) {
    return true;
  }

  // 2. Master Extensions toggle (works even before first draw for tests!)
  if (ui_in_rect(x, y, st_extensions_toggle_rect(g))) {
    st_set_extensions(ui, !ui->extensions_on);
    return true;
  }

  // 3. Recorded hit targets from last draw (reverse order so buttons win over full-card clicks)
  if (s_hits_main) {
    for (size_t i = s_nhits_main; i > 0; i--) {
      if (ui_in_rect(x, y, s_hits_main[i - 1].rc)) {
        phit_act a = s_hits_main[i - 1].act;
        if (a != ACT_INST_SEARCH_FOCUS && a != ACT_DISC_SEARCH_FOCUS) {
          s_inst_search_focused = false;
          s_disc_search_focused = false;
          if (ui->touch_mode) ui->touch_text_focused = false;
        }
        return dispatch_action(ui, &s_hits_main[i - 1]);
      }
    }
  }

  s_inst_search_focused = false;
  s_disc_search_focused = false;
  if (ui->touch_mode) ui->touch_text_focused = false;
  return false;
}

bool st_plugins_handle_wheel(ccord_ui *ui, const sgeom *g, const ccord_input_event *ev)
{
  (void)g;
  if (!ui || !ev) return false;
  if (s_sec_mode != SEC_NONE) {
    float max_s = s_sec_content_h > s_sec_body_rect.h ? (s_sec_content_h - s_sec_body_rect.h) : 0.0f;
    if (max_s > 0.0f) {
      s_sec_scroll -= ev->wheel * 48.0f;
      if (s_sec_scroll < 0.0f) s_sec_scroll = 0.0f;
      if (s_sec_scroll > max_s) s_sec_scroll = max_s;
      ui->dirty = true;
    }
    return true;
  }
  if (st_plugins_has_overlay()) return true; // swallow wheel behind modals/popovers
  if (s_log_id[0] && ui_in_rect(ev->x, ev->y, s_log_box_rect)) {
    float max_ls = s_log_content_h > s_log_box_rect.h ? (s_log_content_h - s_log_box_rect.h) : 0.0f;
    if (max_ls > 0.0f) {
      s_log_scroll -= ev->wheel * 44.0f;
      if (s_log_scroll < 0.0f) s_log_scroll = 0.0f;
      if (s_log_scroll > max_ls) s_log_scroll = max_ls;
      ui->dirty = true;
      return true;
    }
  }
  if (!s_log_id[0] && s_page_view_h > 0.0f) {
    float max_p = st_s_plugins_content_h > s_page_view_h ? (st_s_plugins_content_h - s_page_view_h) : 0.0f;
    if (max_p > 0.0f) {
      st_s_page_scroll -= ev->wheel * 56.0f;
      if (st_s_page_scroll < 0.0f) st_s_page_scroll = 0.0f;
      if (st_s_page_scroll > max_p) st_s_page_scroll = max_p;
      ui->dirty = true;
      return true;
    }
  }
  return false;
}

bool st_plugins_handle_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev) return false;
  if (st_plugins_has_overlay()) {
    if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_ESCAPE) {
      return ui_settings_plugins_dismiss_overlay(ui);
    }
    return true; // modal swallows keys
  }
  if (!ui_settings_plugins_wants_text(ui)) return false;

  char *buf = (s_subtab == 0) ? s_inst_search : s_disc_search;
  size_t cap = sizeof s_inst_search;
  if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
    size_t cur = strlen(buf);
    size_t add = strlen(ev->text);
    if (cur + add < cap) {
      memcpy(buf + cur, ev->text, add + 1);
      if (s_subtab == 0) st_s_page_scroll = 0.0f;
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_BACKSPACE) {
      size_t cur = strlen(buf);
      while (cur > 0 && ((unsigned char)buf[cur - 1] & 0xC0u) == 0x80u) cur--;
      if (cur > 0) buf[cur - 1] = '\0';
      if (s_subtab == 0) st_s_page_scroll = 0.0f;
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN) {
      if (s_subtab == 1) trigger_discover_query(ui, false);
      ui->dirty = true;
      return true;
    }
    if (ev->key == CCORD_KEY_ESCAPE) {
      s_inst_search_focused = false;
      s_disc_search_focused = false;
      if (ui->touch_mode) ui->touch_text_focused = false;
      ui->dirty = true;
      return true;
    }
  }
  return false;
}

int64_t st_plugins_hover_key(const ccord_ui *ui, const sgeom *g, float x, float y)
{
  (void)ui;
  if (st_plugins_has_overlay()) {
    if (s_hits_overlay) {
      for (size_t i = s_nhits_overlay; i > 0; i--) {
        if (ui_in_rect(x, y, s_hits_overlay[i - 1].rc)) return 9000 + (int64_t)i;
      }
    }
    return 8999;
  }
  if (ui_in_rect(x, y, st_extensions_toggle_rect(g))) return 600;
  if (s_hits_main) {
    for (size_t i = s_nhits_main; i > 0; i--) {
      if (ui_in_rect(x, y, s_hits_main[i - 1].rc)) return 601 + (int64_t)i;
    }
  }
  return 0;
}

static bool match_debug_item(const phit_item *it, const char *key)
{
  char buf[96];
  switch (it->act) {
    case ACT_TAB:                snprintf(buf, sizeof buf, "tab:%d", it->arg); break;
    case ACT_SYNC:               snprintf(buf, sizeof buf, "sync"); break;
    case ACT_DISC_SORT_TOGGLE:   snprintf(buf, sizeof buf, "sort"); break;
    case ACT_DISC_SORT_PICK:     snprintf(buf, sizeof buf, "sort:%d", it->arg); break;
    case ACT_CARD_OVERFLOW:      snprintf(buf, sizeof buf, "overflow:%s", it->id); break;
    case ACT_CARD_UPDATE_REVIEW: snprintf(buf, sizeof buf, "update_review:%s", it->id); break;
    case ACT_CARD_REVIEW_CONSENT:snprintf(buf, sizeof buf, "consent:%s", it->id); break;
    case ACT_CAT_INSTALL:        snprintf(buf, sizeof buf, "cat_install:%s", it->id); break;
    case ACT_CAT_DETAILS:        snprintf(buf, sizeof buf, "cat_card:%s", it->id); break;
    case ACT_OV_UNINSTALL_ASK:   snprintf(buf, sizeof buf, "ov_uninstall:%s", it->id); break;
    case ACT_SEC_APPROVE:        snprintf(buf, sizeof buf, "sec_approve"); break;
    case ACT_SEC_CANCEL:         snprintf(buf, sizeof buf, "sec_cancel"); break;
    case ACT_UNINST_CONFIRM:     snprintf(buf, sizeof buf, "uninst_confirm"); break;
    case ACT_UNINST_CANCEL:      snprintf(buf, sizeof buf, "uninst_cancel"); break;
    case ACT_UPD_POLICY_EXPAND:  snprintf(buf, sizeof buf, "policy_expand"); break;
    default: return false;
  }
  return strcmp(buf, key) == 0;
}

bool st_plugins_debug_find_rect(const char *key, ccord_rect *out)
{
  if (!key || !key[0]) return false;
  if (s_hits_overlay) {
    for (size_t i = s_nhits_overlay; i > 0; i--) {
      if (match_debug_item(&s_hits_overlay[i - 1], key)) {
        if (out) *out = s_hits_overlay[i - 1].rc;
        return true;
      }
    }
  }
  if (s_hits_main) {
    for (size_t i = s_nhits_main; i > 0; i--) {
      if (match_debug_item(&s_hits_main[i - 1], key)) {
        if (out) *out = s_hits_main[i - 1].rc;
        return true;
      }
    }
  }
  return false;
}
