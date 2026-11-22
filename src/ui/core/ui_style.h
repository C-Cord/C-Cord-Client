// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Shared UI style tokens and the draw helpers built on them. Private to the ui module (include after the usual
// ui_internal.h). Design
// other surfaces move over one file group at a time (work packages in that document).
//
// Rules the helpers keep, so a surface that uses them cannot drift:
//  - every size that has a theme metric is read from the theme (uis_tokens_of), never typed again;
//  - every other size comes from the spacing / height scale below;
//  - hover, pressed, disabled and focus look the same everywhere and ease over UI_FADE_MS (instant under reduced
//    motion, because they go through ui_fade / ui_enter in motion.c);
//  - every helper writes one record to a small audit table (uis_audit_get) so a headless slice can prove a surface drew
//    through the shared path and with token values.
#include <stdbool.h>
#include <stdint.h>

#include <ccord/render/render.h>
#include <ccord/ui/text_edit.h>
#include <ccord/ui/ui.h>

// ---- spacing scale (logical px). Layout offsets, paddings and gaps are one of these. ----
#define UIS_SP_1 4.0f    // icon-to-label gap, chip padding
#define UIS_SP_2 8.0f    // list-row padding, popover padding, gap between buttons (= theme channel_item_margin_x)
#define UIS_SP_3 12.0f   // input padding, tooltip padding-x, section gap
#define UIS_SP_4 16.0f   // card padding, panel gutter (= theme message_pad_x / composer_margin_x)
#define UIS_SP_5 20.0f   // modal padding
#define UIS_SP_6 24.0f   // page padding
#define UIS_SP_8 32.0f   // window margin around a modal

// ---- control heights (logical px). A row, button or input in a layout is one of these tall. ----
#define UIS_H_BTN_SM 28.0f
#define UIS_H_BTN 32.0f      // = theme channel_item_height = context_menu_item_height
#define UIS_H_BTN_LG 38.0f
#define UIS_H_INPUT 36.0f
#define UIS_TOGGLE_W 44.0f
#define UIS_TOGGLE_H 24.0f
#define UIS_ICON_BTN 32.0f   // square icon-only button (header, user panel, voice bar use this hit size)
#define UIS_ICON_GLYPH 18.0f // glyph size inside an icon button
#define UIS_H_CHIP 20.0f     // role chip / tag: caption text in a pill (radius = half the height)
#define UIS_SWATCH_D 24.0f   // colour swatch, a circle this wide and tall
#define UIS_RADIO_D 20.0f    // radio indicator, a circle this wide and tall
#define UIS_SLIDER_H 8.0f    // slider track (its thumb is a 12 px circle centred on the track)
#define UIS_SCROLL_ARROW_H 20.0f // scrollbar arrow (16 px wide): a uis_icon_button, so its height is a token here

// ---- modal widths ----
#define UIS_MODAL_W_SM 440.0f   // prompts and confirmations
#define UIS_MODAL_W_MD 520.0f   // forms
#define UIS_MODAL_W_LG 600.0f   // profile and pickers
#define UIS_MODAL_Y_BIAS 0.42f  // the box sits a little above the centre: y = (win_h - h) * bias

// ---- motion that belongs to the style layer (the durations UI_FADE_MS / UI_POPUP_MS / UI_MODAL_MS stay in ui_internal.h) ----
#define UIS_MS_TIP 90u
#define UIS_SLIDE_POPOVER 6.0f
#define UIS_SLIDE_MODAL 8.0f
#define UIS_DISABLED_BG 0.45f   // alpha on a disabled control's fill
#define UIS_DISABLED_FG 0.55f   // alpha on a disabled control's label / glyph
#define UIS_PRESSED_DARKEN 0.18f

// Everything the helpers read from the theme, resolved once per call site. Colours stay in ui->theme->c.
typedef struct uis_tokens {
  float r_control;     // buttons, inputs, menu rows, tooltips, chips        (theme channel_item_radius, 4)
  float r_menu;        // context menu panel                                  (theme context_menu_radius, 4)
  float r_surface;     // popovers, modals, cards, composer                   (theme composer_radius, 8)
  float fs_title;      // modal / page titles                                 (theme font_size_large, 20)
  float fs_body;       // message text, input text                            (theme font_size_base, 16)
  float fs_control;    // button labels, row labels, tooltips                 (theme font_size_small, 14)
  float fs_caption;    // timestamps, hints, badges                           (font_size_small - 2, 12)
  float fs_overline;   // uppercase section headings                          (font_size_small - 3, 11)
  float menu_row_h;    //                                                     (theme context_menu_item_height, 32)
  float menu_pad;      //                                                     (theme context_menu_pad, 6)
  float menu_min_w;    //                                                     (theme context_menu_min_width, 188)
  float list_row_h;    //                                                     (theme channel_item_height, 32)
} uis_tokens;
uis_tokens uis_tokens_of(const ccord_ui *ui);

// Interaction state a caller passes in. `hover` must already be false when something sits on top of the control.
typedef struct uis_state {
  bool hover, pressed, focused, disabled, selected;
} uis_state;

// ---- shadows, borders, frames ----
typedef enum uis_elevation : uint8_t {
  UIS_ELEV_POPOVER,   // menus, popovers, tooltips: 3 layers, 2 px steps, 2 px down
  UIS_ELEV_MODAL,     // dialogs: 3 layers, 3 px steps, 3 px down
} uis_elevation;
void uis_shadow(ccord_renderer *r, ccord_rect box, float radius, uis_elevation e);
// shadow + 1 px border_subtle ring + `fill`; returns nothing. The ring is outset (box itself is the content rect).
void uis_frame(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float radius, ccord_color fill, uis_elevation e);
// Popover / menu / picker panel: bg_floating, surface radius, popover elevation.
void uis_popover_frame(ccord_ui *ui, ccord_renderer *r, ccord_rect box);
// Full-window scrim (theme overlay) + modal panel (bg_secondary, surface radius, modal elevation). Call inside the
// caller's ui_popup_begin(... UI_MODAL_MS, UIS_SLIDE_MODAL) so the scrim fades with the dialog.
void uis_modal_frame(ccord_ui *ui, ccord_renderer *r, ccord_rect box);
// A modal box of the wanted size, clamped to the window with UIS_SP_8 margins and placed at UIS_MODAL_Y_BIAS.
ccord_rect uis_modal_box(const ccord_ui *ui, float w, float h);
// Draws the modal title at the modal padding; returns the y where the body starts.
float uis_modal_title(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const char *title);
// Footer button slot: index 0 is the rightmost. Buttons are UIS_H_BTN tall, UIS_SP_2 apart, UIS_SP_5 from the edges.
ccord_rect uis_modal_footer_button(ccord_rect box, int index_from_right, float w);
// A 1 px horizontal rule in `divider`.
void uis_divider(ccord_ui *ui, ccord_renderer *r, float x, float y, float w);
// A 1 px vertical rule in `divider`, `h` tall.
void uis_vdivider(ccord_ui *ui, ccord_renderer *r, float x, float y, float h);

// ---- button ----
typedef enum uis_btn_kind : uint8_t {
  UIS_BTN_PRIMARY,     // brand -> brand_hover, text_on_brand
  UIS_BTN_SECONDARY,   // bg_tertiary -> bg_item_hover, text_header
  UIS_BTN_DANGER,      // danger -> danger lightened, text_on_brand
  UIS_BTN_SUCCESS,     // status_online -> lightened, text_on_brand ("Add Friend", "Accept")
  UIS_BTN_LINK,        // no fill -> bg_item_hover, text_link
  UIS_BTN_DANGER_SOFT, // bg_tertiary at rest -> danger tint on hover (eased), text_header -> text_on_brand ("Remove")
} uis_btn_kind;
typedef enum uis_btn_size : uint8_t { UIS_BTN_S_SM, UIS_BTN_S_MD, UIS_BTN_S_LG } uis_btn_size;
// Sub-states share the (ns, id) of the hover fade; these top bits keep their fades apart from it and from each other.
// Tests read a sub-state with uis_anim_value(ns, id ^ salt).
#define UIS_SALT_PRESS (1ull << 60)
#define UIS_SALT_FOCUS (2ull << 60)
#define UIS_SALT_SELECT (3ull << 60)
float uis_button_height(uis_btn_size size);
// Draws the button inside `rc` (the caller's hit rect). (ns, id) key the hover, press and focus-ring fades (each eases
// over UI_FADE_MS; instant under reduced motion).
void uis_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_btn_kind kind,
                uis_btn_size size, uis_state st, const char *ns, uint64_t id);
// Square icon-only button: hover / selected fill at control radius; the caller draws the glyph at the returned colour.
ccord_color uis_icon_button(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, uis_state st, const char *ns, uint64_t id);

// ---- menu row ----
// One row of a context menu or a select list: brand fill on hover (danger fill for a destructive row), label,
// optional submenu chevron or check dot. `rc` is the row (menu_row_h tall).
void uis_menu_row(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_state st, bool has_sub,
                  bool checked, bool danger, const char *ns, uint64_t id);
// A list row (channel / member / pin / search result): bg_item_hover on hover, bg_item_selected when selected.
void uis_list_row_fill(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, uis_state st, const char *ns, uint64_t id);

// ---- tooltip ----
typedef enum uis_tip_side : uint8_t { UIS_TIP_BELOW, UIS_TIP_ABOVE, UIS_TIP_RIGHT } uis_tip_side;
// Multi-element tooltip container: places a tooltip sized to `content_w` x `content_h` plus standard UIS_SP_3 x UIS_SP_2
// padding, opens the UIS_MS_TIP popup fade, draws the popover frame and records one UIS_K_TOOLTIP audit entry.
// Returns the drawn outer box; caller draws content inside (box.x + UIS_SP_3, box.y + UIS_SP_2) and calls uis_tooltip_end.
ccord_rect uis_tooltip_begin(ccord_ui *ui, ccord_renderer *r, ccord_rect anchor, float content_w, float content_h,
                             uis_tip_side side, uint64_t id);
void uis_tooltip_end(ccord_renderer *r);
// Fades in over UIS_MS_TIP (instant under reduced motion), UIS_SP_3 x UIS_SP_2 padding, 6 px from the anchor, kept
// inside the window with an 8 px margin. Call only while the anchor is hovered; do not wrap it in another popup fade.
// Returns the drawn rect.
ccord_rect uis_tooltip(ccord_ui *ui, ccord_renderer *r, ccord_rect anchor, const char *text, uis_tip_side side,
                       uint64_t id);

// ---- text input ----
// One-line field: bg_input at control radius, UIS_SP_3 padding, body font, placeholder in text_placeholder, selection and
// caret, brand focus ring that eases in over UI_FADE_MS, disabled dims. `masked` draws bullets. Sets ui->text_area when
// focused so the IME window follows. Returns the text origin x.
float uis_input(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const ccord_text_edit *ed, const char *placeholder,
                uis_state st, bool masked, const char *ns, uint64_t id);
// The same field with its text inset `pad_l` from the left edge and `pad_r` from the right (a search field with a leading
// glyph or a trailing clear button). uis_input is this with UIS_SP_3 on both sides.
float uis_input_padded(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float pad_l, float pad_r,
                       const ccord_text_edit *ed, const char *placeholder, uis_state st, bool masked, const char *ns,
                       uint64_t id);

// ---- toggle ----
// The shared switch (ui_draw_switch) forced to UIS_TOGGLE_W x UIS_TOGGLE_H, vertically centred in `row` and flush right.
ccord_rect uis_toggle_rect(ccord_rect row);
void uis_toggle(ccord_ui *ui, ccord_renderer *r, ccord_rect row, bool on, uis_state st, const char *ns, uint64_t id);

// ---- chip ----
// A role pill or tag: bold caption text (fs_caption) on a UIS_H_CHIP tall pill, vertically centred in `rc`, with the
// pill's radius at half its height. Off: bg_tertiary and text_header. `st.selected`: brand and text_on_brand, eased.
void uis_chip(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_state st, const char *ns,
              uint64_t id);

// ---- select ----
// The closed dropdown field: bg_input at control radius, `value` in body text, a down chevron on the right. Hover eases
// to bg_item_hover; `st.focused` or `st.selected` (open) draws the brand ring. Caller sizes `box` (UIS_H_INPUT normally).
void uis_select(ccord_ui *ui, ccord_renderer *r, ccord_rect box, const char *value, uis_state st, const char *ns,
                uint64_t id);

// ---- swatch ----
// A colour swatch: a circle of `colour` inside a 2 px ring (border_subtle; brand when `st.selected`; hover lightens the
// ring). `rc` should be UIS_SWATCH_D square.
void uis_swatch(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, ccord_color colour, uis_state st, const char *ns,
                uint64_t id);

// ---- radio ----
// A labelled radio row: a UIS_RADIO_D circle at UIS_SP_2 from the row's left (a 2 px ring; a brand dot eases in when
// `on`), `label` in control text after it, and the row hover fill. `row` is the caller's row, normally UIS_H_BTN tall.
void uis_radio(ccord_ui *ui, ccord_renderer *r, ccord_rect row, const char *label, bool on, uis_state st, const char *ns,
               uint64_t id);

// ---- label width ----
// Width a button or tab needs for `label`: the bold control label (fs_control) plus UIS_SP_3 on each side.
float uis_label_width(ccord_ui *ui, ccord_renderer *r, const char *label);

// ---- tab ----
// One tab of a segmented row (picker GIFs / Stickers / Emoji / Sounds): the caller sets `rc`, normally UIS_H_BTN_SM tall
// and uis_label_width wide. The selected tab is brand with text_on_brand; a hovered tab eases to bg_item_hover, and
// selecting or deselecting a tab eases its fill and text colour (UI_FADE_MS; the font weight switches at once).
void uis_tab(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, const char *label, uis_state st, const char *ns, uint64_t id);

// ---- surface ----
// A flat bg-tinted strip on the surface radius with no shadow or ring (the voice-message recorder bar sits inside the
// composer). `box` is the whole strip; `fill` is the caller's colour.
void uis_surface(ccord_ui *ui, ccord_renderer *r, ccord_rect box, ccord_color fill);

// ---- slider (a track with a thumb) ----
// Fully rounded track `track` (UIS_SLIDER_H tall) filled in brand up to `fraction` (0..1), thumb circle at the fill end.
// Disabled dims the fill and the thumb. The caller owns the drag or click hit rect.
// The fill eases to a new `fraction` over 150 ms (instant under reduced motion); the track position is its key.
void uis_slider(ccord_ui *ui, ccord_renderer *r, ccord_rect track, float fraction, uis_state st);
// The fill position uis_slider last drew `track` with, 0..1; -1 before the first draw. Read-only, for tests.
float uis_slider_shown(const ccord_ui *ui, ccord_rect track);
// The value the fade keyed by (ns, id) was last drawn with, 0..1; -1 when no fade with that key has been drawn.
// Read-only, for tests: a transition is checked through this, not by copying the easing math.
float uis_anim_value(const ccord_ui *ui, const char *ns, uint64_t id);

// ---- card (a grouped block inside a page) ----
// bg_secondary at the surface radius. Rows, toggles and buttons are drawn on top of it.
void uis_card(ccord_ui *ui, ccord_renderer *r, ccord_rect box);

// ---- scrollbar (a fixed vertical track with a thumb) ----
// The track `track` is a bg_tertiary pill as wide as it is; the thumb `thumb` (text_muted) sits on it. The caller owns
// the click rects and computes the thumb. Audited as UIS_K_SCROLLBAR.
void uis_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect track, ccord_rect thumb);
// An up or down arrow for a scrollbar, `rc` UIS_SCROLL_ARROW_H tall: a uis_icon_button with a chevron in its glyph colour.
// `id` keys the hover fade; use one id per arrow.
void uis_scroll_arrow(ccord_ui *ui, ccord_renderer *r, ccord_rect rc, bool up, uis_state st, const char *ns, uint64_t id);

// ---- plate (a small non-interactive rounded tag) ----
// A hint badge, key cap or status tag: a filled rounded rect at the caller's radius (a theme radius, normally
// uis_tokens_of().r_control). Draws no text and has no hover. Audited as UIS_K_PLATE so a slice sees its radius.
void uis_plate(ccord_ui *ui, ccord_renderer *r, ccord_rect box, float radius, ccord_color fill);

// ---- audit (for the ui-style verify slice) ----
typedef enum uis_kind : uint8_t {
  UIS_K_BUTTON, UIS_K_ICON_BUTTON, UIS_K_MENU_ROW, UIS_K_LIST_ROW, UIS_K_POPOVER, UIS_K_MODAL, UIS_K_TOOLTIP,
  UIS_K_INPUT, UIS_K_TOGGLE, UIS_K_TAB, UIS_K_SURFACE, UIS_K_CHIP, UIS_K_SELECT, UIS_K_SWATCH, UIS_K_RADIO,
  UIS_K_VDIVIDER, UIS_K_SLIDER, UIS_K_CARD, UIS_K_SCROLLBAR, UIS_K_PLATE, UIS_K_COUNT
} uis_kind;
typedef struct uis_audit_entry {
  uint32_t count;        // draws of this kind since the last reset (keeps rising past UIS_AUDIT_MAX)
  ccord_rect rect;       // last draw (its height is rect.h)
  float radius;          // corner radius used
  float font_px;         // label / text size used (0 when none)
  ccord_color fill;      // last fill colour
} uis_audit_entry;
// One draw in order. A divider draws no radius or text, so both are 0 for it.
#define UIS_AUDIT_MAX 256
typedef struct uis_audit_record {
  uis_kind kind;
  ccord_rect rect;
  float radius;
  float font_px;
  ccord_color fill;
} uis_audit_record;
typedef struct uis_audit {
  uis_audit_entry kind[UIS_K_COUNT];   // the last draw of each kind, and its count
  uint32_t total;                       // every draw since the reset, past UIS_AUDIT_MAX too
  uint32_t kept;                        // rec[0 .. kept) hold the first draws in order: min(total, UIS_AUDIT_MAX)
  uis_audit_record rec[UIS_AUDIT_MAX];
} uis_audit;
const uis_audit *uis_audit_get(void);
void uis_audit_reset(void);
const char *uis_kind_name(uis_kind k);
