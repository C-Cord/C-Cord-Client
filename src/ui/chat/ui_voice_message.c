// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Voice messages, composer side: the mic button, the recording bar and the Ogg/Opus encoder feed. The clip is
// encoded as it is recorded, so a long recording never sits in raw PCM. Nothing is dropped silently: the 10 minute cap
// ends the recording and sends it, an encoder error finishes what was already encoded, and a clip whose send fails
// stays in the bar until it is sent or the user discards it. Receive-side decoding of the waveform lives here too.
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/platform/platform.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>

#include "ui_internal.h"
#include "ui_style.h"

#define M(name) (ui->theme->m.name)
#define COL(name) (ui->theme->c.name)

#define UI_VM_TICK_FRAMES 4800u   // at most 100 ms of mic audio is read per UI tick

static void vm_free_peaks(ccord_ui *ui)
{
  if (ui->vm.peaks) ccord_free(ui->vm.peaks);
  ui->vm.peaks = nullptr;
  ui->vm.npeaks = 0;
}

static void vm_drop_encoder(ccord_ui *ui)
{
  if (ui->vm.enc) ccord_sound_encoder_destroy(ui->vm.enc);
  ui->vm.enc = nullptr;
}

static void vm_close_mic(ccord_ui *ui)
{
  if (ui->vm.mic) ccord_audio_destroy(ui->vm.mic);
  ui->vm.mic = nullptr;
}

static void vm_clear_clip(ccord_ui *ui)
{
  if (ui->vm.clip) ccord_free(ui->vm.clip);
  ui->vm.clip = nullptr;
  ui->vm.clip_len = 0;
  ui->vm.clip_ms = 0;
  ui->vm.wave[0] = '\0';
  ui->vm.reviewing = false;
}

static void vm_peak_add(ccord_ui *ui, int peak)
{
  if (ui->vm.npeaks >= UI_VM_MAX_PEAKS) return;
  ui->vm.peaks[ui->vm.npeaks++] = (uint8_t)((peak * 255) / 32767);
}

// One waveform peak per 20 ms block: the loudest sample of the block, scaled to a byte.
static void vm_note_samples(ccord_ui *ui, const int16_t *pcm, size_t frames)
{
  for (size_t i = 0; i < frames; i++) {
    int s = pcm[i];
    if (s < 0) s = -s;
    if (s > ui->vm.blk_peak) ui->vm.blk_peak = s;
    if (++ui->vm.blk_fill == UI_VM_PEAK_FRAMES) {
      vm_peak_add(ui, ui->vm.blk_peak);
      ui->vm.blk_fill = 0;
      ui->vm.blk_peak = 0;
    }
  }
}

// Discord's waveform: UI_VM_BARS bytes, each the loudest peak of its share of the recording.
static void vm_wave_bytes(const ccord_ui *ui, uint8_t out[UI_VM_BARS])
{
  size_t n = ui->vm.npeaks;
  for (size_t i = 0; i < UI_VM_BARS; i++) {
    uint8_t best = 0;
    if (n > 0) {
      size_t a = i * n / UI_VM_BARS;   // a < n: i < UI_VM_BARS
      size_t b = (i + 1) * n / UI_VM_BARS;
      if (b <= a) b = a + 1;
      if (b > n) b = n;
      for (size_t k = a; k < b; k++) {
        if (ui->vm.peaks[k] > best) best = ui->vm.peaks[k];
      }
    }
    out[i] = best;
  }
}

static const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard padded base64. Returns the length written, or 0 when `cap` is too small (out is then unset).
static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
  size_t need = ((n + 2) / 3) * 4;
  if (need + 1 > cap) return 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    size_t rem = n - i;
    uint32_t v = (uint32_t)in[i] << 16;
    if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
    if (rem > 2) v |= in[i + 2];
    out[o++] = b64_alphabet[(v >> 18) & 63u];
    out[o++] = b64_alphabet[(v >> 12) & 63u];
    out[o++] = rem > 1 ? b64_alphabet[(v >> 6) & 63u] : '=';
    out[o++] = rem > 2 ? b64_alphabet[v & 63u] : '=';
  }
  out[o] = '\0';
  return o;
}

static int b64_value(char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

size_t ui_vm_waveform_bars(const char *b64, uint8_t *out, size_t cap)
{
  if (!b64 || !out) return 0;
  uint32_t acc = 0;
  int bits = 0;
  size_t o = 0;
  for (const char *p = b64; *p && *p != '='; p++) {
    int v = b64_value(*p);
    if (v < 0) return 0;
    acc = ((acc << 6) | (uint32_t)v) & 0xFFFFu;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) break;   // a longer waveform than we draw: keep the first bars
      out[o++] = (uint8_t)((acc >> bits) & 0xFFu);
    }
  }
  return o;
}

static void fmt_mmss(uint64_t ms, char *out, size_t cap)
{
  unsigned s = (unsigned)(ms / 1000u);
  snprintf(out, cap, "%u:%02u", s / 60u, s % 60u);
}

static bool vm_begin(ccord_ui *ui, ccord_id channel)
{
  ccord_error err = {0};
  ccord_sound_encoder *enc = ccord_sound_encoder_create(UI_VM_RATE, 1, &err);
  if (!enc) {
    ui_notice(ui, "Voice messages are unavailable: %s", err.message);
    return false;
  }
  uint8_t *peaks = ccord_malloc(UI_VM_MAX_PEAKS);
  if (!peaks) {
    ccord_sound_encoder_destroy(enc);
    ui_notice(ui, "Voice message: out of memory.");
    return false;
  }
  ui->vm.enc = enc;
  ui->vm.peaks = peaks;
  ui->vm.npeaks = 0;
  ui->vm.frames = 0;
  ui->vm.blk_fill = 0;
  ui->vm.blk_peak = 0;
  ui->vm.channel = channel;
  ui->vm.recording = true;
  ui->dirty = true;
  return true;
}

// Encodes up to the cap and returns the frames taken. Reaching the cap, or an encoder error, ends the recording
// through ui_vm_finish, which keeps and sends what was encoded.
static size_t vm_push(ccord_ui *ui, const int16_t *pcm, size_t frames)
{
  if (!ui->vm.recording) return 0;
  uint64_t room = UI_VM_MAX_FRAMES - ui->vm.frames;
  if ((uint64_t)frames > room) frames = (size_t)room;
  if (frames == 0) {
    ui_vm_finish(ui);
    return 0;
  }
  ccord_error err = {0};
  if (ccord_sound_encoder_feed(ui->vm.enc, pcm, frames, &err) != CCORD_OK) {
    ui_notice(ui, "Voice message stopped early: %s", err.message);
    ui_vm_finish(ui);
    return 0;
  }
  ui->vm.frames += frames;
  vm_note_samples(ui, pcm, frames);
  ui->dirty = true;
  if (ui->vm.frames >= UI_VM_MAX_FRAMES) ui_vm_finish(ui);
  return frames;
}

bool ui_vm_is_active(const ccord_ui *ui)
{
  return ui->vm.recording || ui->vm.reviewing;
}

bool ui_vm_start(ccord_ui *ui)
{
  if (ui_vm_is_active(ui)) {
    ui_notice(ui, "Send or discard the voice message first.");
    return false;
  }
  ccord_id ch_id = ccord_store_selected_channel(ui->d.store);
  const ccord_channel *ch = ccord_store_channel(ui->d.store, ch_id);
  if (!ch || ch->kind != CCORD_CHANNEL_TEXT) {
    ui_notice(ui, "Open a text channel to record a voice message.");
    return false;
  }
  ccord_audio_config cfg = {.sample_rate = (int)UI_VM_RATE, .channels = 1,
                            .frame_samples = (int)UI_VM_PEAK_FRAMES, .capture = true, .device_id = ui->settings_mic_device_id[0] ? ui->settings_mic_device_id : nullptr};
  ccord_error err = {0};
  ccord_audio *mic = ccord_audio_create(&cfg, &err);
  if (!mic || !ccord_audio_available(mic)) {
    if (mic) ccord_audio_destroy(mic);
    ui_notice(ui, "No microphone is available for a voice message.");
    return false;
  }
  if (!vm_begin(ui, ch_id)) {
    ccord_audio_destroy(mic);
    return false;
  }
  ui->vm.mic = mic;
  return true;
}

bool ui_vm_test_begin(ccord_ui *ui)
{
  if (ui_vm_is_active(ui)) return false;
  return vm_begin(ui, ccord_store_selected_channel(ui->d.store));
}

size_t ui_vm_feed_test_pcm(ccord_ui *ui, const int16_t *pcm, size_t frames)
{
  return vm_push(ui, pcm, frames);
}

void ui_vm_tick(ccord_ui *ui)
{
  if (!ui->vm.recording) return;
  ui->dirty = true;   // the elapsed time and the live bars move every frame
  if (!ui->vm.mic) return;
  int16_t buf[UI_VM_TICK_FRAMES];
  size_t n = ccord_audio_read(ui->vm.mic, buf, UI_VM_TICK_FRAMES);
  if (n > 0) (void)vm_push(ui, buf, n);
}

bool ui_vm_send_clip(ccord_ui *ui)
{
  if (!ui->vm.reviewing || !ui->vm.clip) return false;
  ccord_error err = {0};
  ccord_status st = ccord_session_send_voice_message(ui->d.session, ui->vm.clip_channel, ui->vm.clip,
                                                     ui->vm.clip_len, ui->vm.clip_ms, ui->vm.wave, &err);
  ui->dirty = true;
  if (st != CCORD_OK) {
    ui_notice(ui, "Voice message not sent: %s. Send it again or discard it.", err.message);
    return false;
  }
  ui->vm.sent_count++;
  ui->vm.last_sent_ms = ui->vm.clip_ms;
  vm_clear_clip(ui);
  return true;
}

void ui_vm_finish(ccord_ui *ui)
{
  if (!ui->vm.recording) return;
  ui->vm.recording = false;
  vm_close_mic(ui);
  if (ui->vm.blk_fill > 0) {
    vm_peak_add(ui, ui->vm.blk_peak);
    ui->vm.blk_fill = 0;
    ui->vm.blk_peak = 0;
  }
  ui->dirty = true;
  if (ui->vm.frames == 0) {
    vm_drop_encoder(ui);
    vm_free_peaks(ui);
    ui_notice(ui, "Nothing was recorded.");
    return;
  }
  uint8_t *ogg = nullptr;
  size_t len = 0;
  ccord_error err = {0};
  ccord_status st = ccord_sound_encoder_finish(ui->vm.enc, &ogg, &len, &err);
  vm_drop_encoder(ui);
  if (st != CCORD_OK) {
    vm_free_peaks(ui);
    ui_notice(ui, "Could not finish the voice message: %s", err.message);
    return;
  }
  uint8_t wave[UI_VM_BARS];
  vm_wave_bytes(ui, wave);
  vm_free_peaks(ui);
  if (b64_encode(wave, UI_VM_BARS, ui->vm.wave, sizeof ui->vm.wave) == 0) ui->vm.wave[0] = '\0';
  uint64_t ms = ui->vm.frames * 1000u / UI_VM_RATE;
  ui->vm.clip = ogg;
  ui->vm.clip_len = len;
  ui->vm.clip_ms = ms > 0 ? (uint32_t)ms : 1u;
  ui->vm.clip_channel = ui->vm.channel;
  ui->vm.reviewing = true;
  (void)ui_vm_send_clip(ui);
}

void ui_vm_cancel(ccord_ui *ui)
{
  if (!ui->vm.recording) return;
  ui->vm.recording = false;
  vm_close_mic(ui);
  vm_drop_encoder(ui);
  vm_free_peaks(ui);
  ui->dirty = true;
  ui_notice(ui, "Voice message discarded.");
}

void ui_vm_destroy(ccord_ui *ui)
{
  vm_close_mic(ui);
  vm_drop_encoder(ui);
  vm_free_peaks(ui);
  vm_clear_clip(ui);
  ui->vm.recording = false;
}

ccord_rect ui_vm_mic_rect(ccord_rect box)
{
  return (ccord_rect){box.x + box.w - 8.0f - 26.0f, box.y + (box.h - 24.0f) * 0.5f, 26.0f, 24.0f};
}

bool ui_vm_handle_click(ccord_ui *ui, float x, float y)
{
  if (ui_vm_is_active(ui)) {
    if (ui_in_rect(x, y, ui->vm.send_rect)) {
      if (ui->vm.recording) ui_vm_finish(ui);
      else (void)ui_vm_send_clip(ui);
      return true;
    }
    if (ui_in_rect(x, y, ui->vm.cancel_rect)) {
      if (ui->vm.recording) ui_vm_cancel(ui);
      else vm_clear_clip(ui);
      ui->dirty = true;
      return true;
    }
    return ui_in_rect(x, y, ui->vm.bar_rect);   // the bar swallows clicks: the composer under it is not live
  }
  if (ui_in_rect(x, y, ui->vm.mic_rect)) {
    (void)ui_vm_start(ui);
    ui->dirty = true;
    return true;
  }
  return false;
}

void ui_vm_draw_mic(ccord_ui *ui, ccord_renderer *r, ccord_rect box)
{
  ccord_rect rc = ui_vm_mic_rect(box);
  ui->vm.mic_rect = rc;
  bool hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, rc);
  // The glyph eases between the two greys (ui_fade), so it does not snap on the pointer.
  ccord_color c = ui_state_mix(ui, "vmmic", 0, hover, false, COL(text_secondary), COL(text_header), COL(text_secondary));
  float cx = rc.x + rc.w * 0.5f;
  float cy = rc.y + rc.h * 0.5f;
  ccord_draw_rrect(r, (ccord_rect){cx - 3.5f, cy - 7.0f, 7.0f, 10.0f}, 3.5f, c);   // capsule
  ccord_draw_rect(r, (ccord_rect){cx - 5.5f, cy - 1.0f, 1.2f, 4.0f}, c);            // cradle
  ccord_draw_rect(r, (ccord_rect){cx + 4.3f, cy - 1.0f, 1.2f, 4.0f}, c);
  ccord_draw_rect(r, (ccord_rect){cx - 5.5f, cy + 2.8f, 11.0f, 1.2f}, c);
  ccord_draw_rect(r, (ccord_rect){cx - 0.6f, cy + 3.0f, 1.2f, 3.0f}, c);            // stem
  ccord_draw_rect(r, (ccord_rect){cx - 3.0f, cy + 6.0f, 6.0f, 1.2f}, c);            // base
}

void ui_vm_draw_bar(ccord_ui *ui, ccord_renderer *r, ccord_rect box)
{
  ui->vm.bar_rect = box;
  uis_surface(ui, r, box, COL(bg_input));
  // Both buttons are as wide as the longer label, so "Discard" is never cut short.
  float by = box.y + (box.h - UIS_H_BTN_SM) * 0.5f;
  float bw = uis_label_width(ui, r, "Discard");
  ccord_rect cancel = {box.x + UIS_SP_2, by, bw, UIS_H_BTN_SM};
  ccord_rect send = {box.x + box.w - UIS_SP_2 - bw, by, bw, UIS_H_BTN_SM};
  ui->vm.cancel_rect = cancel;
  ui->vm.send_rect = send;
  bool rec = ui->vm.recording;
  uis_button(ui, r, cancel, rec ? "Cancel" : "Discard", UIS_BTN_SECONDARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, cancel)}, "vmcancel", 0);
  uis_button(ui, r, send, "Send", UIS_BTN_PRIMARY, UIS_BTN_S_SM,
             (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, send)}, "vmsend", 0);

  char label[64];
  float x = cancel.x + cancel.w + UIS_SP_3;
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_header));
  if (rec) {
    ccord_draw_circle(r, x + 4.0f, box.y + box.h * 0.5f, 4.0f, COL(danger));
    x += 14.0f;
    char t[16];
    fmt_mmss(ui->vm.frames * 1000u / UI_VM_RATE, t, sizeof t);
    snprintf(label, sizeof label, "%s", t);
  } else {
    char t[16];
    fmt_mmss(ui->vm.clip_ms, t, sizeof t);
    if (box.w < 420.0f) snprintf(label, sizeof label, "Voice (%s)", t);
    else snprintf(label, sizeof label, "Voice message ready (%s)", t);
  }
  ccord_vec2 lw = ccord_text_measure(r, label, strlen(label), &ts, 0.0f);
  ccord_text_draw(r, label, strlen(label), x, box.y + (box.h - lw.y) * 0.5f, &ts, 0.0f);

  // Bars: live peaks, newest on the right while recording; the stored waveform, stretched, once reviewing.
  float x0 = x + lw.x + UIS_SP_3;
  float x1 = send.x - UIS_SP_3;
  size_t nb = x1 > x0 ? (size_t)((x1 - x0) / 3.0f) : 0u;
  uint8_t wv[UI_VM_BARS];
  size_t nw = rec ? 0u : ui_vm_waveform_bars(ui->vm.wave, wv, sizeof wv);
  for (size_t i = 0; i < nb; i++) {
    uint8_t v = 0;
    if (rec) {
      if (i + ui->vm.npeaks >= nb) {
        size_t k = i + ui->vm.npeaks - nb;
        if (k < ui->vm.npeaks) v = ui->vm.peaks[k];
      }
    } else if (nw > 0) {
      v = wv[i * nw / nb];
    }
    float h = 2.0f + (float)v / 255.0f * (box.h - 16.0f);
    ccord_draw_rrect(r, (ccord_rect){x0 + (float)i * 3.0f, by + UIS_H_BTN_SM * 0.5f - h * 0.5f, 2.0f, h}, 1.0f,
                     rec ? COL(text_muted) : COL(brand));
  }
}
