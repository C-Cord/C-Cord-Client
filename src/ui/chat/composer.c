// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"

// ---------------------------------------------------------------- composer

static float composer_wrap(const ccord_ui *ui)
{
  // The text area of ui_draw_composer: the box less its padding, the '+' button and the right-hand buttons
  // (desktop: GIF + Sticker + Emoji + Mic = 128px; mobile: Emoji + Send/Mic = 88px).
  float left_pad = ui->mobile_layout ? 38.0f : 28.0f;
  float right_reserve = ui->mobile_layout ? 88.0f : 128.0f;
  return maxf(ui->layout.composer.w - 2.0f * M(composer_margin_x) - 2.0f * M(composer_pad_x) - left_pad - right_reserve, 20.0f);
}


static size_t ui_channel_active_uploads(const ccord_ui *ui, ccord_upload_progress *out, size_t max)
{
  if (!ui || !ui->d.session || !ui->d.store || ui_is_dm_requests_view(ui)) return 0;
  ccord_id cid = ccord_store_selected_channel(ui->d.store);
  if (cid == CCORD_ID_NONE) return 0;
  return ccord_session_active_uploads(ui->d.session, cid, out, max);
}

static float ui_upload_banner_height(const ccord_ui *ui)
{
  size_t n = ui_channel_active_uploads(ui, nullptr, 0);
  if (n == 0) return 0.0f;
  size_t rows = n < UI_UPLOADS_SHOW_MAX ? n : UI_UPLOADS_SHOW_MAX;
  return UI_UPLOAD_ROW_H * (float)rows;
}

void ui_upload_debug_get(const ccord_ui *ui, ui_upload_debug *out)
{
  if (!out) return;
  if (!ui) {
    memset(out, 0, sizeof *out);
    return;
  }
  *out = ui->dbg_uploads;
}

static float ui_staged_banner_height(const ccord_ui *ui)
{
  if (!ui || ui->staged_file_count == 0) return 0.0f;
  size_t cols = ui->staged_file_count <= 2u ? ui->staged_file_count : (ui->staged_file_count <= 6u ? 3u : 5u);
  size_t frows = (ui->staged_file_count + cols - 1u) / cols;
  float row_h = ui->mobile_layout ? 38.0f : 28.0f;
  return row_h * (float)frows;
}

static size_t composer_preedit_fit(ccord_renderer *r, const char *s, size_t len, const ccord_text_style *st, float max_w)
{
  if (len == 0 || max_w <= 0.0f) return 0;
  size_t lo = 0, hi = len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    while (mid > lo && (((unsigned char)s[mid] & 0xC0u) == 0x80u)) mid--;
    if (mid <= lo) mid = ccord_utf8_next(s, hi, lo);
    if (mid <= lo || mid >= hi) break;
    if (ccord_text_measure(r, s, mid, st, 0.0f).x <= max_w) lo = mid;
    else hi = mid;
  }
  return lo;
}

// Measures or draws the IME composition preedit starting at (start_x, start_y) inside [0, wrap], wrapping across
// lines so long compositions never overflow the composer or overlap right-hand buttons. Returns bottom y relative to
// the composer text origin.
static float composer_preedit_walk(const ccord_ui *ui, ccord_renderer *r, const char *pre, size_t plen, float wrap,
                                   float ox, float oy, float start_x, float start_y, float lh, float caret_h, bool draw)
{
  if (!pre || plen == 0) return start_y + lh;
  ccord_text_style pst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(brand));
  float cx = start_x, cy = start_y;
  size_t pos = 0;
  while (pos < plen) {
    if (cx == 0.0f && pos > 0) {
      while (pos < plen && (pre[pos] == ' ' || pre[pos] == '\t')) pos++;
      if (pos >= plen) break;
    }
    size_t rem = plen - pos;
    float rw = ccord_text_measure(r, pre + pos, rem, &pst, 0.0f).x;
    if (cx + rw <= wrap + 0.01f) {
      if (draw) {
        ccord_text_draw(r, pre + pos, rem, ox + cx, oy + cy, &pst, 0.0f);
        ccord_draw_rect(r, (ccord_rect){ox + cx, oy + cy + caret_h - 1.0f, rw, 1.0f}, COL(brand));
      }
      cx += rw;
      break;
    }
    float avail = wrap - cx;
    size_t fit = avail > 0.0f ? composer_preedit_fit(r, pre + pos, rem, &pst, avail) : 0u;
    size_t take = 0;
    bool broke_at_space = false;
    if (fit > 0 && fit < rem) {
      if (pre[pos + fit] == ' ' || pre[pos + fit] == '\t') {
        take = fit;
        while (take > 0 && (pre[pos + take - 1u] == ' ' || pre[pos + take - 1u] == '\t')) take--;
        broke_at_space = take > 0;
      } else {
        size_t sp = fit;
        while (sp > 0 && pre[pos + sp - 1u] != ' ' && pre[pos + sp - 1u] != '\t') sp--;
        if (sp > 0) {
          while (sp > 0 && (pre[pos + sp - 1u] == ' ' || pre[pos + sp - 1u] == '\t')) sp--;
          if (sp > 0) {
            take = sp;
            broke_at_space = true;
          }
        }
      }
    }
    if (!broke_at_space) {
      if (cx > 0.0f) {
        cx = 0.0f;
        cy += lh;
        continue;
      }
      take = fit > 0 ? fit : ccord_utf8_next(pre + pos, rem, 0);
      if (take == 0) take = 1u;
    }
    float pw = ccord_text_measure(r, pre + pos, take, &pst, 0.0f).x;
    if (draw) {
      ccord_text_draw(r, pre + pos, take, ox + cx, oy + cy, &pst, 0.0f);
      ccord_draw_rect(r, (ccord_rect){ox + cx, oy + cy + caret_h - 1.0f, pw, 1.0f}, COL(brand));
    }
    cx += pw;
    pos += take;
    if (broke_at_space) {
      while (pos < plen && (pre[pos] == ' ' || pre[pos] == '\t')) pos++;
    }
    if (pos < plen) {
      cx = 0.0f;
      cy += lh;
    }
  }
  return cy + lh;
}

float ui_composer_measure(ccord_ui *ui, ccord_renderer *r)
{
  float lh = ui_line_h(ui, M(font_size_base));
  float wrap = composer_wrap(ui);
  float th = lh;
  const char *text = ccord_text_edit_cstr(&ui->edit);
  if (ui->edit.len > 0) {
    th = ui_composer_format_height(ui, r, text, ui->edit.len, wrap);
  }
  size_t plen = strlen(ui->ime_preedit);
  if (plen > 0) {
    ccord_rect caret = ui_composer_format_caret(ui, r, text, ui->edit.len, wrap, ui->edit.caret);
    float pre_bottom = composer_preedit_walk(ui, r, ui->ime_preedit, plen, wrap, 0.0f, 0.0f,
                                             caret.x, caret.y, lh, caret.h, false);
    if (pre_bottom > th) th = pre_bottom;
  }
  float banner = (ui->reply_message != CCORD_ID_NONE ? UI_REPLY_BAR_H : 0.0f) +
                 ui_upload_banner_height(ui) + ui_staged_banner_height(ui);
  return clampf(th + 2.0f * M(composer_pad_y) + banner, M(composer_min_height), M(composer_max_height));
}

void ui_draw_composer(ccord_ui *ui, ccord_renderer *r)
{
  memset(&ui->dbg_uploads, 0, sizeof ui->dbg_uploads);
  ui->msg_req_banner_visible = false;
  ui->msg_req_banner_accept_rect = (ccord_rect){0, 0, 0, 0};
  ui->msg_req_banner_reject_rect = (ccord_rect){0, 0, 0, 0};
  ui->mv.composer_btn = (ccord_rect){0, 0, 0, 0};
  ui->reply_bar_rect = ui->reply_close_rect = ui->reply_text_rect = ui->reply_input_rect = (ccord_rect){0, 0, 0, 0};
  // Voice message controls are set again below when they are drawn; stale rects must never take a click.
  ui->vm.mic_rect = ui->vm.bar_rect = ui->vm.send_rect = ui->vm.cancel_rect = (ccord_rect){0, 0, 0, 0};
  const ccord_layout_rect *strip = &ui->layout.composer;
  ccord_draw_rect(r, ui_rect(*strip), COL(bg_primary));
  if (ui_is_dm_requests_view(ui)) {
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    return;
  }
  ccord_rect box = ui_composer_box(ui);
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  const ccord_channel *sel_ch = ui_selected_channel(ui);
  if (sel_ch && sel_ch->guild_id == CCORD_DM_GUILD_ID && sel_ch->is_message_request) {
    ui->msg_req_banner_visible = true;
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    ccord_draw_rrect(r, box, M(composer_radius), COL(bg_secondary));
    const char *who = ui_dm_display_name(ui, sel_ch);
    char prompt[192];
    snprintf(prompt, sizeof prompt, "Do you want to let %s message you?", who && who[0] ? who : "this user");
    float mrbh = uis_btn_h_for(ui, 28.0f);
    ccord_rect rej_r = {box.x + box.w - 86.0f, box.y + (box.h - mrbh) * 0.5f, 74.0f, mrbh};
    ccord_rect acc_r = {rej_r.x - 82.0f, rej_r.y, 74.0f, mrbh};
    ui->msg_req_banner_accept_rect = acc_r;
    ui->msg_req_banner_reject_rect = rej_r;
    bool ahv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, acc_r);
    bool rhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rej_r);
    uis_button(ui, r, acc_r, "Accept", UIS_BTN_PRIMARY, UIS_BTN_S_SM, (uis_state){.hover = ahv}, "mrbacc", 0);
    uis_button(ui, r, rej_r, "Reject", UIS_BTN_DANGER_SOFT, UIS_BTN_S_SM, (uis_state){.hover = rhv}, "mrbrej", 0);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_header));
    float py = box.y + (box.h - ui_line_h(ui, ps.size)) * 0.5f;
    ccord_text_draw_ellipsis(r, prompt, strlen(prompt), box.x + 14.0f, py, maxf(acc_r.x - (box.x + 24.0f), 20.0f), &ps);
    return;
  }
  if (sel_ch && sel_ch->kind == CCORD_CHANNEL_TEXT && ui_self_pending(ui, sel_ch->guild_id) &&
      !ui_role_preview_active_for(ui, sel_ch->guild_id)) {
    // Membership screening not completed: the box is a locked banner whose button reopens the rules.
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    ccord_draw_rrect(r, box, M(composer_radius), COL(bg_secondary));
    const char *why = ui_composer_lock_reason(ui);
    if (!why) why = "Complete the server rules to send messages.";
    float bw = uis_label_width(ui, r, "Complete the rules");
    float bh = uis_btn_h_for(ui, 28.0f);
    ccord_rect btn = {box.x + box.w - bw - 12.0f, box.y + (box.h - bh) * 0.5f, bw, bh};
    ui->mv.composer_btn = btn;
    bool bhv = ui->mouse_in && ui_in_rect(ui->mx, ui->my, btn);
    uis_button(ui, r, btn, "Complete the rules", UIS_BTN_PRIMARY, UIS_BTN_S_SM, (uis_state){.hover = bhv}, "mvcomp", 0);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_placeholder));
    float py = box.y + (box.h - ui_line_h(ui, ps.size)) * 0.5f;
    ccord_text_draw_ellipsis(r, why, strlen(why), box.x + 14.0f, py, maxf(btn.x - (box.x + 24.0f), 20.0f), &ps);
    return;
  }
  if (ui_vm_is_active(ui)) {
    // Recording, or a clip waiting to send: the bar takes the place of the text box until it is sent or discarded.
    ui->composer_attach_rect = (ccord_rect){0, 0, 0, 0};
    ui->text_area = (ccord_rect){0, 0, 0, 0};
    ui_vm_draw_bar(ui, r, box);
    return;
  }
  ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
  bool replying = ui->reply_message != CCORD_ID_NONE;
  if (replying) {
    // One unit with the composer box below it: same x and width, no gap, rounded top corners here and square ones there.
    ccord_rect bar = {box.x, box.y, box.w, UI_REPLY_BAR_H};
    float rad = fminf(M(composer_radius), bar.h);
    ccord_draw_rrect(r, bar, rad, COL(bg_secondary));
    ccord_draw_rect(r, (ccord_rect){bar.x, bar.y + rad, bar.w, bar.h - rad}, COL(bg_secondary));
    ui->reply_bar_rect = bar;
    const ccord_message *rm = ccord_store_find_message(ui->d.store, ui->reply_channel, ui->reply_message);
    const ccord_user *ru = rm ? ccord_store_user(ui->d.store, rm->author_id) : nullptr;
    const char *rname = ru && ru->name && ru->name[0] ? ru->name : "a message";
    if (rm && ru) {
      ccord_id rg = ccord_store_selected_guild(ui->d.store);
      const ccord_member *gm = (rg != CCORD_ID_NONE && rg != CCORD_DM_GUILD_ID)
                                   ? ccord_store_find_member(ui->d.store, rg, rm->author_id) : nullptr;
      if (gm && gm->nick[0] != '\0') rname = gm->nick;
    }
    float by = bar.y + (bar.h - UI_REPLY_BTN_H) * 0.5f;
    bool mouse = ui->mouse_in && !ui->menu.open && !ui->settings_open;
    ccord_rect close_r = {bar.x + bar.w - UI_REPLY_BAR_PAD * 0.5f - UI_REPLY_CLOSE_W, by, UI_REPLY_CLOSE_W, UI_REPLY_BTN_H};
    ui->reply_close_rect = close_r;
    bool close_hv = mouse && ui_in_rect(ui->mx, ui->my, close_r);
    ccord_draw_rrect(r, close_r, close_r.h * 0.5f, close_hv ? COL(bg_item_hover) : COL(bg_tertiary));
    ui_icon_close(r, close_r.x + close_r.w * 0.5f, close_r.y + close_r.h * 0.5f, 12.0f,
                  close_hv ? COL(text_header) : COL(text_muted));
    float right = close_r.x - 6.0f;
    if (bar.w >= 110.0f) {
      ccord_rect pr = {close_r.x - 6.0f - UI_REPLY_PING_W, by, UI_REPLY_PING_W, UI_REPLY_BTN_H};
      ui->reply_ping_rect = pr;
      bool on = ui->reply_ping;
      bool pr_hv = mouse && ui_in_rect(ui->mx, ui->my, pr);
      ccord_color pc = on ? COL(brand) : COL(text_muted);
      ccord_draw_rrect(r, pr, pr.h * 0.5f, on ? ui_alpha(COL(brand), pr_hv ? 0.40f : 0.28f)
                                              : (pr_hv ? COL(bg_item_hover) : COL(bg_tertiary)));
      ccord_text_style pst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), pc);
      ccord_vec2 pm = ccord_text_measure(r, "@", 1, &pst, 0.0f);
      float pcx = pr.x + pr.w * 0.5f, pcy = pr.y + pr.h * 0.5f;
      ccord_text_draw(r, "@", 1, pcx - pm.x * 0.5f, pcy - pm.y * 0.5f, &pst, 0.0f);
      if (!on) {   // off is also shown by a strike, not only by the muted colour
        ccord_draw_line(r, (ccord_vec2){pcx - 6.0f, pcy + 5.0f}, (ccord_vec2){pcx + 6.0f, pcy - 5.0f}, 1.5f, pc);
      }
      right = pr.x - 8.0f;
    }
    ccord_text_style ls = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_secondary));
    ccord_text_style nst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 1.0f, COL(text_header));
    float tx = bar.x + UI_REPLY_BAR_PAD;
    float avail = maxf(right - tx, 0.0f);
    float ty = bar.y + (bar.h - ui_line_h(ui, ls.size)) * 0.5f;
    float lw = ccord_text_draw_ellipsis(r, "Replying to ", 12, tx, ty, avail, &ls);
    float nw = 0.0f;
    if (avail - lw > 12.0f) nw = ccord_text_draw_ellipsis(r, rname, strlen(rname), tx + lw, ty, avail - lw, &nst);
    ui->reply_text_rect = (ccord_rect){tx, ty, lw + nw, ui_line_h(ui, ls.size)};
    box.y += bar.h;
    box.h -= bar.h;
    ui->reply_input_rect = box;
    if (box.h < 10.0f) return;
  }
  memset(ui->staged_spoiler_rects, 0, sizeof ui->staged_spoiler_rects);
  memset(ui->staged_remove_rects, 0, sizeof ui->staged_remove_rects);
  {
    ccord_upload_progress ups[UI_UPLOADS_SHOW_MAX];
    size_t nup_total = ui_channel_active_uploads(ui, ups, UI_UPLOADS_SHOW_MAX);
    size_t nup = nup_total < UI_UPLOADS_SHOW_MAX ? nup_total : UI_UPLOADS_SHOW_MAX;
    if (nup > 0) {
      float ubanner_h = UI_UPLOAD_ROW_H * (float)nup;
      ccord_rect ubanner = {box.x, box.y, box.w, ubanner_h};
      uis_tokens utk = uis_tokens_of(ui);
      ccord_draw_rrect(r, ubanner, utk.r_control, COL(bg_secondary));
      ui->dbg_uploads.count = nup_total;
      ui->dbg_uploads.banner_rect = ubanner;
      for (size_t i = 0; i < nup; i++) {
        ccord_rect row_rc = {box.x + 2.0f, box.y + (float)i * UI_UPLOAD_ROW_H + 2.0f,
                             maxf(box.w - 4.0f, 20.0f), UI_UPLOAD_ROW_H - 4.0f};
        ccord_draw_rrect(r, row_rc, utk.r_control, COL(bg_tertiary));
        int pct = 0;
        float target_frac = 0.04f;
        if (ups[i].phase == CCORD_UPLOAD_FINALIZE) {
          pct = 100;
          target_frac = 1.0f;
        } else if (ups[i].phase == CCORD_UPLOAD_PUT) {
          float bfrac = ups[i].total_bytes > 0
                          ? clampf((float)ups[i].uploaded_bytes / (float)ups[i].total_bytes, 0.0f, 1.0f)
                          : 0.5f;
          pct = (int)(bfrac * 100.0f + 0.5f);
          if (pct > 99 && ups[i].uploaded_bytes < ups[i].total_bytes) pct = 99;
          target_frac = clampf(0.05f + 0.91f * bfrac, 0.05f, 0.96f);
        }
        float shown = ui_ease_to(ui, "uprog", ups[i].upload_id ? ups[i].upload_id : (uint64_t)(i + 1u),
                                 target_frac, 90u);
        ccord_color accent = ups[i].phase == CCORD_UPLOAD_FINALIZE ? COL(status_online) : COL(brand);
        ui_icon_attachment(r, row_rc.x + 12.0f, row_rc.y + 11.0f, 13.0f, accent);

        char title[160];
        const char *fname = ups[i].filename[0] ? ups[i].filename : "attachment";
        if (ups[i].file_count > 1u) {
          snprintf(title, sizeof title, "Uploading %s (%u/%u)", fname,
                   (unsigned)(ups[i].file_index + 1u), (unsigned)ups[i].file_count);
        } else {
          snprintf(title, sizeof title, "Uploading %s", fname);
        }
        if (i + 1u == nup && nup_total > nup) {
          size_t tl = strlen(title);
          if (tl + 16u < sizeof title) {
            snprintf(title + tl, sizeof title - tl, " (+%zu more)", nup_total - nup);
          }
        }

        char done_s[32], tot_s[32], stat[96];
        dc_format_size(ups[i].uploaded_bytes, done_s, sizeof done_s);
        dc_format_size(ups[i].total_bytes, tot_s, sizeof tot_s);
        if (ups[i].phase == CCORD_UPLOAD_SLOT) {
          snprintf(stat, sizeof stat, "Preparing\xE2\x80\xA6 \xC2\xB7 %s (0%%)", tot_s);
        } else if (ups[i].phase == CCORD_UPLOAD_FINALIZE) {
          snprintf(stat, sizeof stat, "Finishing\xE2\x80\xA6 \xC2\xB7 %s (100%%)", tot_s);
        } else {
          snprintf(stat, sizeof stat, "%s / %s (%d%%)", done_s, tot_s, pct);
        }

        ccord_text_style sts = ui_style(ui, CCORD_FONT_REGULAR, utk.fs_caption, COL(text_secondary));
        ccord_text_style tts = ui_style(ui, CCORD_FONT_BOLD, utk.fs_caption, COL(text_header));
        ccord_vec2 sm = ccord_text_measure(r, stat, strlen(stat), &sts, 0.0f);
        float text_y = row_rc.y + 3.0f;
        float stat_x = maxf(row_rc.x + row_rc.w - sm.x - 8.0f, row_rc.x + 24.0f);
        if (row_rc.w >= 140.0f) {
          ccord_text_draw(r, stat, strlen(stat), stat_x, text_y, &sts, 0.0f);
        }
        float title_avail = maxf((row_rc.w >= 140.0f ? stat_x - 8.0f : row_rc.x + row_rc.w - 8.0f) - (row_rc.x + 22.0f), 10.0f);
        ccord_text_draw_ellipsis(r, title, strlen(title), row_rc.x + 22.0f, text_y, title_avail, &tts);

        ccord_rect bar = {row_rc.x + 8.0f, row_rc.y + row_rc.h - 6.5f, maxf(row_rc.w - 16.0f, 8.0f), 4.0f};
        ccord_draw_rrect(r, bar, bar.h * 0.5f, COL(bg_primary));
        float fw = maxf(bar.w * clampf(shown, 0.0f, 1.0f), bar.h);
        ccord_draw_rrect(r, (ccord_rect){bar.x, bar.y, fminf(fw, bar.w), bar.h}, bar.h * 0.5f, accent);

        ui->dbg_uploads.bar_rects[i] = bar;
        ui->dbg_uploads.shown_fraction[i] = shown;
        ui->dbg_uploads.pct[i] = pct;
        snprintf(ui->dbg_uploads.label[i], sizeof ui->dbg_uploads.label[i], "%.96s | %.48s", title, stat);
      }
      box.y += ubanner_h;
      box.h -= ubanner_h;
      if (box.h < 10.0f) return;
    }
  }
  if (ui->staged_file_count > 0) {
    size_t max_cols = box.w < 260.0f ? 1u : (box.w < 460.0f ? 2u : (ui->staged_file_count <= 6u ? 3u : 5u));
    size_t cols = ui->staged_file_count < max_cols ? ui->staged_file_count : max_cols;
    size_t frows = (ui->staged_file_count + cols - 1u) / cols;
    float row_h = ui->mobile_layout ? 38.0f : 28.0f;
    float cell_h = ui->mobile_layout ? 34.0f : 24.0f;
    float fbanner_h = row_h * (float)frows;
    ccord_rect fbanner = {box.x, box.y, box.w, fbanner_h};
    ccord_draw_rrect(r, fbanner, 4.0f, COL(bg_secondary));
    float cell_w = box.w / (float)cols;
    for (size_t i = 0; i < ui->staged_file_count; i++) {
      size_t col = i % cols;
      size_t row = i / cols;
      ccord_rect cell = {box.x + (float)col * cell_w + 2.0f, box.y + (float)row * row_h + 2.0f,
                         maxf(cell_w - 4.0f, 20.0f), cell_h};
      bool sp = ui->staged_files[i].spoiler;
      ccord_draw_rrect(r, cell, 4.0f, COL(bg_tertiary));
      ccord_draw_rect(r, (ccord_rect){cell.x, cell.y + 3.0f, 2.0f, cell.h - 6.0f},
                      sp ? COL(status_idle) : COL(status_online));
      float rem_sz = ui->mobile_layout ? 32.0f : 20.0f;
      float rem_h = ui->mobile_layout ? 32.0f : 22.0f;
      ccord_rect rem_rc = {cell.x + cell.w - rem_sz - 2.0f, cell.y + (cell.h - rem_h) * 0.5f, rem_sz, rem_h};
      ui->staged_remove_rects[i] = rem_rc;
      float sp_w = cols <= 2u ? (ui->mobile_layout ? 64.0f : 54.0f) : (ui->mobile_layout ? 40.0f : 30.0f);
      float sp_h = ui->mobile_layout ? 30.0f : 20.0f;
      ccord_rect sp_rc = {rem_rc.x - sp_w - 4.0f, cell.y + (cell.h - sp_h) * 0.5f, sp_w, sp_h};
      ui->staged_spoiler_rects[i] = sp_rc;
      ccord_draw_rrect(r, sp_rc, 4.0f, sp ? ui_alpha(COL(status_idle), 0.30f) : COL(bg_secondary));
      const char *slbl = cols <= 2u ? "SPOILER" : "SP";
      ccord_text_style sst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 4.0f,
                                      sp ? COL(text_header) : COL(text_muted));
      ccord_vec2 sm = ccord_text_measure(r, slbl, strlen(slbl), &sst, 0.0f);
      ccord_text_draw(r, slbl, strlen(slbl), sp_rc.x + (sp_rc.w - sm.x) * 0.5f,
                      sp_rc.y + (sp_rc.h - sm.y) * 0.5f, &sst, 0.0f);
      ui_icon_close(r, rem_rc.x + rem_rc.w * 0.5f, rem_rc.y + rem_rc.h * 0.5f, 11.0f, COL(text_muted));
      char flabel[192];
      snprintf(flabel, sizeof flabel, "%s %s (%.1f KB)", sp ? "[SPOILER]" : "[File]",
               ui->staged_files[i].filename, (double)ui->staged_files[i].size / 1024.0);
      ccord_text_style fls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_header));
      float avail = maxf(sp_rc.x - (cell.x + 8.0f), 6.0f);
      ccord_text_draw_ellipsis(r, flabel, strlen(flabel), cell.x + 7.0f,
                               cell.y + (cell.h - ui_line_h(ui, fls.size)) * 0.5f, avail, &fls);
    }
    box.y += fbanner_h;
    box.h -= fbanner_h;
    if (box.h < 10.0f) return;
  }
  bool editing = ui->editing_message != CCORD_ID_NONE;
  if (editing) ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, M(composer_radius) + 1.0f, COL(brand));
  uis_surface(ui, r, box, COL(bg_input));
  if (replying) {   // the reply bar sits on the box: its top edge is square
    ccord_draw_rect(r, (ccord_rect){box.x, box.y, box.w, fminf(M(composer_radius), box.h)}, COL(bg_input));
  }
  float attach_w = uis_icon_btn_for(ui);
  ui->composer_attach_rect = (ccord_rect){box.x + (ui->mobile_layout ? 2.0f : 4.0f),
                                          box.y + (box.h - attach_w) * 0.5f, attach_w, attach_w};
  bool attach_hv = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, ui->composer_attach_rect);
  ccord_text_style pls = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base),
                                  uis_icon_button(ui, r, ui->composer_attach_rect, (uis_state){.hover = attach_hv, .selected = ui->plus_menu_open}, "compattach", 0));
  ccord_vec2 plt = ccord_text_measure(r, "+", 1, &pls, 0.0f);
  ccord_text_draw(r, "+", 1, ui->composer_attach_rect.x + (attach_w - plt.x) * 0.5f,
                  ui->composer_attach_rect.y + (attach_w - plt.y) * 0.5f, &pls, 0.0f);
  float left_pad = ui->mobile_layout ? 38.0f : 28.0f;
  float right_reserve = ui->mobile_layout ? 88.0f : 128.0f;
  ccord_rect area = {box.x + M(composer_pad_x) + left_pad, box.y + M(composer_pad_y),
                     box.w - 2.0f * M(composer_pad_x) - left_pad - right_reserve, box.h - 2.0f * M(composer_pad_y)};
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_normal));
  float wrap = composer_wrap(ui);   // = max(area.w, 20); the styled layout wraps, measures and draws at this width
  const char *text = ccord_text_edit_cstr(&ui->edit);
  size_t len = ui->edit.len;
  float lh = ui_line_h(ui, st.size);

  if (ui->pending_click) {
    ui->pending_click = false;
    size_t pos = ui_composer_format_hit(ui, r, text, len, wrap, ui->pending_x - area.x, ui->pending_y - area.y + ui->composer_scroll);
    if (ui->pending_select_mode == 1) {
      ccord_text_edit_select_word_at(&ui->edit, pos);
    } else if (ui->pending_select_mode == 2) {
      ccord_text_edit_select_line_at(&ui->edit, pos);
    } else {
      ccord_text_edit_set_caret(&ui->edit, pos, ui->pending_extend);
    }
    ui->pending_select_mode = 0;
    ui->caret_epoch_ms = ui->now_ms;
    ui->caret_on = true;
    ui_update_autocomplete(ui);
  }
  float total = len ? ui_composer_format_height(ui, r, text, len, wrap) : lh;
  if (ui->pending_vert_move != 0) {
    int dir = ui->pending_vert_move;
    ui->pending_vert_move = 0;
    ccord_rect cur = ui_composer_format_caret(ui, r, text, len, wrap, ui->edit.caret);
    if (dir < 0 && cur.y < lh * 0.5f) {
      ccord_text_edit_set_caret(&ui->edit, 0, ui->pending_extend);
    } else if (dir > 0 && cur.y + lh * 1.5f >= total) {
      ccord_text_edit_set_caret(&ui->edit, len, ui->pending_extend);
    } else {
      float ty = cur.y + (float)dir * lh + lh * 0.5f;
      size_t pos = ui_composer_format_hit(ui, r, text, len, wrap, cur.x, ty);
      ccord_text_edit_set_caret(&ui->edit, pos, ui->pending_extend);
    }
    ui->caret_epoch_ms = ui->now_ms;
    ui->caret_on = true;
  }
  ccord_rect caret = ui_composer_format_caret(ui, r, text, len, wrap, ui->edit.caret);
  size_t plen = strlen(ui->ime_preedit);
  float pre_bottom = caret.y + lh;
  if (plen > 0) {
    pre_bottom = composer_preedit_walk(ui, r, ui->ime_preedit, plen, wrap, 0.0f, 0.0f,
                                       caret.x, caret.y, lh, caret.h, false);
    if (pre_bottom > total) total = pre_bottom;
  }
  if (caret.y < ui->composer_scroll) ui->composer_scroll = caret.y;
  if (pre_bottom > ui->composer_scroll + area.h) ui->composer_scroll = pre_bottom - area.h;
  ui->composer_scroll = clampf(ui->composer_scroll, 0.0f, maxf(total - area.h, 0.0f));
  // The platform text-input / IME candidate window follows the CARET, not the whole box.
  ui->text_area = (ccord_rect){area.x + caret.x, area.y - ui->composer_scroll + caret.y,
                               maxf(caret.w, 1.0f), caret.h};

  float clip_l = maxf(box.x, area.x - 4.0f);
  float clip_r = fminf(box.x + box.w, area.x + maxf(area.w, 0.0f) + 4.0f);
  ccord_push_clip(r, (ccord_rect){clip_l, box.y, maxf(clip_r - clip_l, 0.0f), box.h});
  const ccord_channel *ch = ui_selected_channel(ui);
  const char *locked = ui_composer_lock_reason(ui);
  if (len == 0 && plen == 0) {
    char ph[160];
    if (locked) {
      snprintf(ph, sizeof ph, "%s", locked);
    } else if (ui->d.session && ccord_session_is_stale(ui->d.session)) {
      snprintf(ph, sizeof ph, "Reconnecting\xe2\x80\xa6");   // saved data on screen: sending is refused until READY
    } else if (ch && ch->guild_id == CCORD_DM_GUILD_ID) {
      const ccord_user *u = ch->user_id != CCORD_ID_NONE ? ccord_store_user(ui->d.store, ch->user_id) : nullptr;
      const char *dname = (u && u->name && u->name[0]) ? u->name : ch->name;
      snprintf(ph, sizeof ph, "Message @%s", dname ? dname : "");
    } else {
      snprintf(ph, sizeof ph, "Message #%s", ch ? ch->name : "");
    }
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_placeholder));
    ccord_text_draw_ellipsis(r, ph, strlen(ph), area.x, area.y, area.w, &ps);
  } else if (len > 0) {
    size_t sel_lo = ui->edit.caret < ui->edit.anchor ? ui->edit.caret : ui->edit.anchor;
    size_t sel_hi = ui->edit.caret < ui->edit.anchor ? ui->edit.anchor : ui->edit.caret;
    ui_composer_format_draw(ui, r, text, len, wrap, area.x, area.y - ui->composer_scroll, sel_lo, sel_hi,
                            ui->composer_scroll - lh, ui->composer_scroll + area.h + lh);
  }
  // IME composition preedit: distinct colour with an underline, wrapped from the caret within [0, wrap].
  if (plen > 0) {
    (void)composer_preedit_walk(ui, r, ui->ime_preedit, plen, wrap, area.x, area.y - ui->composer_scroll,
                                caret.x, caret.y, lh, caret.h, true);
  }
  if (ui->caret_on && !ui->settings_open && !ui->menu.open) {
    float cy = area.y - ui->composer_scroll + caret.y;
    ccord_draw_rect(r, (ccord_rect){area.x + caret.x, cy + (lh - caret.h) * 0.5f, 1.5f, caret.h}, COL(caret));
  }
  ccord_pop_clip(r);
  ui_expr_draw_composer_buttons(ui, r, box);
  bool mobile_send = ui->mobile_layout && !locked && (ui->edit.len > 0 || ui->staged_file_count > 0);
  if (mobile_send) {
    float sw = 40.0f, sh = 38.0f;
    ccord_rect sb = {box.x + box.w - sw - 5.0f, box.y + (box.h - sh) * 0.5f, sw, sh};
    ui->mobile_send_btn_rect = sb;
    ccord_draw_rrect(r, sb, sh * 0.5f, COL(brand));
    // Crisp send arrow icon centered in the pill.
    float cx = sb.x + sb.w * 0.5f, cy = sb.y + sb.h * 0.5f;
    ccord_draw_line(r, (ccord_vec2){cx - 5.5f, cy}, (ccord_vec2){cx + 5.0f, cy}, 2.0f, COL(text_on_brand));
    ccord_draw_line(r, (ccord_vec2){cx + 1.0f, cy - 4.5f}, (ccord_vec2){cx + 5.5f, cy}, 2.0f, COL(text_on_brand));
    ccord_draw_line(r, (ccord_vec2){cx + 1.0f, cy + 4.5f}, (ccord_vec2){cx + 5.5f, cy}, 2.0f, COL(text_on_brand));
  } else if (!locked) {
    ui_vm_draw_mic(ui, r, box);   // rightmost; the emoji button(s) sit left of it
  }
  if (editing) {
    const char *hint = ui->mobile_layout ? "Editing message (press Back to cancel)"
                                         : "Editing message - Enter saves, Esc cancels";
    ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    ccord_text_draw_ellipsis(r, hint, strlen(hint), box.x + 2.0f, box.y + box.h + 5.0f, box.w, &hs);
  } else if (ch && ch->rate_limit_per_user > 0) {
    uint32_t rem_s = ui_slowmode_remaining_s(ui, ch);
    char shint[96];
    ccord_color scol = COL(text_muted);
    if (rem_s > 0) {
      snprintf(shint, sizeof shint, "Slowmode: %us remaining", (unsigned)rem_s);
      scol = COL(status_idle);
    } else {
      bool immune = false;
      if (ui->d.store && ch->guild_id != CCORD_ID_NONE && ch->guild_id != CCORD_DM_GUILD_ID) {
        ccord_id self = ccord_store_self_id(ui->d.store);
        if (ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) {
          uint64_t p = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
          uint64_t bypass = CCORD_PERM_BYPASS_SLOWMODE | CCORD_PERM_MANAGE_CHANNELS | CCORD_PERM_MANAGE_MESSAGES;
          immune = (p & CCORD_PERM_ADMINISTRATOR) != 0 || (p & bypass) != 0;
        }
      }
      if (immune) {
        snprintf(shint, sizeof shint, "Slowmode is enabled (%us \xE2\x80\x94 immune)",
                 (unsigned)ch->rate_limit_per_user);
      } else {
        snprintf(shint, sizeof shint, "Slowmode is enabled (%us)", (unsigned)ch->rate_limit_per_user);
      }
    }
    ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, scol);
    ccord_text_draw(r, shint, strlen(shint), box.x + 2.0f, box.y + box.h + 4.0f, &hs, box.w);
  }
}

// ---------------------------------------------------------------- sending

static bool is_blank(const char *s, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    if (s[i] != ' ' && s[i] != '\n' && s[i] != '\t') return false;
  }
  return true;
}

const char *ui_composer_lock_reason(const ccord_ui *ui)
{
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui->d.store || !ui_channel_has_chat(ch)) return nullptr;
  if (ch->guild_id == CCORD_DM_GUILD_ID) {
    if (ch->is_message_request) return "Accept this message request to reply.";
    return nullptr;
  }
  if (ui_role_preview_active_for(ui, ch->guild_id)) {
    uint64_t perms = ui_effective_permissions(ui, ch->guild_id, ch->id);
    bool thread = ccord_discord_type_is_thread(ch->discord_type);
    uint64_t need = CCORD_PERM_VIEW_CHANNEL | (thread ? CCORD_PERM_SEND_MESSAGES_IN_THREADS : CCORD_PERM_SEND_MESSAGES);
    if (ccord_perm_has(perms, need)) return nullptr;
    return "Selected role does not have permission to send messages in this channel.";
  }
  // A pending member still holds the roles' permissions locally, so this must answer before the permission check.
  if (ui_self_pending(ui, ch->guild_id)) return "Complete the server rules to send messages.";
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) return nullptr;
  uint64_t perms = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
  bool thread = ccord_discord_type_is_thread(ch->discord_type);
  uint64_t need = CCORD_PERM_VIEW_CHANNEL | (thread ? CCORD_PERM_SEND_MESSAGES_IN_THREADS : CCORD_PERM_SEND_MESSAGES);
  if (ccord_perm_has(perms, need)) return nullptr;
  const ccord_member *me = ccord_store_find_member(ui->d.store, ch->guild_id, self);
  if (me && me->timeout_until_ms > ui->now_unix_ms) return "You have been timed out and cannot send messages.";
  return "You do not have permission to send messages in this channel.";
}

void ui_send_composer(ccord_ui *ui)
{
  const char *lock = ui_composer_lock_reason(ui);
  if (lock) {
    ui_notice(ui, "%s", lock);
    return;
  }
  const char *t = ccord_text_edit_cstr(&ui->edit);
  size_t len = ui->edit.len;
  bool has_files = ui->staged_file_count > 0;
  if (is_blank(t, len) && !has_files) return;   // Enter on an empty composer sends nothing, like the real client
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) {
    ui_notice(ui, "Select a text channel first");
    return;
  }
  ccord_error err;
  if (ui->editing_message != CCORD_ID_NONE && !has_files) {
    if (!ui->d.session) {
      ui_notice(ui, "Not connected");
      return;
    }
    char exp_buf[CCORD_SEND_MAX_BYTES + 1];
    char ser_buf[CCORD_SEND_MAX_BYTES + 1];
    if (len > 0 && memchr(t, ':', len) != nullptr) {
      len = ui_expr_replace_shortcodes(ui->d.store, ch->guild_id, t, len, exp_buf, sizeof exp_buf);
      t = exp_buf;
    }
    if (len > 0 && (memchr(t, '@', len) != nullptr || memchr(t, '#', len) != nullptr)) {
      len = ccord_ui_serialize_mentions(ui->d.store, ch->guild_id, t, len, ser_buf, sizeof ser_buf);
      t = ser_buf;
    }
    char hook_edit_buf[CCORD_SEND_MAX_BYTES + 1] = {0};
    char hook_cancel[128] = {0};
    if (ui->d.contrib &&
        !ccord_contrib_run_before_edit(ui->d.contrib, ui->editing_channel, ui->editing_message, t,
                                       hook_edit_buf, sizeof hook_edit_buf, hook_cancel, sizeof hook_cancel)) {
      ui_notice(ui, "%s", hook_cancel[0] ? hook_cancel : "Edit cancelled by plugin");
      return;
    }
    if (hook_edit_buf[0]) {
      t = hook_edit_buf;
      len = strlen(hook_edit_buf);
    }
    if (ccord_session_edit_message(ui->d.session, ui->editing_channel, ui->editing_message, t, len, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot edit: %s", err.message);
      return;
    }
    ui->editing_channel = ui->editing_message = CCORD_ID_NONE;
    ccord_text_edit_clear(&ui->edit);
    ui->composer_scroll = 0.0f;
    ui->dirty = true;
    return;
  }

  // Slash commands (`/name ...`) when not uploading files or replying
  char builtin_tx[CCORD_SEND_MAX_BYTES + 1];
  if (!has_files && ui->reply_message == CCORD_ID_NONE && len > 0 && t[0] == '/' &&
      memchr(t, '\n', len) == nullptr) {
    size_t n = strcspn(t + 1, " \t\n");
    if (n > 0 && n <= CCORD_SLASH_NAME_MAX_BYTES) {
      char name[64];
      memcpy(name, t + 1, n);
      name[n] = '\0';
      if (strcasecmp(name, "tts") == 0) {
        // /tts sends a text-to-speech message; the session enforces SEND_TTS_MESSAGES.
        const char *body = t + 1 + n;
        while (*body == ' ' || *body == '\t') body++;
        size_t blen = strlen(body);
        if (blen == 0) {
          ui_notice(ui, "/tts needs a message to speak");
          return;
        }
        if (!ui->d.session) {
          ui_notice(ui, "Not connected");
          return;
        }
        uint32_t tts_rem = ui_slowmode_remaining_s(ui, ch);
        if (tts_rem > 0) {
          ui_notice(ui, "Slowmode active: wait %us", (unsigned)tts_rem);
          return;
        }
        char tts_exp[CCORD_SEND_MAX_BYTES + 1];
        char tts_ser[CCORD_SEND_MAX_BYTES + 1];
        if (memchr(body, ':', blen) != nullptr) {
          blen = ui_expr_replace_shortcodes(ui->d.store, ch->guild_id, body, blen, tts_exp, sizeof tts_exp);
          body = tts_exp;
        }
        if (memchr(body, '@', blen) != nullptr || memchr(body, '#', blen) != nullptr) {
          blen = ccord_ui_serialize_mentions(ui->d.store, ch->guild_id, body, blen, tts_ser, sizeof tts_ser);
          body = tts_ser;
        }
        if (ccord_session_send_tts_message(ui->d.session, ch->id, body, blen, &err) != CCORD_OK) {
          ui_notice(ui, "Cannot send TTS: %s", err.message);
          return;
        }
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui_record_slowmode_sent(ui, ch);
        ui_dismiss_new_marker(ui, false);
        ui->msg_scroll_vel = 0.0f;
        ccord_vlist_reset_bottom(&ui->vlist);
        ui->composer_scroll = 0.0f;
        ui->dirty = true;
        return;
      }
      if (strcasecmp(name, "gif") == 0 || strcasecmp(name, "sticker") == 0) {
        const char *arg = t + 1 + n;
        while (*arg == ' ' || *arg == '\t') arg++;
        int tab = strcasecmp(name, "gif") == 0 ? 0 : 1;   // EXPR_TAB_GIFS / EXPR_TAB_STICKERS
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui->composer_scroll = 0.0f;
        ui_expr_picker_open_query(ui, tab, ch->id, arg, strlen(arg));
        return;
      }
      if (strcasecmp(name, "schedule") == 0) {
        const char *arg = t + 1 + n;
        while (*arg == ' ' || *arg == '\t') arg++;
        char body_copy[CCORD_SEND_MAX_BYTES + 1];
        snprintf(body_copy, sizeof body_copy, "%s", arg);
        ccord_text_edit_clear(&ui->edit);
        if (body_copy[0] != '\0') (void)ccord_text_edit_insert(&ui->edit, body_copy, strlen(body_copy), &err);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui->composer_scroll = 0.0f;
        ui_schedule_dialog_open(ui);
        return;
      }
      if (strcasecmp(name, "nick") == 0) {
        if (ch->guild_id == CCORD_DM_GUILD_ID) {
          ui_notice(ui, "/nick can only be used in a server");
          return;
        }
        const char *arg = t + 1 + n;
        while (*arg == ' ' || *arg == '\t') arg++;
        ccord_admin_request nreq;
        ccord_id self_id = ccord_store_self_id(ui->d.store);
        if (ccord_admin_set_nick(&nreq, ch->guild_id, self_id, true, arg, nullptr, &err) != CCORD_OK) {
          ui_notice(ui, "Cannot change nickname: %s", err.message);
          return;
        }
        ui_admin_submit(ui, &nreq);
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui->composer_scroll = 0.0f;
        ui_notice(ui, arg[0] ? "Nickname updated" : "Nickname cleared");
        ui->dirty = true;
        return;
      }
      // Built-in text-transform commands (/shrug, /tableflip, /unflip, /me, /spoiler):
      // rewrite the outgoing text in place and fall through to normal send (preserving slowmode, plugins, shortcodes).
      bool transformed = false;
      const char *arg = t + 1 + n;
      while (*arg == ' ' || *arg == '\t') arg++;
      if (strcasecmp(name, "shrug") == 0) {
        (void)snprintf(builtin_tx, sizeof builtin_tx, "%s%s\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF", arg, arg[0] ? " " : "");
        transformed = true;
      } else if (strcasecmp(name, "tableflip") == 0) {
        (void)snprintf(builtin_tx, sizeof builtin_tx, "%s%s(\xE2\x95\xAF\xC2\xB0\xE2\x96\xA1\xC2\xB0)\xE2\x95\xAF\xEF\xB8\xB5 \xE2\x94\xBB\xE2\x94\x81\xE2\x94\xBB", arg, arg[0] ? " " : "");
        transformed = true;
      } else if (strcasecmp(name, "unflip") == 0) {
        (void)snprintf(builtin_tx, sizeof builtin_tx, "%s%s\xE2\x94\xAC\xE2\x94\x80\xE2\x94\xAC\xE3\x83\x8E( \xC2\xBA _ \xC2\xBA\xE3\x83\x8E)", arg, arg[0] ? " " : "");
        transformed = true;
      } else if (strcasecmp(name, "me") == 0) {
        if (arg[0] == '\0') { ui_notice(ui, "/me needs a message"); return; }
        (void)snprintf(builtin_tx, sizeof builtin_tx, "*%s*", arg);
        transformed = true;
      } else if (strcasecmp(name, "spoiler") == 0) {
        if (arg[0] == '\0') { ui_notice(ui, "/spoiler needs a message"); return; }
        (void)snprintf(builtin_tx, sizeof builtin_tx, "||%s||", arg);
        transformed = true;
      }
      if (transformed) {
        t = builtin_tx;
        len = strlen(builtin_tx);
      } else {
      const ccord_slash_command *cmd = dc_find_slash_cmd(ui, ch->guild_id, name, n);
      if (cmd) {
        if (!ui->d.session) {
          ui_notice(ui, "Not connected");
          return;
        }
        if (!dc_slash_cmd_allowed(ui, ch, cmd)) {
          ui_notice(ui, "You do not have permission to use /%s", cmd->name);
          return;
        }
        slash_parsed sp;
        dc_slash_parse_args(cmd, t, len, &sp);
        if (sp.has_sub_tree && sp.sub_cmd_idx < 0) {
          ui_notice(ui, "/%s requires a subcommand", cmd->name);
          return;
        }
        const char *opt_names[CCORD_SLASH_MAX_OPTIONS];
        uint8_t opt_kinds[CCORD_SLASH_MAX_OPTIONS];
        const char *opt_vals[CCORD_SLASH_MAX_OPTIONS];
        size_t nopts = 0;
        for (size_t i = 0; i < cmd->option_count; i++) {
          if (!dc_slash_opt_in_scope(cmd, i, &sp)) continue;
          if (!sp.opt_set[i] || (sp.opt_vals[i][0] == '\0' && cmd->options[i].kind != CCORD_SLASH_OPT_STRING)) {
            if (cmd->options[i].required) {
              ui_notice(ui, "/%s requires option '%s'", cmd->name, cmd->options[i].name);
              return;
            }
            continue;
          }
          if (cmd->options[i].required && sp.opt_vals[i][0] == '\0') {
            ui_notice(ui, "/%s requires option '%s'", cmd->name, cmd->options[i].name);
            return;
          }
          char verr[128] = {0};
          if (!dc_validate_slash_opt_val(ui->d.store, ch->guild_id, &cmd->options[i], sp.opt_vals[i], verr)) {
            ui_notice(ui, "/%s: %s", cmd->name, verr);
            return;
          }
          opt_names[nopts] = cmd->options[i].name;
          opt_kinds[nopts] = (uint8_t)cmd->options[i].kind;
          opt_vals[nopts] = sp.opt_vals[i];
          nopts++;
        }
        ccord_interaction_request ireq = {
          .type = CCORD_INTERACTION_APPLICATION_COMMAND,
          .application_id = cmd->application_id,
          .guild_id = ch->guild_id == CCORD_DM_GUILD_ID ? CCORD_ID_NONE : ch->guild_id,
          .channel_id = ch->id,
          .command_id = cmd->id,
          .command_version = cmd->version != CCORD_ID_NONE ? cmd->version : cmd->id,
          .command_name = cmd->name,
          .option_names = nopts > 0 ? opt_names : nullptr,
          .option_kinds = nopts > 0 ? opt_kinds : nullptr,
          .option_values = nopts > 0 ? opt_vals : nullptr,
          .option_count = nopts,
          .focused_option = -1,
          .command_type = 1u,
          .subcommand_group = sp.sub_group_idx >= 0 ? cmd->options[sp.sub_group_idx].name : nullptr,
          .subcommand = sp.sub_cmd_idx >= 0 ? cmd->options[sp.sub_cmd_idx].name : nullptr,
        };
        if (ccord_session_send_interaction(ui->d.session, &ireq, &err) != CCORD_OK) {
          ui_notice(ui, "/%s: %s", cmd->name, err.message);
          return;
        }
        ui->slash_auto_due_ms = 0;
        ccord_session_clear_slash_choices(ui->d.session);
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui_dismiss_new_marker(ui, false);
        ui->msg_scroll_vel = 0.0f;
        ccord_vlist_reset_bottom(&ui->vlist);
        ui->composer_scroll = 0.0f;
        ui->dirty = true;
        return;
      }
      // Dispatch plugin commands only after built-ins and Discord application commands (prevents hijacking).
      if (ui->d.contrib && ccord_contrib_find_command(ui->d.contrib, name)) {
        const char *args = t + 1 + n;
        while (*args == ' ' || *args == '\n') args++;
        if (ccord_contrib_run_command(ui->d.contrib, name, args, ch->guild_id, ch->id, &err) != CCORD_OK) {
          ui_notice(ui, "/%s: %s", name, err.message);
          return;
        }
        ccord_text_edit_clear(&ui->edit);
        ui_clear_channel_draft(ui, ch->id);
        ui->autocomplete_open = false;
        ui->nautocomplete = 0;
        ui->dirty = true;
        return;
      }
      }
    }
  }

  uint32_t rem_s = ui_slowmode_remaining_s(ui, ch);
  if (rem_s > 0) {
    ui_notice(ui, "Slowmode active: wait %us", (unsigned)rem_s);
    return;
  }

  char expanded_buf[CCORD_SEND_MAX_BYTES + 1];
  char serial_buf[CCORD_SEND_MAX_BYTES + 1];
  if (len > 0 && t[0] != '/' && memchr(t, ':', len) != nullptr) {
    len = ui_expr_replace_shortcodes(ui->d.store, ch->guild_id, t, len, expanded_buf, sizeof expanded_buf);
    t = expanded_buf;
  }
  if (len > 0 && (memchr(t, '@', len) != nullptr || memchr(t, '#', len) != nullptr)) {
    len = ccord_ui_serialize_mentions(ui->d.store, ch->guild_id, t, len, serial_buf, sizeof serial_buf);
    t = serial_buf;
  }
  char hook_send_buf[CCORD_SEND_MAX_BYTES + 1] = {0};
  char hook_cancel[128] = {0};
  if (ui->d.contrib &&
      !ccord_contrib_run_before_send(ui->d.contrib, ch->id,
                                     (ui->reply_message != CCORD_ID_NONE && ui->reply_channel == ch->id)
                                       ? ui->reply_message : CCORD_ID_NONE,
                                     t, hook_send_buf, sizeof hook_send_buf, hook_cancel, sizeof hook_cancel)) {
    ui_notice(ui, "%s", hook_cancel[0] ? hook_cancel : "Message cancelled by plugin");
    return;
  }
  if (hook_send_buf[0]) {
    t = hook_send_buf;
    len = strlen(hook_send_buf);
  }
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return;
  }
  if (has_files) {
    ccord_upload_file ufiles[CCORD_RICH_MAX_ATTACHMENTS];
    for (size_t i = 0; i < ui->staged_file_count; i++) {
      ufiles[i] = (ccord_upload_file){
        .filename = ui->staged_files[i].filename,
        .data = ui->staged_files[i].data,
        .size = ui->staged_files[i].size,
        .spoiler = ui->staged_files[i].spoiler,
      };
    }
    ccord_id rep_id = (ui->reply_message != CCORD_ID_NONE && ui->reply_channel == ch->id)
                        ? ui->reply_message
                        : CCORD_ID_NONE;
    if (ccord_session_send_message_with_files(ui->d.session, ch->id, rep_id, ui->reply_ping,
                                              t, len, ufiles, ui->staged_file_count, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot send attachment: %s", err.message);
      return;
    }
    ccord_ui_clear_attachment(ui);
    ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
    ui->reply_ping = true;
    ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
    ccord_text_edit_clear(&ui->edit);
    ui_clear_channel_draft(ui, ch->id);
    ui->autocomplete_open = false;
    ui->nautocomplete = 0;
    ui_record_slowmode_sent(ui, ch);
    ui_dismiss_new_marker(ui, false);
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
    ui->composer_scroll = 0.0f;
    ui->dirty = true;
    return;
  }
  if (ui->reply_message != CCORD_ID_NONE) {
    if (ccord_session_send_reply_ex(ui->d.session, ui->reply_channel, ui->reply_message, ui->reply_ping,
                                    t, len, &err) != CCORD_OK) {
      ui_notice(ui, "Cannot reply: %s", err.message);
      return;
    }
    ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
    ui->reply_ping = true;
    ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
    ccord_text_edit_clear(&ui->edit);
    ui_clear_channel_draft(ui, ch->id);
    ui->autocomplete_open = false;
    ui->nautocomplete = 0;
    ui_record_slowmode_sent(ui, ch);
    ui_dismiss_new_marker(ui, false);
    ui->msg_scroll_vel = 0.0f;
    ccord_vlist_reset_bottom(&ui->vlist);
    ui->composer_scroll = 0.0f;
    ui->dirty = true;
    return;
  }
  if (ccord_session_send_message(ui->d.session, ch->id, t, len, &err) != CCORD_OK) {
    ui_notice(ui, "Cannot send: %s", err.message);
    return;
  }
  ccord_text_edit_clear(&ui->edit);
  ui_clear_channel_draft(ui, ch->id);
  ui->autocomplete_open = false;
  ui->nautocomplete = 0;
  ui_record_slowmode_sent(ui, ch);
  ui_dismiss_new_marker(ui, false);
  ui->msg_scroll_vel = 0.0f;
  ccord_vlist_reset_bottom(&ui->vlist);   // follow the echoed message when it arrives
  ui->composer_scroll = 0.0f;
  ui->dirty = true;
}

void ui_send_typing(ccord_ui *ui)
{
  if (!ui->d.session || ui->editing_message != CCORD_ID_NONE) return;
  if (ui->edit.len > 0 && ui->edit.data[0] == '/') return;   // typing a slash command does not broadcast typing
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch || !ui_channel_has_chat(ch)) return;
  if (ui_slowmode_remaining_s(ui, ch) > 0) return;
  uint64_t now = ui->now_ms;
  if (ui->typing_channel == ch->id && now >= ui->last_typing_ms &&
      now - ui->last_typing_ms < UI_TYPING_THROTTLE_MS) {
    return;
  }
  ccord_error err;
  if (ccord_session_send_typing(ui->d.session, ch->id, &err) == CCORD_OK) {
    ui->typing_channel = ch->id;
    ui->last_typing_ms = now;
  }
}

void ui_begin_edit(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  const ccord_message *m = ccord_store_find_message(ui->d.store, channel_id, message_id);
  if (!m) {
    ui_notice(ui, "Message is no longer available");
    return;
  }
  char clean_buf[CCORD_MESSAGE_MAX_BYTES + 1];
  size_t len = ui_message_clean_body(ui, m, clean_buf, sizeof clean_buf);
  if (len > ui->edit.max_bytes) {
    ui_notice(ui, "Message is too long to edit here");
    return;
  }
  ccord_text_edit_clear(&ui->edit);
  ccord_error err;
  if (ccord_text_edit_insert(&ui->edit, clean_buf, len, &err) != CCORD_OK) {
    ui_notice(ui, "%s", err.message);
    return;
  }
  ccord_text_edit_set_caret(&ui->edit, ui->edit.len, false);
  ui->editing_channel = channel_id;
  ui->editing_message = message_id;
  ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
  ui->composer_scroll = 0.0f;
  ui->caret_epoch_ms = ui->now_ms;
  ui->caret_on = true;
  ui->dirty = true;
}

void ui_cancel_edit(ccord_ui *ui)
{
  if (ui->editing_message == CCORD_ID_NONE) return;
  ui->editing_channel = ui->editing_message = CCORD_ID_NONE;
  ccord_text_edit_clear(&ui->edit);
  ui->composer_scroll = 0.0f;
  ui->dirty = true;
}

void ui_begin_reply(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  const ccord_message *m = ccord_store_find_message(ui->d.store, channel_id, message_id);
  if (!m) {
    ui_notice(ui, "Message is no longer available");
    return;
  }
  ui_cancel_edit(ui);
  ui->reply_channel = channel_id;
  ui->reply_message = message_id;
  ui->reply_ping = true;
  ui->dirty = true;
}

void ui_cancel_reply(ccord_ui *ui)
{
  if (ui->reply_message == CCORD_ID_NONE) return;
  ui->reply_channel = ui->reply_message = CCORD_ID_NONE;
  ui->reply_ping = true;
  ui->reply_ping_rect = (ccord_rect){0, 0, 0, 0};
  ui->dirty = true;
}

