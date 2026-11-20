// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "settings_internal.h"

// ---- drawing helpers ----

void st_label_value(ccord_ui *ui, ccord_renderer *r, float x, float *y, float w, const char *label, const char *value)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_style vs = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_body, COL(text_normal));
  ccord_text_draw(r, label, strlen(label), x, *y, &ls, 0.0f);
  *y += ui_line_h(ui, ls.size) + 2.0f;
  ccord_text_draw_ellipsis(r, value, strlen(value), x, *y, w, &vs);
  *y += ui_line_h(ui, vs.size) + 14.0f;
}

static void draw_row_label(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label)
{
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
  ccord_text_draw(r, label, strlen(label), g->content.x, st_row_y(g, row), &ls, 0.0f);
}

static void draw_chip_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, int count,
                          const char *const *labels, int selected)
{
  for (int i = 0; i < count; i++) {
    ccord_rect o = st_chips_rect(g, row, i, count);
    uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, o), .selected = i == selected};
    uint64_t cid = (uint64_t)ui->settings_page * 4096u + (uint64_t)row * 64u + (uint64_t)i;
    uis_chip(ui, r, o, labels[i], st, "chip", cid);
  }
}

static void draw_toggle_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label,
                            const char *note, bool on, bool dim)
{
  draw_row_label(ui, r, g, row, label);
  if (note && note[0]) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
    ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, st_row_y(g, row) + 18.0f,
                             g->content.w - 60.0f, &ns);
  }
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, st_toggle_rect(g, row)), .disabled = dim};
  uis_toggle(ui, r, st_row_rect(g, row), on, st, "swset", (uint64_t)ui->settings_page * 64u + (uint64_t)row);
}

static void draw_device_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label,
                            const char *device)
{
  draw_row_label(ui, r, g, row, label);
  ccord_rect d = st_device_rect(g, row);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, d)};
  uis_select(ui, r, d, device, st, "sdev", (uint64_t)ui->settings_page * 64u + (uint64_t)row);
}

static void draw_volume_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label, int value,
                            const char *suffix, bool dim)
{
  draw_row_label(ui, r, g, row, label);
  ccord_rect minus = st_vol_minus_rect(g, row), plus = st_vol_plus_rect(g, row), track = st_vol_track_rect(g, row);
  uint64_t vid = (uint64_t)ui->settings_page * 64u + (uint64_t)row;
  uis_state bst = {.hover = ui_in_rect(ui->mx, ui->my, minus), .disabled = dim};
  uis_button(ui, r, minus, "-", UIS_BTN_SECONDARY, UIS_BTN_S_SM, bst, "volm", vid);
  bst.hover = ui_in_rect(ui->mx, ui->my, plus);
  uis_button(ui, r, plus, "+", UIS_BTN_SECONDARY, UIS_BTN_S_SM, bst, "volp", vid);
  uis_state sst = {.disabled = dim};
  uis_slider(ui, r, track, (float)value / 100.0f, sst);
  char vs[24];
  snprintf(vs, sizeof vs, "%d%s", value, suffix);
  ccord_text_style vsl = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  ccord_text_draw(r, vs, strlen(vs), plus.x + plus.w + 12.0f, st_row_y(g, row) + 26.0f, &vsl, 0.0f);
}

// ---- pages ----

const char *st_state_name(ccord_plugin_state s)
{
  switch (s) {
    case CCORD_PLUGIN_STARTING: return "Starting";
    case CCORD_PLUGIN_READY: return "Ready";
    case CCORD_PLUGIN_FAILED: return "Failed";
    case CCORD_PLUGIN_STOPPED: return "Stopped";
    case CCORD_PLUGIN_NEEDS_CONSENT: return "Needs Consent";
  }
  return "?";
}

static void draw_appearance(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_row_label(ui, r, g, 0, "MESSAGE DISPLAY");
  draw_chip_row(ui, r, g, 0, 2, st_k_display_mode, ui->settings_display_mode);

  // M6 theme chips + UI zoom slider + compact theme summary/swatches (rows 1-3).
  ui_settings_m6_appearance_draw(ui, r, g->content);

  draw_toggle_row(ui, r, g, 5, "CUSTOM THEME OVERRIDES", "Apply custom style overrides from settings.ini.",
                  ui->settings_quickcss_enabled, false);
  draw_toggle_row(ui, r, g, 7, "REDUCED MOTION", "Turn off fades, slides and other animations.",
                  ui->settings_reduced_motion, false);
  draw_row_label(ui, r, g, 6, "STYLE OVERRIDE SNIPPET");
  ccord_rect qbox = st_quickcss_box_rect(g);
  uis_state qst = {.focused = ui->account_focus_field == 6};
  uis_input(ui, r, qbox, &ui->quickcss_edit, "", qst, false, "squick", 6);
}

static const char *voice_conn_state_name(int state)
{
  switch (state) {
    case CCORD_VOICE_IDLE: return "Idle";
    case CCORD_VOICE_CONNECTING: return "Connecting";
    case CCORD_VOICE_HELLO: return "Hello";
    case CCORD_VOICE_READY: return "Ready";
    case CCORD_VOICE_ESTABLISHED: return "Established";
    case CCORD_VOICE_RECONNECTING: return "Reconnecting";
    case CCORD_VOICE_FAILED: return "Failed";
    default: return "Unknown";
  }
}

// Live RTC diagnostics + DAVE E2EE summary, read from the session (which mirrors the voice
// connection). Nothing is fabricated: outside voice the panel says so, and the "Verified" badge
// only appears when the DAVE backend actually reports a verified MLS handshake.
static void draw_voice_diagnostics(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  ccord_rect card = {g->content.x, st_row_y(g, VOICE_ROWS), g->content.w, VOICE_DIAG_H};
  uis_card(ui, r, card);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_caption, COL(text_muted));
  ccord_text_draw(r, "CONNECTION DIAGNOSTICS", 21, card.x + 12.0f, card.y + 10.0f, &hs, 0.0f);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_secondary));
  bool in_voice = ui->d.store && ccord_store_active_voice_channel(ui->d.store) != CCORD_ID_NONE;
  if (!in_voice) {
    const char *note = "Join a voice channel to see live connection diagnostics.";
    ccord_text_draw_ellipsis(r, note, strlen(note), card.x + 12.0f, card.y + 34.0f, card.w - 24.0f, &ns);
    return;
  }
  ccord_rtc_stats st;
  ccord_dave_status dv;
  ccord_session_voice_stats(ui->d.session, &st);
  ccord_session_dave_status(ui->d.session, &dv);
  char line[192];
  snprintf(line, sizeof line, "State: %s    Ping: %d ms    Bitrate: %u kbps", voice_conn_state_name(st.state),
           st.ping_ms, (unsigned)st.bitrate_kbps);
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 32.0f, card.w - 24.0f, &ns);
  snprintf(line, sizeof line, "Jitter: %zu    Lost packets: %llu    SSRC: %u", st.jitter_depth,
           (unsigned long long)st.packets_lost, (unsigned)st.ssrc);
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 52.0f, card.w - 24.0f, &ns);
  const char *enc = dv.active ? "End-to-end encrypted (DAVE)" : "Encryption: not active";
  ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_control, dv.active ? COL(status_online) : COL(text_muted));
  ccord_text_draw_ellipsis(r, enc, strlen(enc), card.x + 12.0f, card.y + 74.0f, card.w - 24.0f, &es);
  snprintf(line, sizeof line, "Protocol v%u    Epoch: %u%s", (unsigned)dv.protocol_version, (unsigned)st.dave_epoch,
           dv.verified ? "    Verified" : "");
  ccord_text_draw_ellipsis(r, line, strlen(line), card.x + 12.0f, card.y + 94.0f, card.w - 24.0f, &ns);
}

// Fixed scrollbar in the right margin (the page scrolls; the scrollbar does not).
static void draw_voice_scrollbar(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  float max = st_voice_max_scroll(g);
  if (max <= 0.0f) return;
  ccord_rect tr = st_voice_scroll_track(g), up = st_voice_scroll_up_rect(g), down = st_voice_scroll_down_rect(g);
  float th = st_voice_scroll_thumb_h(g);
  float ty = tr.y + (tr.h - th) * (st_s_voice_scroll / max);
  uis_scrollbar(ui, r, tr, (ccord_rect){tr.x, ty, tr.w, th});
  uis_state us = {.hover = ui_in_rect(ui->mx, ui->my, up)}, ds = {.hover = ui_in_rect(ui->mx, ui->my, down)};
  uis_scroll_arrow(ui, r, up, true, us, "vscroll", 0);
  uis_scroll_arrow(ui, r, down, false, ds, "vscroll", 1);
}

static void draw_voice(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  if (st_s_last_drawn_page != SP_VOICE) st_s_dev.valid = false;
  st_voice_devices_refresh(ui, false);
  draw_voice_scrollbar(ui, r, g);   // fixed, drawn against the unshifted st_geometry
  sgeom gv = *g;
  gv.content.y -= st_s_voice_scroll;
  ccord_push_clip(r, g->content);   // scrolled rows must not paint over the title

  // Rows 0-8 keep the original layout; M5 additions (camera, PTT key, sensitivity, diagnostics)
  // follow at rows 9-12, reached by scrolling.
  draw_device_row(ui, r, &gv, 0, "INPUT DEVICE", st_voice_input_name(ui));
  draw_device_row(ui, r, &gv, 1, "OUTPUT DEVICE", st_voice_output_name(ui));
  draw_volume_row(ui, r, &gv, 2, "INPUT VOLUME", ui->settings_input_volume, "%", false);
  draw_volume_row(ui, r, &gv, 3, "OUTPUT VOLUME", ui->settings_output_volume, "%", false);
  draw_row_label(ui, r, &gv, 4, "INPUT MODE");
  draw_chip_row(ui, r, &gv, 4, 2, st_k_voice_mode, ui->settings_voice_mode);
  bool dsp = ccord_dsp_available();
  const char *dsp_note = dsp ? nullptr : "Unavailable: this build has no speexdsp backend.";
  draw_toggle_row(ui, r, &gv, 5, "NOISE SUPPRESSION",
                  dsp ? "Reduces background noise picked up by your microphone." : dsp_note,
                  ui->settings_noise_suppression && dsp, !dsp);
  draw_toggle_row(ui, r, &gv, 6, "ECHO CANCELLATION",
                  dsp ? "Removes speaker echo picked up by the microphone." : dsp_note,
                  ui->settings_echo_cancel && dsp, !dsp);
  draw_toggle_row(ui, r, &gv, 7, "AUTOMATIC GAIN CONTROL",
                  dsp ? "Keeps your voice at a consistent level." : dsp_note,
                  ui->settings_auto_gain && dsp, !dsp);
  draw_toggle_row(ui, r, &gv, 8, "HARDWARE ACCELERATION", "Use the GPU for video encoding and rendering.",
                  ui->settings_hardware_accel, false);

  draw_device_row(ui, r, &gv, 9, "CAMERA", st_voice_camera_name(ui));
  // Push to Talk key: live capture. While armed the button prompts for a key press.
  char binding[64];
  if (st_s_ptt_armed) snprintf(binding, sizeof binding, "Press a key...");
  else st_ptt_binding_name(ui, binding, sizeof binding);
  draw_device_row(ui, r, &gv, 10, "PUSH TO TALK KEY", binding);
  bool vad = ui->settings_voice_mode == 0;
  draw_volume_row(ui, r, &gv, 11, "INPUT SENSITIVITY", ui->settings_vad_sensitivity, "", !vad);
  if (!vad) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
    const char *note = "Push to Talk is active; the mic opens while the bound key is held.";
    ccord_text_draw_ellipsis(r, note, strlen(note), gv.content.x, st_row_y(&gv, 11) + 40.0f, gv.content.w - 60.0f, &ns);
  }

  draw_voice_diagnostics(ui, r, &gv);
  ccord_pop_clip(r);
}

// The "Automatic" pill at the end of a streaming chip row: a chip, selected while the choice is Automatic.
static void draw_auto_pill(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, bool sel)
{
  ccord_rect ar = st_auto_pill_rect(g, row);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, ar), .selected = sel};
  uis_chip(ui, r, ar, "Automatic", st, "sauto", (uint64_t)row);
}

static void draw_streaming(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_row_label(ui, r, g, 0, "GPU / DISCORD ENCODER");
  draw_auto_pill(ui, r, g, 0, ui->settings_encoder == (int)CCORD_STREAM_ENCODER_AUTO);
  draw_chip_row(ui, r, g, 0, STREAM_ENC_COUNT, st_k_encoders, st_find_val_index(st_k_encoder_vals, STREAM_ENC_COUNT, ui->settings_encoder));
  draw_row_label(ui, r, g, 1, "CAPTURE FRAMEWORK");
  draw_auto_pill(ui, r, g, 1, ui->settings_framework == (int)CCORD_STREAM_FRAMEWORK_AUTO);
  draw_chip_row(ui, r, g, 1, STREAM_FW_COUNT, st_k_frameworks, st_find_val_index(st_k_framework_vals, STREAM_FW_COUNT, ui->settings_framework));
  draw_row_label(ui, r, g, 2, "RESOLUTION");
  draw_chip_row(ui, r, g, 2, 4, st_k_resolutions, ui->settings_resolution);
  draw_row_label(ui, r, g, 3, "FRAMERATE");
  draw_chip_row(ui, r, g, 3, 3, st_k_fps, ui->settings_fps);
  draw_row_label(ui, r, g, 4, "CODEC");
  draw_chip_row(ui, r, g, 4, 3, st_k_codecs, ui->settings_codec);
  const ccord_stream_state *st = ccord_store_stream(ui->d.store);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  char note[192];
  snprintf(note, sizeof note, "%s",
           st && st->active ? "A stream is live; changes apply to the next Go Live session."
                            : "Encoder and capture backend default to Automatic and are configured here.");
  ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, st_row_y(g, 5) + 4.0f, g->content.w, &ns);
}

static void draw_text_images(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_toggle_row(ui, r, g, 0, "INLINE EMBEDS", "Show link previews and embed cards in chat.", ui->settings_embeds, false);
  draw_toggle_row(ui, r, g, 1, "ATTACHMENT PREVIEWS", "Render image and file attachment cards.", ui->settings_attachments, false);
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  const char *note = "Turning a card off hides it locally; the message and its data stay in the channel.";
  ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, st_row_y(g, 2) + 8.0f, g->content.w, &ns);
}

// True while a left press that started on `rc` is recent and the pointer is still on it.
bool st_settings_pressed(const ccord_ui *ui, ccord_rect rc)
{
  return ui->now_ms < st_s_press_until_ms && ui_in_rect(st_s_press_x, st_s_press_y, rc) && ui_in_rect(ui->mx, ui->my, rc);
}

static void draw_keybind_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g, int row, const char *label, const char *key)
{
  draw_row_label(ui, r, g, row, label);
  ccord_rect b = st_keybind_rect(g, row);
  bool armed = row == 0 ? st_s_ptt_armed : (row >= 1 && row <= 3 && st_s_kb_armed == row - 1);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, b), .pressed = st_settings_pressed(ui, b), .focused = armed};
  uis_button(ui, r, b, key, UIS_BTN_SECONDARY, UIS_BTN_S_SM, st, "skey", (uint64_t)row);
}

static void draw_keybinds(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  char ptt[64], mute[64], deafen[64], qs[64];
  st_ptt_binding_name(ui, ptt, sizeof ptt);
  st_keybind_name(ui->settings_keybinds[0].scancode, ui->settings_keybinds[0].mods, mute, sizeof mute);
  st_keybind_name(ui->settings_keybinds[1].scancode, ui->settings_keybinds[1].mods, deafen, sizeof deafen);
  st_keybind_name(ui->settings_keybinds[2].scancode, ui->settings_keybinds[2].mods, qs, sizeof qs);
  if (st_s_ptt_armed) snprintf(ptt, sizeof ptt, "Press a key...");
  if (st_s_kb_armed == 0) snprintf(mute, sizeof mute, "Press a key...");
  if (st_s_kb_armed == 1) snprintf(deafen, sizeof deafen, "Press a key...");
  if (st_s_kb_armed == 2) snprintf(qs, sizeof qs, "Press a key...");
  draw_keybind_row(ui, r, g, 0, "PUSH TO TALK", ptt);
  draw_keybind_row(ui, r, g, 1, "TOGGLE MUTE", mute);
  draw_keybind_row(ui, r, g, 2, "TOGGLE DEAFEN", deafen);
  draw_keybind_row(ui, r, g, 3, "OPEN QUICK SWITCHER", qs);
  draw_keybind_row(ui, r, g, 4, "START / STOP STREAM", "Ctrl + Shift + L");
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  const char *note = "Click a key row, then press the new chord (modifiers included). Push to Talk is "
                     "captured on Voice & Video; changes persist in settings.ini.";
  ccord_text_draw_ellipsis(r, note, strlen(note), g->content.x, st_row_y(g, 5) + 8.0f, g->content.w, &ns);
  if (ui->d.contrib) {
    size_t npk = ccord_contrib_keybind_count(ui->d.contrib);
    for (size_t i = 0; i < npk && i < 8u; i++) {
      const ccord_keybind_contribution *kb = ccord_contrib_keybind_at(ui->d.contrib, i);
      if (!kb) continue;
      char lbl[128];
      snprintf(lbl, sizeof lbl, "%s (%s)", (kb->label && kb->label[0]) ? kb->label : kb->bind_id, kb->owner);
      const char *combo = (st_s_plugin_kb_armed == (int)i)
                            ? "Press a key..."
                            : ((kb->current_combo && kb->current_combo[0]) ? kb->current_combo : "Unbound");
      draw_keybind_row(ui, r, g, 6 + (int)i, lbl, combo);
    }
  }
}

static void draw_extensions_row(ccord_ui *ui, ccord_renderer *r, const sgeom *g)
{
  draw_row_label(ui, r, g, 0, "EXTENSIONS");
  float x = g->content.x, w = g->content.w;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_control, COL(text_muted));
  const char *note = ui->extensions_on ? "Turning off stops every extension and removes its menu items."
                                       : "Turning on starts the extensions again.";
  ccord_text_draw_ellipsis(r, note, strlen(note), x, st_row_y(g, 0) + 20.0f, w - 60.0f, &ns);
  ccord_rect t = st_extensions_toggle_rect(g);
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, t)};
  uis_toggle(ui, r, st_row_rect(g, 0), ui->extensions_on, st, "swext", 0);
}

// One sidebar scroll button: the shared scroll arrow (uis_scroll_arrow), with its own audit namespace.
static void sidebar_scroll_btn(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, bool up, uint64_t id)
{
  uis_state st = {.hover = ui_in_rect(ui->mx, ui->my, rc)};
  uis_scroll_arrow(ui, r, rc, up, st, "sbscroll", id);
}

void ui_settings_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui->settings_open) return;
  st_settings_init_defaults(ui);
  st_settings_fix_page(ui);
  if (ui_is_login_screen(ui)) {
    const ccord_theme *saved_theme = ui->theme;
    if (ui->d.protected_theme) ui->theme = ui->d.protected_theme;
    st_draw_login_screen(ui, r);
    ui->theme = saved_theme;
    return;
  }
  sgeom g = st_geometry(ui);
  ccord_draw_rect(r, g.panel, COL(bg_primary));
  if (g.sidebar.w > 0.0f) {
    ccord_draw_rect(r, g.sidebar, COL(bg_secondary));
    if (ui->mobile_layout) {
      ccord_text_style sts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
      ccord_text_draw(r, "Settings", 8, g.sidebar.x + UIS_SP_4, g.close.y + UIS_SP_1, &sts, 0.0f);
    }
    ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_overline, COL(text_muted));
    int last_cat = -1;
    int order[SP_COUNT];
    int norder = st_sidebar_order(order);
    st_s_drawn.count = 0;
    st_s_drawn.body = ui->settings_page;
    ccord_push_clip(r, g.list);
    for (int k = 0; k < norder; k++) {
      int i = order[k];
      if (st_k_pages[i].category != last_cat) {
        last_cat = st_k_pages[i].category;
        float cy = g.page[i].y - 20.0f;
        ccord_text_draw(r, st_k_categories[last_cat], strlen(st_k_categories[last_cat]), g.page[i].x + 6.0f, cy, &hs, 0.0f);
      }
      if (st_s_drawn.count < 24) {
        st_s_drawn.page[st_s_drawn.count] = i;
        snprintf(st_s_drawn.label[st_s_drawn.count], sizeof st_s_drawn.label[0], "%s", st_k_pages[i].label);
        st_s_drawn.rect[st_s_drawn.count] = g.page[i];
        st_s_drawn.count++;
      }
      uis_state st = {.hover = i == ui->hover_settings_page, .selected = !ui->mobile_layout && i == ui->settings_page};
      uis_list_row_fill(ui, r, g.page[i], st, "spage", (uint64_t)i);
      ccord_color tc = st.selected ? COL(text_interactive_active) : st.hover ? COL(text_interactive_hover) : COL(text_normal);
      if (!ui->mobile_layout && !st.selected && !st.hover) tc = COL(text_muted);
      ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, uis_tokens_of(ui).fs_body, tc);
      ccord_text_draw_ellipsis(r, st_k_pages[i].label, strlen(st_k_pages[i].label), g.page[i].x + 10.0f,
                               g.page[i].y + (g.page[i].h - ui_line_h(ui, ps.size)) * 0.5f, g.page[i].w - 16.0f, &ps);
      if (ui->mobile_layout) {
        ui_icon_chevron(r, g.page[i].x + g.page[i].w - UIS_SP_4, g.page[i].y + g.page[i].h * 0.5f, UIS_SP_3, ICON_RIGHT, COL(text_muted));
      }
    }
    ccord_pop_clip(r);
    if (g.sb_up.w > 0.0f) {
      sidebar_scroll_btn(ui, r, g.sb_up, true, 0);
      sidebar_scroll_btn(ui, r, g.sb_down, false, 1);
    }
  }

  if (g.back.w > 0.0f) {
    uis_button(ui, r, g.back, "< Back", UIS_BTN_SECONDARY, UIS_BTN_S_SM, (uis_state){0}, "sback", 0);
  }
  if (g.content.w > 0.0f) {
    ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, uis_tokens_of(ui).fs_title, COL(text_header));
    const char *title = st_k_pages[ui->settings_page].label;
    ccord_text_draw(r, title, strlen(title), g.content.x, g.content.y, &ts, 0.0f);
    ccord_push_clip(r, g.panel);
    ccord_push_alpha(r, ui_enter(ui, "spagebody", (uint64_t)ui->settings_page, 160));   // a page eases in when chosen
    switch (ui->settings_page) {
      case SP_ACCOUNT: st_draw_account(ui, r, &g); break;
      case SP_PROFILE_EDIT: ui_settings_profile_draw(ui, r, g.content, g.panel); break;
      case SP_APPEARANCE: draw_appearance(ui, r, &g); break;
      case SP_VOICE: draw_voice(ui, r, &g); break;
      case SP_STREAMING: draw_streaming(ui, r, &g); break;
      case SP_TEXT_IMAGES: draw_text_images(ui, r, &g); break;
      case SP_NOTIFICATIONS:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_NOTIFICATIONS, g.content, g.panel);
        break;
      case SP_ACCESSIBILITY:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_ACCESSIBILITY, g.content, g.panel);
        break;
      case SP_STREAMER:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_STREAMER, g.content, g.panel);
        break;
      case SP_LANGUAGE:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_LANGUAGE, g.content, g.panel);
        break;
      case SP_ADVANCED:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_ADVANCED, g.content, g.panel);
        break;
      case SP_CCORD:
        ui_settings_m6_prefs_draw(ui, r, CCORD_M6_PREFS_CCORD, g.content, g.panel);
        break;
      case SP_KEYBINDS: draw_keybinds(ui, r, &g); break;
      case SP_PRIVACY:
      case SP_DEVICES:
      case SP_CONNECTIONS:
        ui_settings_m6_draw(ui, r, ui->settings_page - SP_PRIVACY, g.content, g.panel);
        break;
      case SP_PLUGINS:
        draw_extensions_row(ui, r, &g);
        st_draw_plugins(ui, r, &g);
        break;
      default: break;
    }
    st_s_last_drawn_page = ui->settings_page;
    ccord_pop_alpha(r);
    ccord_pop_clip(r);
  }

  // Close control with an ESC badge (desktop only).
  uis_tokens tk = uis_tokens_of(ui);
  if (!ui->mobile_layout) {
    ccord_rect badge = {g.close.x - 34.0f, g.close.y + 8.0f, 26.0f, 16.0f};
    uis_plate(ui, r, badge, tk.r_control, COL(bg_tertiary));
    ccord_text_style es = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, COL(text_muted));
    ccord_vec2 et = ccord_text_measure(r, "ESC", 3, &es, 0.0f);
    ccord_text_draw(r, "ESC", 3, badge.x + (badge.w - et.x) * 0.5f, badge.y + (badge.h - et.y) * 0.5f, &es, 0.0f);
  }
  uis_state cst = {.hover = ui->hover_settings_close};
  ccord_color cc = uis_icon_button(ui, r, g.close, cst, "sclose", 0);
  ui_icon_close(r, g.close.x + g.close.w * 0.5f, g.close.y + g.close.h * 0.5f, UIS_ICON_GLYPH, cc);
  if (ui->settings_page == SP_ACCOUNT && ui->account_mfa_required) {
    st_draw_mfa_popup(ui, r);
  }
}

void ccord_ui_get_settings_debug(const ccord_ui *ui, ccord_ui_settings_debug *out)
{
  *out = (ccord_ui_settings_debug){0};
  if (!ui) return;
  sgeom g = st_geometry(ui);
  out->open = ui->settings_open;
  out->page = ui->settings_page;
  out->sidebar_count = st_s_drawn.count;
  for (int i = 0; i < st_s_drawn.count; i++) {
    out->sidebar[i].page = st_s_drawn.page[i];
    snprintf(out->sidebar[i].label, sizeof out->sidebar[i].label, "%s", st_s_drawn.label[i]);
    out->sidebar[i].rect = st_s_drawn.rect[i];
  }
  out->drawn_page = st_s_drawn.body;
  // The fixed-size snapshot predates the M6 pages: it keeps carrying the original nine tabs
  // (indices unchanged). The M6 tab rects come from ui_settings_m6_host_rects instead.
  out->page_count = SP_COUNT < 9 ? SP_COUNT : 9;
  for (int i = 0; i < out->page_count; i++) out->tabs[i] = g.page[i];
  for (int i = 0; i < 2; i++) out->display_mode[i] = st_chips_rect(&g, 0, i, 2);
  // Voice & Video rows scroll; report their drawn positions (shifted by the page offset).
  sgeom gv = g;
  if (ui->settings_page == SP_VOICE) gv.content.y -= st_s_voice_scroll;
  out->input_device = st_device_rect(&gv, 0);
  out->output_device = st_device_rect(&gv, 1);
  out->input_volume = st_vol_track_rect(&gv, 2);
  out->output_volume = st_vol_track_rect(&gv, 3);
  for (int i = 0; i < 2; i++) out->voice_mode[i] = st_chips_rect(&gv, 4, i, 2);
  for (int i = 0; i < 5; i++) {
    out->encoder[i] = st_chips_rect(&g, 0, i, 5);
    out->framework[i] = st_chips_rect(&g, 1, i, 5);
  }
  for (int i = 0; i < 3; i++) {
    out->codec[i] = st_chips_rect(&g, 4, i, 3);
    out->fps[i] = st_chips_rect(&g, 3, i, 3);
  }
  for (int i = 0; i < 4; i++) out->resolution[i] = st_chips_rect(&g, 2, i, 4);
  out->embed_toggle = st_toggle_rect(&g, 0);
  out->attachment_toggle = st_toggle_rect(&g, 1);
  out->hardware_toggle = st_toggle_rect(&gv, 8);
  out->plugins_switch = ui_settings_rect(ui, false);
  out->billing_btn = st_account_web_btn_rect(&g, 0);
  out->nitro_btn = st_account_web_btn_rect(&g, 1);
  out->shop_btn = st_account_web_btn_rect(&g, 2);
  out->boost_btn = st_account_web_btn_rect(&g, 3);
  out->quickcss_toggle = st_toggle_rect(&g, 5);
  out->reduced_motion_toggle = st_toggle_rect(&g, 7);
  out->notify_desktop_toggle = st_toggle_rect(&g, 3);
  for (int i = 0; i < 4; i++) out->notify_timeout[i] = st_chips_rect(&g, 4, i, 4);

  out->encoder_count = 5;
  out->codec_count = 3;
  out->framework_count = 5;
  out->resolution_count = 4;
  out->fps_count = 3;
  out->display_mode_value = ui->settings_display_mode;
  out->voice_mode_value = ui->settings_voice_mode;
  out->mic_device_value = ui->settings_mic_device;
  out->speaker_device_value = ui->settings_speaker_device;
  out->input_volume_value = ui->settings_input_volume;
  out->output_volume_value = ui->settings_output_volume;
  out->encoder_value = ui->settings_encoder;
  out->codec_value = ui->settings_codec;
  out->framework_value = ui->settings_framework;
  out->resolution_value = ui->settings_resolution;
  out->fps_value = ui->settings_fps;
  out->embeds = ui->settings_embeds;
  out->attachments = ui->settings_attachments;
  out->hardware_accel = ui->settings_hardware_accel;
  out->noise_suppression = ui->settings_noise_suppression;
  out->echo_cancel = ui->settings_echo_cancel;
  out->auto_gain = ui->settings_auto_gain;
  out->quickcss_enabled = ui->settings_quickcss_enabled;
  out->reduced_motion = ui->settings_reduced_motion;
  out->notify_desktop = ui->settings_notify_desktop;
  out->notify_timeout_ms = ui->settings_notify_timeout_ms;
  out->window_width = ui->settings_win_w;
  out->window_height = ui->settings_win_h;
  snprintf(out->quickcss, sizeof out->quickcss, "%.255s", ui->settings_quickcss);
  out->quickcss_box = st_quickcss_box_rect(&g);
  out->quickcss_focused = ui->settings_page == SP_APPEARANCE && ui->account_focus_field == 6;
  out->close_to_tray = ccord_ui_pref_close_to_background();
  out->close_to_tray_toggle = ui_settings_m6_close_tray_rect(ui);
  for (int i = 0; i < 4; i++) {
    out->keybind_rows[i] = st_keybind_rect(&g, i);
  }
  out->keybind_capture = st_s_ptt_armed ? 0 : (st_s_kb_armed >= 0 ? st_s_kb_armed + 1 : -1);
  out->saved_account_count = ui->saved_account_count;
  out->account_add_btn = st_account_add_btn_rect(&g);
  for (size_t i = 0; i < ui->saved_account_count && i < SAVED_ACCOUNT_ROWS; i++) {
    out->saved_account_ids[i] = ui->saved_accounts[i].user_id;
    out->saved_account_rows[i] = st_saved_account_row_rect(&g, (int)i);
    out->saved_account_switch[i] = st_saved_account_switch_rect(&g, (int)i);
    out->saved_account_remove[i] = st_saved_account_remove_rect(&g, (int)i);
  }
  out->account_token_box = st_account_token_box_rect(&g);
  out->account_token_paste_btn = st_account_token_paste_rect(&g);
  out->account_token_save_btn = st_account_token_save_rect(&g);
  out->account_token_clear_btn = st_account_token_clear_rect(&g);
  out->account_email_box = st_account_email_box_rect(&g);
  out->account_password_box = st_account_password_box_rect(&g);
  out->account_mfa_box = (ccord_rect){0};
  out->account_login_btn = st_account_login_btn_rect(&g);
  out->login_screen = ui_is_login_screen(ui);
  out->login_tab = ui->login_tab;
  out->account_focus_field = ui->account_focus_field;
  out->account_mfa_required = ui->account_mfa_required;
  snprintf(out->account_login_status, sizeof out->account_login_status, "%s", ui->account_login_status);
  if (out->login_screen) {
    ccord_rect card = st_login_card_rect(ui);
    out->login_card = card;
    out->login_tab_token = st_login_tab_rect(&card, 0);
    out->login_tab_credentials = st_login_tab_rect(&card, 1);
    out->account_token_box = st_login_token_box_rect(&card);
    out->account_token_paste_btn = st_login_token_paste_rect(&card);
    out->account_token_save_btn = st_login_token_save_rect(&card);
    out->account_email_box = st_login_email_box_rect(&card);
    out->account_password_box = st_login_password_box_rect(&card);
    out->account_login_btn = st_login_credentials_btn_rect(&card);
  }
  if (ui->account_mfa_required) {
    ccord_rect pop = st_mfa_popup_rect(ui);
    out->mfa_popup_rect = pop;
    out->account_mfa_box = st_mfa_popup_input_rect(&pop);
    out->account_login_btn = st_mfa_popup_verify_rect(&pop);
    out->mfa_back_btn = st_mfa_popup_back_rect(&pop);
  }
  out->account_token_focused = (ui->account_focus_field == 1);
  out->account_token_len = ui->account_token_edit.len;
  ui_settings_m6_debug(ui, out);   // M6 prefs seam (ui.c): local theme/zoom/streamer/notify mirrors
  // Governed account settings: the store is the single source of truth (REST answer / gateway
  // push). Report the confirmed object when it has arrived; the mirrors only carry Discord defaults
  // until then. Local prefs above (theme/zoom/streamer/notify_sounds/hardware_accel) stay mirror-based.
  const ccord_user_settings *us = ui->d.store ? ccord_store_user_settings(ui->d.store) : nullptr;
  if (us) {
    out->developer_mode = us->developer_mode;
    snprintf(out->locale, sizeof out->locale, "%.15s", us->locale);
    out->explicit_content_filter = us->explicit_content_filter;
    out->animate_stickers = us->animate_stickers;
    out->gif_auto_play = us->gif_auto_play;
    out->animate_emoji = us->animate_emoji;
    out->enable_tts_command = us->enable_tts_command;
    out->stream_notifications_enabled = us->stream_notifications_enabled;
    out->default_guilds_restricted = us->default_guilds_restricted;
    out->allow_age_restricted = us->allow_access_to_age_restricted_servers;
  }
}

// M5 test/introspection seam (m5-ui-settings): drawn rects of the Voice & Video controls that the
// fixed ccord_ui_settings_debug struct cannot carry, plus the page scroll state. Not declared in a
// public header; test_ui_voice_settings.c prototypes it. Rects reflect the current scroll offset.
void ui_settings_voice_m5_debug(const ccord_ui *ui, ccord_rect *out_camera, ccord_rect *out_ptt_key,
                                ccord_rect *out_sens_track, ccord_rect *out_scroll_up, ccord_rect *out_scroll_down,
                                ccord_rect *out_diag, float *out_scroll, float *out_max_scroll, bool *out_in_voice);
void ui_settings_voice_m5_debug(const ccord_ui *ui, ccord_rect *out_camera, ccord_rect *out_ptt_key,
                                ccord_rect *out_sens_track, ccord_rect *out_scroll_up, ccord_rect *out_scroll_down,
                                ccord_rect *out_diag, float *out_scroll, float *out_max_scroll, bool *out_in_voice)
{
  sgeom g = st_geometry(ui);
  sgeom gv = g;
  gv.content.y -= st_s_voice_scroll;
  if (out_camera) *out_camera = st_device_rect(&gv, 9);
  if (out_ptt_key) *out_ptt_key = st_device_rect(&gv, 10);
  if (out_sens_track) *out_sens_track = st_vol_track_rect(&gv, 11);
  if (out_scroll_up) *out_scroll_up = st_voice_scroll_up_rect(&g);
  if (out_scroll_down) *out_scroll_down = st_voice_scroll_down_rect(&g);
  if (out_diag) *out_diag = (ccord_rect){g.content.x, st_row_y(&gv, VOICE_ROWS), g.content.w, VOICE_DIAG_H};
  if (out_scroll) *out_scroll = st_s_voice_scroll;
  if (out_max_scroll) *out_max_scroll = st_voice_max_scroll(&g);
  if (out_in_voice) *out_in_voice = ui->d.store && ccord_store_active_voice_channel(ui->d.store) != CCORD_ID_NONE;
}

