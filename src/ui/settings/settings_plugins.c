// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_internal.h"

char st_s_plugin_log_id[65];
char st_s_plugin_consent_id[65];
uint64_t st_s_plugin_consent_perms;
float st_s_plugin_consent_box_h = 224.0f;

bool ui_settings_security_modal_open(const ccord_ui *ui)
{
  if (st_s_plugin_consent_id[0] != '\0') return true;
  if (ui && (ui->login_screen || ui->account_mfa_required)) return true;
  return false;
}
char st_s_plugin_detail_id[65];
char st_s_plugin_status_msg[192];
char st_s_plugin_search[96];
bool st_s_plugin_search_focused;
int st_s_plugin_subtab;       // 0=Installed, 1=Discover, 2=Updates
int st_s_plugin_tier_filter;  // 0=All, 1=Official, 2=Verified, 3=Unverified
int st_s_plugin_sort_idx;     // 0=popular, 1=trending, 2=tier, 3=updated, 4=name
bool st_s_plugin_class1_only;

static const char *plugin_sort_key(int idx)
{
  static const char *const k_keys[5] = {"popular", "trending", "tier", "updated", "name"};
  return (idx >= 0 && idx < 5) ? k_keys[idx] : "popular";
}

static const char *plugin_sort_label(int idx)
{
  static const char *const k_lbls[5] = {"Popular", "Trending", "Tier", "Updated", "A-Z"};
  return (idx >= 0 && idx < 5) ? k_lbls[idx] : "Popular";
}

void st_trigger_plugin_discover_query(ccord_ui *ui, bool append_next_page)
{
  if (!ui || !ui->d.contrib) return;
  ccord_contrib_catalog_query(ui->d.contrib, st_s_plugin_search, st_s_plugin_tier_filter,
                              st_s_plugin_class1_only ? 1 : 0, plugin_sort_key(st_s_plugin_sort_idx),
                              append_next_page);
}


static const char *plugin_tier_label(ccord_plugin_tier t)
{
  switch (t) {
    case CCORD_TIER_OFFICIAL:   return "[Official]";
    case CCORD_TIER_VERIFIED:   return "[Verified]";
    case CCORD_TIER_UNVERIFIED: return "[Unverified]";
    default:                    return "";
  }
}

static ccord_color plugin_tier_color(const ccord_ui *ui, ccord_plugin_tier t)
{
  switch (t) {
    case CCORD_TIER_OFFICIAL:   return COL(brand);
    case CCORD_TIER_VERIFIED:   return COL(status_online);
    default:                    return COL(text_muted);
  }
}

static const char *plugin_api_class_label(uint8_t api_class)
{
  if (api_class >= 3) return "[API Class 3]";
  if (api_class == 2) return "[API Class 2]";
  return "[API Class 1]";
}

static ccord_color plugin_api_class_color(const ccord_ui *ui, uint8_t api_class)
{
  if (api_class >= 3) return COL(danger);
  if (api_class == 2) return COL(status_idle);
  return COL(status_online);
}

void st_plugin_subtab_rects(const sgeom *g, ccord_rect *tab_inst, ccord_rect *tab_disc,
                                ccord_rect *tab_upd, ccord_rect *btn_sync)
{
  float x = g->content.x, y = g->content.y + 84.0f, w = g->content.w;
  *tab_inst = (ccord_rect){x, y, 118.0f, UIS_H_BTN_SM};
  *tab_disc = (ccord_rect){x + 126.0f, y, 108.0f, UIS_H_BTN_SM};
  *tab_upd  = (ccord_rect){x + 242.0f, y, 116.0f, UIS_H_BTN_SM};
  *btn_sync = (ccord_rect){x + w - 128.0f, y, 128.0f, UIS_H_BTN_SM};
}

float st_plugin_card_layout(const ccord_ui *ui, const sgeom *g, size_t idx, const ccord_plugin_info *p, float y,
                                plugin_card_rects *out)
{
  (void)idx;
  memset(out, 0, sizeof *out);
  bool has_err = p->error && p->error[0];
  bool has_warn = p->warning_sev != CCORD_WARN_NONE && p->warning_title && p->warning_title[0];
  bool has_upd = p->update_version && p->update_version[0];
  bool has_cfg = ui && ui->d.contrib && ccord_contrib_plugin_settings(ui->d.contrib, p->id) != nullptr;
  float h = 104.0f + (has_err ? 20.0f : 0.0f) + (has_warn ? 20.0f : 0.0f);
  float x = g->content.x, w = g->content.w;
  float by = y + h - 34.0f;
  out->card = (ccord_rect){x, y, w, h};
  out->btn_toggle = (ccord_rect){x + 14.0f, by, p->state == CCORD_PLUGIN_NEEDS_CONSENT ? 120.0f : 84.0f, UIS_H_BTN_SM};
  float bx = out->btn_toggle.x + out->btn_toggle.w + 6.0f;
  out->btn_log = (ccord_rect){bx, by, 108.0f, UIS_H_BTN_SM};
  bx += out->btn_log.w + 6.0f;
  out->btn_reload = (ccord_rect){bx, by, 78.0f, UIS_H_BTN_SM};
  bx += out->btn_reload.w + 6.0f;
  if (has_cfg) {
    out->btn_configure = (ccord_rect){bx, by, 86.0f, UIS_H_BTN_SM};
    bx += out->btn_configure.w + 6.0f;
  }
  out->btn_export = (ccord_rect){bx, by, 106.0f, UIS_H_BTN_SM};
  bx += out->btn_export.w + 6.0f;
  if (has_upd && bx + 106.0f <= x + w - 14.0f) {
    out->btn_update = (ccord_rect){bx, by, 106.0f, UIS_H_BTN_SM};
    bx += out->btn_update.w + 6.0f;
  }
  if (!p->is_bundled) {
    float rx = x + w - 14.0f;
    if (rx - 92.0f >= bx) {
      rx -= 92.0f;
      out->btn_uninstall = (ccord_rect){rx, by, 92.0f, UIS_H_BTN_SM};
      rx -= 6.0f;
    }
    if (rx - 94.0f >= bx) {
      rx -= 94.0f;
      out->btn_auto = (ccord_rect){rx, by, 94.0f, UIS_H_BTN_SM};
    }
  }
  return h;
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

void st_draw_plugins(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  ccord_rect r_inst, r_disc, r_upd, r_sync;
  st_plugin_subtab_rects(g, &r_inst, &r_disc, &r_upd, &r_sync);
  size_t inst_n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
  const ccord_catalog_status *cst = ccord_contrib_catalog_status(ui->d.contrib);
  size_t pend_upd = cst ? cst->pending_updates : 0;

  char l_inst[32], l_upd[32];
  snprintf(l_inst, sizeof l_inst, "Installed (%zu)", inst_n);
  if (pend_upd > 0) snprintf(l_upd, sizeof l_upd, "Updates (%zu)", pend_upd);
  else snprintf(l_upd, sizeof l_upd, "Updates");

  st_draw_button(ui, r, r_inst, l_inst, st_s_plugin_subtab == 0 && !st_s_plugin_detail_id[0]);
  st_draw_button(ui, r, r_disc, "Discover", st_s_plugin_subtab == 1 && !st_s_plugin_detail_id[0]);
  st_draw_button(ui, r, r_upd, l_upd, st_s_plugin_subtab == 2 && !st_s_plugin_detail_id[0]);
  st_draw_button(ui, r, r_sync, (cst && cst->syncing) ? "Syncing..." : "Sync Catalog", false);

  float x = g->content.x, y = g->content.y + 120.0f, w = g->content.w;
  ccord_text_style ms = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_body, COL(text_header));
  ccord_text_style vs = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));

  // Debug Log modal view
  if (st_s_plugin_log_id[0]) {
    const ccord_plugin_info *p = find_plugin_info(ui->d.contrib, st_s_plugin_log_id);
    if (!p) { st_s_plugin_log_id[0] = '\0'; }
    else {
      char hdr[128];
      snprintf(hdr, sizeof hdr, "Debug Log: %s (%s)", p->name, p->id);
      ccord_text_draw_ellipsis(r, hdr, strlen(hdr), x, y, w - 260.0f, &ns);
      st_draw_button(ui, r, (ccord_rect){x + w - 246.0f, y - 4.0f, 74.0f, UIS_H_BTN_SM}, "Reload", false);
      st_draw_button(ui, r, (ccord_rect){x + w - 164.0f, y - 4.0f, 74.0f, UIS_H_BTN_SM}, "Clear", false);
      st_draw_button(ui, r, (ccord_rect){x + w - 82.0f, y - 4.0f, 82.0f, UIS_H_BTN_SM}, "Back", true);
      y += 32.0f;
      ccord_rect box = {x, y, w, g->panel.y + g->panel.h - y - 16.0f};
      uis_card(ui, r, box);
      size_t lc = ccord_contrib_plugin_log_count(ui->d.contrib, p->id);
      if (lc == 0) {
        const char *empty = "No debug log entries recorded for this plugin yet.";
        ccord_text_draw(r, empty, strlen(empty), box.x + 14.0f, box.y + 14.0f, &ms, box.w - 28.0f);
      } else {
        float ly = box.y + 12.0f;
        size_t max_lines = (size_t)((box.h - 24.0f) / 18.0f);
        size_t start = lc > max_lines ? lc - max_lines : 0;
        for (size_t i = start; i < lc; i++) {
          const ccord_plugin_log_entry *e = ccord_contrib_plugin_log_at(ui->d.contrib, p->id, i);
          if (!e) continue;
          const char *tag = e->level == 3 ? "[ERR] " : e->level == 2 ? "[WARN] " : e->level == 0 ? "[DBG] " : "[INFO] ";
          ccord_color col = e->level == 3 ? COL(danger) : e->level == 2 ? COL(status_idle) : e->level == 0 ? COL(text_muted) : COL(text_normal);
          ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, col);
          char line[512];
          snprintf(line, sizeof line, "%s%s", tag, e->message ? e->message : "");
          ccord_text_draw_ellipsis(r, line, strlen(line), box.x + 12.0f, ly, box.w - 24.0f, &ls);
          ly += 18.0f;
        }
      }
      return;
    }
  }

  // API Class Permission Review / Consent view (protected security surface: immune to plugin themes)
  if (st_s_plugin_consent_id[0]) {
    const ccord_plugin_info *p = find_plugin_info(ui->d.contrib, st_s_plugin_consent_id);
    if (!p || p->perms_mask != st_s_plugin_consent_perms) {
      if (p) snprintf(st_s_plugin_status_msg, sizeof st_s_plugin_status_msg,
                      "Permissions changed for %s; review again before enabling.", p->name);
      st_s_plugin_consent_id[0] = '\0';
      st_s_plugin_consent_perms = 0;
      st_s_plugin_consent_box_h = 224.0f;
    } else {
      const ccord_theme *saved_theme = ui->theme;
      if (ui->d.protected_theme) ui->theme = ui->d.protected_theme;
      char pmsg[768];
      if (p->allowed_hosts && p->allowed_hosts[0]) {
        snprintf(pmsg, sizeof pmsg, "Declared permissions: %s\nAllowed hosts: %s",
                 p->permissions && p->permissions[0] ? p->permissions : "none", p->allowed_hosts);
      } else {
        snprintf(pmsg, sizeof pmsg, "Declared permissions: %s", p->permissions && p->permissions[0] ? p->permissions : "none");
      }
      const char *warn = "Review the API permissions this plugin requests before enabling. Undeclared Class 2/3 APIs terminate the host.";
      float text_w = fmaxf(80.0f, w - 32.0f);
      float pmsg_h = fmaxf(20.0f, ccord_text_measure(r, pmsg, strlen(pmsg), &ms, text_w).y);
      float warn_h = fmaxf(20.0f, ccord_text_measure(r, warn, strlen(warn), &vs, text_w).y);
      float warn_y_off = 72.0f + pmsg_h + 12.0f;
      float box_h = fmaxf(224.0f, warn_y_off + warn_h + 62.0f);
      float max_h = g->panel.y + g->panel.h - y - 16.0f;
      if (max_h >= 224.0f && box_h > max_h) box_h = max_h;
      st_s_plugin_consent_box_h = box_h;

      ccord_rect box = {x, y, w, box_h};
      uis_card(ui, r, box);
      uint8_t api_cls = (uint8_t)(p->risk_class + 1);
      ccord_text_style ws = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_body, plugin_api_class_color(ui, api_cls));
      const char *title = api_cls >= 3 ? "API CLASS 3: Host & Network Permissions Requested"
                                       : "API CLASS 2: C-Cord Internal Permissions Requested";
      ccord_text_draw_ellipsis(r, title, strlen(title), box.x + 16.0f, box.y + 14.0f, text_w, &ws);
      char sub[256];
      snprintf(sub, sizeof sub, "Plugin: %s v%s (%s)", p->name, p->version, p->id);
      ccord_text_draw_ellipsis(r, sub, strlen(sub), box.x + 16.0f, box.y + 42.0f, text_w, &ns);
      ccord_text_draw(r, pmsg, strlen(pmsg), box.x + 16.0f, box.y + 72.0f, &ms, text_w);
      ccord_text_draw(r, warn, strlen(warn), box.x + 16.0f, box.y + warn_y_off, &vs, text_w);
      st_draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 46.0f, 184.0f, UIS_H_BTN}, "Approve & Enable", true);
      st_draw_button(ui, r, (ccord_rect){box.x + 212.0f, box.y + box.h - 46.0f, 96.0f, UIS_H_BTN}, "Cancel", false);
      ui->theme = saved_theme;
      return;
    }
  }

  // Inline Catalog Plugin Detail view (also used by ccord://plugins/install/<id> deep links)
  if (st_s_plugin_detail_id[0]) {
    const ccord_catalog_entry *e = ccord_contrib_catalog_detail(ui->d.contrib);
    if (!e || strcmp(e->id, st_s_plugin_detail_id) != 0) {
      const ccord_catalog_entry *fb = ccord_contrib_catalog_find(ui->d.contrib, st_s_plugin_detail_id);
      e = (fb && fb->has_detail) ? fb : nullptr;
    }
    if (!e) {
      st_draw_button(ui, r, (ccord_rect){x, y, 86.0f, UIS_H_BTN_SM}, "Back", false);
      ccord_rect box = {x, y + 34.0f, w, 128.0f};
      uis_card(ui, r, box);
      char hdr[128];
      bool busy = cst && cst->detail_loading;
      if (busy) {
        snprintf(hdr, sizeof hdr, "Loading %s from catalog...", st_s_plugin_detail_id);
      } else {
        snprintf(hdr, sizeof hdr, "Plugin not found in catalog: %s", st_s_plugin_detail_id);
      }
      ccord_text_draw_ellipsis(r, hdr, strlen(hdr), box.x + 16.0f, box.y + 18.0f, box.w - 32.0f, &ns);
      const char *sub = busy
                            ? "Fetching verified metadata and SHA-256 digest before prompting for install."
                            : (cst && cst->detail_error[0])
                                  ? cst->detail_error
                                  : "Verify the plugin ID on plugins.c-cord.org or click Retry.";
      ccord_text_draw_ellipsis(r, sub, strlen(sub), box.x + 16.0f, box.y + 46.0f, box.w - 32.0f, &ms);
      st_draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 120.0f, UIS_H_BTN}, "Retry", true);
      return;
    } else {
      st_draw_button(ui, r, (ccord_rect){x, y, 86.0f, UIS_H_BTN_SM}, "Back", false);
      ccord_rect box = {x, y + 34.0f, w, 286.0f};
      uis_card(ui, r, box);
      float nw = ccord_text_draw_ellipsis(r, e->name, strlen(e->name), box.x + 16.0f, box.y + 14.0f, w - 300.0f, &ns);
      float bx = box.x + 16.0f + nw + 8.0f;
      bx += ccord_text_draw_ellipsis(r, e->version, strlen(e->version), bx, box.y + 16.0f, 68.0f, &vs) + 8.0f;
      const char *tl = plugin_tier_label(e->tier);
      ccord_text_style tsty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_tier_color(ui, e->tier));
      if (tl[0]) {
        ccord_text_draw(r, tl, strlen(tl), bx, box.y + 16.0f, &tsty, 0.0f);
        bx += ccord_text_measure(r, tl, strlen(tl), &tsty, 0.0f).x + 8.0f;
      }
      const char *al = plugin_api_class_label(e->api_class);
      ccord_text_style asty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_api_class_color(ui, e->api_class));
      ccord_text_draw(r, al, strlen(al), bx, box.y + 16.0f, &asty, 0.0f);

      char meta[320];
      snprintf(meta, sizeof meta, "ID: %s  |  Author: @%s  |  Repo: %s  |  License: %s",
               e->id, e->author, e->repo, e->license && e->license[0] ? e->license : "Unspecified");
      ccord_text_draw_ellipsis(r, meta, strlen(meta), box.x + 16.0f, box.y + 42.0f, box.w - 32.0f, &ms);
      ccord_text_draw(r, e->description && e->description[0] ? e->description : "No description provided.",
                      strlen(e->description && e->description[0] ? e->description : "No description provided."),
                      box.x + 16.0f, box.y + 66.0f, &ms, box.w - 32.0f);
      char perms[512];
      snprintf(perms, sizeof perms, "Declared permissions: %s", e->permissions && e->permissions[0] ? e->permissions : "none (API Class 1 only)");
      ccord_text_draw_ellipsis(r, perms, strlen(perms), box.x + 16.0f, box.y + 114.0f, box.w - 32.0f, &vs);
      char sha[160];
      snprintf(sha, sizeof sha, "SHA-256: %s", e->sha256);
      ccord_text_draw_ellipsis(r, sha, strlen(sha), box.x + 16.0f, box.y + 136.0f, box.w - 32.0f, &vs);
      if (e->warning_sev != CCORD_WARN_NONE && e->warning_title && e->warning_title[0]) {
        ccord_color wc = e->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
        ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, wc);
        char wline[320];
        snprintf(wline, sizeof wline, "Moderator Warning: %s - %s", e->warning_title, e->warning_message ? e->warning_message : "");
        ccord_text_draw_ellipsis(r, wline, strlen(wline), box.x + 16.0f, box.y + 162.0f, box.w - 32.0f, &wst);
      } else if (e->changelog && e->changelog[0]) {
        char cl[320];
        snprintf(cl, sizeof cl, "Release notes: %s", e->changelog);
        ccord_text_draw_ellipsis(r, cl, strlen(cl), box.x + 16.0f, box.y + 162.0f, box.w - 32.0f, &ms);
      }
      if (e->update_needs_review) {
        ccord_text_style rws = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, COL(status_idle));
        const char *rm = "Auto-update paused: this update adds new Class 2/3 API permissions or carries a moderator warning.";
        ccord_text_draw_ellipsis(r, rm, strlen(rm), box.x + 16.0f, box.y + 190.0f, box.w - 32.0f, &rws);
      }
      if (!e->bundled) {
        if (e->installing) {
          st_draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN}, "Verifying SHA-256...", true);
        } else if (!e->installed) {
          st_draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN}, "Install .cce", true);
        } else if (e->update_available) {
          st_draw_button(ui, r, (ccord_rect){box.x + 16.0f, box.y + box.h - 44.0f, 152.0f, UIS_H_BTN},
                      e->update_needs_review ? "Approve & Update" : "Update Now", true);
        }
        if (e->installed) {
          float ux = box.x + ((!e->installed || e->update_available) ? 180.0f : 16.0f);
          st_draw_button(ui, r, (ccord_rect){ux, box.y + box.h - 44.0f, 96.0f, UIS_H_BTN}, "Uninstall", false);
        }
      }
      return;
    }
  }

  // Subtab 1: Discover Directory (server-side FTS5 search + cursor pagination)
  if (st_s_plugin_subtab == 1) {
    // Row 1: Search bar + Search button + Sort toggle
    ccord_rect r_q = {x, y, w - 196.0f, UIS_H_BTN_SM};
    ccord_rect r_go = {x + w - 188.0f, y, 74.0f, UIS_H_BTN_SM};
    ccord_rect r_sort = {x + w - 106.0f, y, 106.0f, UIS_H_BTN_SM};
    uis_plate(ui, r, r_q, uis_tokens_of(ui).r_control, COL(bg_input));
    if (st_s_plugin_search_focused) {
      ccord_draw_rect(r, (ccord_rect){r_q.x, r_q.y + r_q.h - 2.0f, r_q.w, 2.0f}, COL(brand));
    }
    const char *qdisp = st_s_plugin_search[0] ? st_s_plugin_search : "Search plugins (press Enter)...";
    ccord_text_style qst = st_s_plugin_search[0] ? ns : ms;
    ccord_text_draw_ellipsis(r, qdisp, strlen(qdisp), r_q.x + 10.0f, r_q.y + 6.0f, r_q.w - 20.0f, &qst);
    st_draw_button(ui, r, r_go, "Search", true);
    char slbl[32];
    snprintf(slbl, sizeof slbl, "Sort: %s", plugin_sort_label(st_s_plugin_sort_idx));
    st_draw_button(ui, r, r_sort, slbl, false);
    y += 34.0f;

    // Row 2: Tier chips + API Class 1 filter
    static const char *const k_flbl[4] = {"All", "Official", "Verified", "Unverified"};
    static const float k_fw[4] = {54.0f, 76.0f, 76.0f, 88.0f};
    float fx = x;
    for (int t = 0; t < 4; t++) {
      st_draw_button(ui, r, (ccord_rect){fx, y, k_fw[t], UIS_H_BTN_SM}, k_flbl[t], st_s_plugin_tier_filter == t);
      fx += k_fw[t] + 6.0f;
    }
    st_draw_button(ui, r, (ccord_rect){fx + 4.0f, y, 126.0f, UIS_H_BTN_SM},
                st_s_plugin_class1_only ? "Class 1 Only: ON" : "API Class 1 Only", st_s_plugin_class1_only);
    y += 36.0f;

    size_t cn = ui->d.contrib ? ccord_contrib_catalog_count(ui->d.contrib) : 0;
    if (cn == 0) {
      const char *msg = (cst && cst->status_msg[0]) ? cst->status_msg
                      : "No plugins loaded yet. Click Sync Catalog or Search.";
      ccord_text_draw(r, msg, strlen(msg), x, y, &ms, w);
      return;
    }
    float bottom_lim = g->panel.y + g->panel.h - (cst && cst->has_more ? 52.0f : 16.0f);
    for (size_t i = 0; i < cn; i++) {
      const ccord_catalog_entry *e = ccord_contrib_catalog_at(ui->d.contrib, i);
      if (!e) continue;
      bool has_w = e->warning_sev != CCORD_WARN_NONE && e->warning_title && e->warning_title[0];
      float ch = 98.0f + (has_w ? 20.0f : 0.0f);
      if (y + ch > bottom_lim) break;
      ccord_rect card = {x, y, w, ch};
      uis_card(ui, r, card);
      float nw = ccord_text_draw_ellipsis(r, e->name, strlen(e->name), card.x + 14.0f, card.y + 10.0f, w - 340.0f, &ns);
      float bx = card.x + 14.0f + nw + 8.0f;
      bx += ccord_text_draw_ellipsis(r, e->version, strlen(e->version), bx, card.y + 12.0f, 64.0f, &vs) + 8.0f;
      const char *tl = plugin_tier_label(e->tier);
      ccord_text_style tsty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_tier_color(ui, e->tier));
      if (tl[0]) {
        ccord_text_draw(r, tl, strlen(tl), bx, card.y + 12.0f, &tsty, 0.0f);
        bx += ccord_text_measure(r, tl, strlen(tl), &tsty, 0.0f).x + 8.0f;
      }
      const char *al = plugin_api_class_label(e->api_class);
      ccord_text_style asty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_api_class_color(ui, e->api_class));
      ccord_text_draw(r, al, strlen(al), bx, card.y + 12.0f, &asty, 0.0f);

      ccord_text_draw_ellipsis(r, e->description && e->description[0] ? e->description : "No description",
                               strlen(e->description && e->description[0] ? e->description : "No description"),
                               card.x + 14.0f, card.y + 34.0f, w - 28.0f, &ms);
      char sub[224];
      snprintf(sub, sizeof sub, "by @%s  |  %s  |  %llu downloads",
               e->author, e->id, (unsigned long long)e->downloads);
      ccord_text_draw_ellipsis(r, sub, strlen(sub), card.x + 14.0f, card.y + ch - 28.0f, w - 224.0f, &vs);
      if (has_w) {
        ccord_color wc = e->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
        ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, wc);
        char wl[220];
        snprintf(wl, sizeof wl, "Warning: %s", e->warning_title);
        ccord_text_draw_ellipsis(r, wl, strlen(wl), card.x + 14.0f, card.y + 54.0f, w - 28.0f, &wst);
      }
      st_draw_button(ui, r, (ccord_rect){card.x + card.w - 200.0f, card.y + card.h - 34.0f, 76.0f, UIS_H_BTN_SM}, "Details", false);
      const char *albl = e->installing ? "Installing..."
                       : e->bundled ? "Bundled"
                       : !e->installed ? "Install"
                       : e->update_available ? (e->update_needs_review ? "Review Upd" : "Update")
                       : "Installed";
      st_draw_button(ui, r, (ccord_rect){card.x + card.w - 116.0f, card.y + card.h - 34.0f, 102.0f, UIS_H_BTN_SM},
                  albl, !e->installed || e->update_available);
      y += ch + 10.0f;
    }
    if (cst && cst->has_more) {
      ccord_rect r_more = {x, g->panel.y + g->panel.h - 44.0f, 148.0f, UIS_H_BTN_SM};
      st_draw_button(ui, r, r_more, cst->loading_more ? "Loading..." : "Load More", false);
      char pinfo[96];
      snprintf(pinfo, sizeof pinfo, "Loaded %zu of %zu plugins", cn, cst->total_count);
      ccord_text_draw_ellipsis(r, pinfo, strlen(pinfo), x + 160.0f, r_more.y + 6.0f, w - 168.0f, &vs);
    }
    return;
  }

  // Subtab 2: Updates & Settings
  if (st_s_plugin_subtab == 2) {
    const ccord_catalog_prefs *pr_cur = ccord_contrib_catalog_prefs(ui->d.contrib);
    ccord_catalog_prefs pr = pr_cur ? *pr_cur : (ccord_catalog_prefs){.auto_update_enabled = false, .auto_update_min_tier = 2};
    ccord_rect box = {x, y, w, 142.0f};
    uis_card(ui, r, box);
    ccord_text_draw_ellipsis(r, "Automatic Plugin Updates", 24, box.x + 14.0f, box.y + 12.0f, w - 80.0f, &ns);
    const char *anote = "Verifies SHA-256 and pauses if an update adds new Class 2/3 API permissions.";
    ccord_text_draw_ellipsis(r, anote, strlen(anote), box.x + 14.0f, box.y + 34.0f, w - 80.0f, &ms);
    ccord_rect sw_row = {box.x + 14.0f, box.y + 12.0f, box.w - 28.0f, 36.0f};
    uis_state swst = {.hover = ui_in_rect(ui->mx, ui->my, sw_row)};
    uis_toggle(ui, r, sw_row, pr.auto_update_enabled, swst, "swautoupd", 0);

    float tx = box.x + 14.0f, ty = box.y + 68.0f;
    static const char *const k_tlbl[3] = {"Official Only", "Official & Verified", "All Catalog"};
    static const float k_tw[3] = {104.0f, 134.0f, 118.0f};
    for (int k = 0; k < 3; k++) {
      st_draw_button(ui, r, (ccord_rect){tx, ty, k_tw[k], UIS_H_BTN_SM}, k_tlbl[k], pr.auto_update_min_tier == (uint8_t)(k + 1));
      tx += k_tw[k] + 8.0f;
    }
    const char *sline = (cst && cst->status_msg[0]) ? cst->status_msg : "Catalog ready";
    ccord_text_draw_ellipsis(r, sline, strlen(sline), box.x + 14.0f, box.y + box.h - 28.0f, w - 200.0f, &vs);
    st_draw_button(ui, r, (ccord_rect){box.x + box.w - 178.0f, box.y + box.h - 36.0f, 164.0f, UIS_H_BTN_SM}, "Run Safe Auto-Updates", true);

    y += box.h + 14.0f;
    char uhdr[64];
    snprintf(uhdr, sizeof uhdr, "PENDING UPDATES (%zu)", pend_upd);
    ccord_text_draw(r, uhdr, strlen(uhdr), x, y, &vs, 0.0f);
    y += 18.0f;

    size_t un = ui->d.contrib ? ccord_contrib_catalog_update_count(ui->d.contrib) : 0;
    size_t listed = 0;
    for (size_t i = 0; i < un; i++) {
      const ccord_catalog_entry *e = ccord_contrib_catalog_update_at(ui->d.contrib, i);
      if (!e || !e->update_available) continue;
      float ch = 72.0f;
      if (y + ch > g->panel.y + g->panel.h - 16.0f) break;
      listed++;
      ccord_rect uc = {x, y, w, ch};
      uis_card(ui, r, uc);
      char ut[192];
      snprintf(ut, sizeof ut, "%s  v%s -> v%s", e->name, e->installed_version, e->version);
      ccord_text_draw_ellipsis(r, ut, strlen(ut), uc.x + 14.0f, uc.y + 12.0f, w - 164.0f, &ns);
      ccord_text_style st_s = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control,
                                       e->update_needs_review ? COL(status_idle) : COL(status_online));
      const char *um = e->update_needs_review ? "Paused: New API Class permissions or warning - review required"
                                              : "Safe to auto-update (same API permissions)";
      ccord_text_draw_ellipsis(r, um, strlen(um), uc.x + 14.0f, uc.y + 38.0f, w - 164.0f, &st_s);
      st_draw_button(ui, r, (ccord_rect){uc.x + uc.w - 134.0f, uc.y + 20.0f, 120.0f, UIS_H_BTN_SM},
                  e->installing ? "Updating..." : (e->update_needs_review ? "Review & Update" : "Update Now"), true);
      y += ch + 8.0f;
    }
    if (listed == 0) {
      const char *okm = "All installed catalog plugins are up to date.";
      ccord_text_draw(r, okm, strlen(okm), x, y + 6.0f, &ms, w);
    }
    return;
  }

  // Subtab 0: Installed
  const char *drop_hint = st_s_plugin_status_msg[0] ? st_s_plugin_status_msg
                        : (cst && cst->status_msg[0]) ? cst->status_msg
                        : "Drag & drop a .cce plugin package into C-Cord to install, or browse Discover.";
  ccord_text_draw_ellipsis(r, drop_hint, strlen(drop_hint), x, y, w, &ms);
  y += 24.0f;

  size_t n = ui->d.contrib ? ccord_contrib_plugin_count(ui->d.contrib) : 0;
  if (n == 0) {
    const char *none = "No extensions are loaded. Drop a .cce package or install one from Discover.";
    ccord_text_draw(r, none, strlen(none), x, y, &ms, w);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(ui->d.contrib, i);
    if (!p) continue;
    plugin_card_rects rc;
    float h = st_plugin_card_layout(ui, g, i, p, y, &rc);
    if (y + h > g->panel.y + g->panel.h - 16.0f) break;
    uis_card(ui, r, rc.card);
    float nw = ccord_text_draw_ellipsis(r, p->name, strlen(p->name), rc.card.x + 14.0f, rc.card.y + 10.0f, w - 340.0f, &ns);
    float bx = rc.card.x + 14.0f + nw + 8.0f;
    bx += ccord_text_draw_ellipsis(r, p->version, strlen(p->version), bx, rc.card.y + 12.0f, 64.0f, &vs) + 8.0f;

    const char *tl = plugin_tier_label(p->tier);
    if (tl[0]) {
      ccord_text_style tsty = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_tier_color(ui, p->tier));
      ccord_text_draw(r, tl, strlen(tl), bx, rc.card.y + 12.0f, &tsty, 0.0f);
      bx += ccord_text_measure(r, tl, strlen(tl), &tsty, 0.0f).x + 8.0f;
    }

    uint8_t api_cls = (uint8_t)(p->risk_class + 1);
    const char *al = plugin_api_class_label(api_cls);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, plugin_api_class_color(ui, api_cls));
    ccord_text_draw(r, al, strlen(al), bx, rc.card.y + 12.0f, &ts, 0.0f);

    ccord_color sc = p->state == CCORD_PLUGIN_READY ? COL(status_online)
                   : p->state == CCORD_PLUGIN_FAILED ? COL(danger)
                   : (p->state == CCORD_PLUGIN_STARTING || p->state == CCORD_PLUGIN_NEEDS_CONSENT) ? COL(status_idle)
                   : COL(status_offline);
    const char *sn = st_state_name(p->state);
    ccord_text_style ss = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, sc);
    float sw_ = ccord_text_measure(r, sn, strlen(sn), &ss, 0.0f).x;
    ccord_draw_circle(r, rc.card.x + rc.card.w - 20.0f - sw_ - 10.0f, rc.card.y + 10.0f + ui_line_h(ui, ns.size) * 0.5f, 4.0f, sc);
    ccord_text_draw(r, sn, strlen(sn), rc.card.x + rc.card.w - 14.0f - sw_, rc.card.y + 12.0f, &ss, 0.0f);

    float info_y = rc.card.y + 36.0f;
    char perms[280];
    snprintf(perms, sizeof perms, "Permissions: %s", p->permissions && p->permissions[0] ? p->permissions : "none (API Class 1)");
    ccord_text_draw_ellipsis(r, perms, strlen(perms), rc.card.x + 14.0f, info_y, w - 28.0f, &vs);
    info_y += 18.0f;
    if (p->warning_sev != CCORD_WARN_NONE && p->warning_title && p->warning_title[0]) {
      ccord_color wc = p->warning_sev >= CCORD_WARN_DANGER ? COL(danger) : COL(status_idle);
      ccord_text_style wst = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, wc);
      char wl[220];
      snprintf(wl, sizeof wl, "Warning: %s", p->warning_title);
      ccord_text_draw_ellipsis(r, wl, strlen(wl), rc.card.x + 14.0f, info_y, w - 28.0f, &wst);
      info_y += 18.0f;
    }
    if (p->error && p->error[0]) {
      ccord_text_style es = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(danger));
      ccord_text_draw_ellipsis(r, p->error, strlen(p->error), rc.card.x + 14.0f, info_y, w - 28.0f, &es);
    }

    const char *tlabel = p->state == CCORD_PLUGIN_NEEDS_CONSENT ? "Review Perms"
                       : (p->state == CCORD_PLUGIN_READY || p->state == CCORD_PLUGIN_STARTING) ? "Disable" : "Enable";
    st_draw_button(ui, r, rc.btn_toggle, tlabel, p->state == CCORD_PLUGIN_NEEDS_CONSENT);
    size_t ecnt = ccord_contrib_plugin_error_count(ui->d.contrib, p->id);
    char llabel[48];
    if (ecnt > 0) snprintf(llabel, sizeof llabel, "Log (%zu)", ecnt);
    else snprintf(llabel, sizeof llabel, "Debug Log");
    st_draw_button(ui, r, rc.btn_log, llabel, false);
    st_draw_button(ui, r, rc.btn_reload, "Reload", false);
    if (rc.btn_configure.w > 0.0f) {
      st_draw_button(ui, r, rc.btn_configure, "Configure", false);
    }
    st_draw_button(ui, r, rc.btn_export, "Export .cce", false);
    if (rc.btn_update.w > 0.0f) {
      char ul[48];
      if (p->update_needs_review) snprintf(ul, sizeof ul, "Review Upd");
      else snprintf(ul, sizeof ul, "Update %s", p->update_version);
      st_draw_button(ui, r, rc.btn_update, ul, true);
    }
    if (rc.btn_auto.w > 0.0f) {
      st_draw_button(ui, r, rc.btn_auto, p->auto_update ? "Auto: On" : "Auto: Off", false);
    }
    if (rc.btn_uninstall.w > 0.0f) {
      st_draw_button(ui, r, rc.btn_uninstall, "Uninstall", false);
    }
    y += h + 10.0f;
  }
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
  st_s_plugin_subtab = 1;
  st_s_plugin_log_id[0] = '\0';
  st_s_plugin_consent_id[0] = '\0';
  st_s_plugin_consent_perms = 0;
  st_s_plugin_consent_box_h = 224.0f;
  snprintf(st_s_plugin_detail_id, sizeof st_s_plugin_detail_id, "%s", plugin_id);
  if (ui->d.contrib) {
    ccord_contrib_catalog_fetch_detail(ui->d.contrib, plugin_id);
  }
  ui_notice(ui, "Review plugin before installing: %.48s", plugin_id);
  ui->dirty = true;
  return true;
}

