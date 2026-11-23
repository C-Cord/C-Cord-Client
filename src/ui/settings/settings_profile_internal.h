// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "settings_profile.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/net/profile_edit.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/text_edit.h>

#include "ui_internal.h"
#include "ui_style.h"

#define COL(name) (ui->theme->c.name)

enum { F_NAME = 0, F_PRONOUNS, F_BIO, F_HEX, F_TEXT_COUNT };
enum { F_THEME_PRI = 0, F_THEME_ACC, F_THEME_HEX_COUNT };
enum { IMG_KEEP = 0, IMG_REMOVE, IMG_NEW };
enum { IMG_AVATAR = 0, IMG_BANNER };

typedef enum prof_picker_kind : uint8_t {
  PROF_PICK_NONE = 0,
  PROF_PICK_DECO = 1,
  PROF_PICK_EFFECT = 2,
  PROF_PICK_NAMEPLATE = 3,
  PROF_PICK_CLAN = 4,
  PROF_PICK_CROP = 5,
  PROF_PICK_COLOR = 6,
  PROF_PICK_FRAME = 7,
} prof_picker_kind;

enum { COLOR_TARGET_THEME_PRI = 0, COLOR_TARGET_THEME_ACC = 1, COLOR_TARGET_ACCENT = 2 };

typedef struct prof_snap {
  char name[CCORD_PROFILE_DISPLAY_NAME_MAX * 4u + 8u];
  char pronouns[CCORD_PROFILE_PRONOUNS_MAX * 4u + 1u];
  char bio[CCORD_PROFILE_BIO_MAX * 4u + 1u];
  char avatar_url[256];
  char banner_url[256];
  int32_t accent;                  // -1 none
  bool has_theme;
  uint32_t theme_primary;
  uint32_t theme_accent;
  ccord_id deco_sku_id;
  char deco_asset[128];            // asset hash or empty
  char deco_url[256];              // real Discord CDN URL
  char deco_name[96];
  char effect_id[64];
  char effect_name[96];
  char effect_thumb[256];
  bool has_effect_cfg;
  ccord_profile_effect_config effect_cfg;
  ccord_id frame_sku_id;
  char frame_name[96];
  char frame_thumb[256];
  bool has_frame_cfg;
  ccord_profile_frame_config frame_cfg;
  ccord_id nameplate_sku_id;
  char nameplate_asset[128];
  char nameplate_palette[24];
  char nameplate_name[96];
  ccord_id clan_guild_id;
  char clan_tag[16];
  char clan_badge[64];
} prof_snap;

typedef struct prof_img_draft {
  int mode;
  uint8_t *orig_data;              // original chosen file bytes (kept for non-destructive re-cropping)
  size_t orig_len;
  uint8_t *data;                   // bytes that will actually be uploaded (cropped PNG or original GIF)
  size_t len;
  bool is_gif;
  unsigned seq;
  char key[48];
  bool uploaded;
  float zoom;                      // 1.0 .. 3.0 real crop zoom
  float pan_x;                     // -1.0 .. 1.0 real crop pan
  float pan_y;                     // -1.0 .. 1.0 real crop pan
} prof_img_draft;

#define PROF_PICKER_HITS_MAX 128u

typedef struct prof_picker_hit {
  // 1=close, 2=apply, 3=clear, 4=search/hex, 5=cat, 6=item, 7=motion,
  // 8=zoom_minus, 9=zoom_plus, 10=refresh/retry, 11=open_shop,
  // 12=pan_left, 13=pan_right, 14=pan_up, 15=pan_down, 16=crop_reset,
  // 17=color_sv, 18=color_hue, 19=color_suggest
  int kind;
  int arg;
  ccord_rect rect;
} prof_picker_hit;

typedef struct prof_picker_state {
  prof_picker_kind kind;
  int crop_slot;                   // IMG_AVATAR or IMG_BANNER (also color_target for PROF_PICK_COLOR)
  bool search_inited;
  ccord_text_edit search;          // search filter (or hex input for PROF_PICK_COLOR)
  bool search_focused;
  int filter_idx;                  // 0 = All
  bool reduced_motion;
  float scroll;
  float max_scroll;
  // Staged selection inside open picker before clicking Apply
  ccord_id staged_sku;
  char staged_id[128];             // deco asset hash, effect snowflake id, or nameplate asset path
  char staged_url[256];            // real CDN URL (deco URL or effect thumbnail)
  char staged_palette[24];
  char staged_name[96];
  char staged_tag[16];
  char staged_badge[64];
  bool staged_has_effect;
  ccord_profile_effect_config staged_effect;
  bool staged_has_frame;
  ccord_profile_frame_config staged_frame;
  float staged_zoom;
  float staged_pan_x;
  float staged_pan_y;
  // HSV Color Picker state (PROF_PICK_COLOR)
  float color_h;                   // 0..360
  float color_s;                   // 0..1
  float color_v;                   // 0..1
  float color_tex_h;               // last hue baked into "prof-cp-sv" (-1 = none)
  int color_drag;                  // 0 none, 1 SV square, 2 Hue bar
  ccord_rect color_sv_rc;
  ccord_rect color_hue_rc;
  // Hit rects from last modal draw
  ccord_rect box;
  ccord_rect list_rect;
  prof_picker_hit hits[PROF_PICKER_HITS_MAX];
  size_t nhits;
} prof_picker_state;

typedef struct profile_state {
  const ccord_ui *owner;
  ccord_text_edit ed[F_TEXT_COUNT];
  ccord_text_edit theme_ed[F_THEME_HEX_COUNT]; // custom hex inputs for Nitro Primary & Accent theme colors
  int theme_hex_focus;             // -1 none, F_THEME_PRI (0), or F_THEME_ACC (1)
  ccord_fetch_state last_col_state;
  int subtab;                      // 0 = Main Profile, 1 = Per-Server Profiles
  ccord_id selected_guild;
  bool guild_dropdown_open;
  ccord_id last_fetched_guild;     // last guild passed to ccord_session_request_profile_ex
  bool self_profile_requested;     // global self profile requested on open
  bool collectibles_requested;     // collectibles inventory requested on open
  prof_snap base;                  // saved profile (global or selected guild)
  int32_t accent;                  // draft solid accent, -1 none
  bool hex_bad;
  bool draft_has_theme;
  uint32_t draft_theme_primary;
  uint32_t draft_theme_accent;
  ccord_id draft_deco_sku;
  char draft_deco_asset[128];
  char draft_deco_url[256];
  char draft_deco_name[96];
  char draft_effect_id[64];
  char draft_effect_name[96];
  char draft_effect_thumb[256];
  bool draft_has_effect_cfg;
  ccord_profile_effect_config draft_effect_cfg;
  ccord_id draft_frame_sku;
  char draft_frame_name[96];
  char draft_frame_thumb[256];
  bool draft_has_frame_cfg;
  ccord_profile_frame_config draft_frame_cfg;
  ccord_id draft_np_sku;
  char draft_np_asset[128];
  char draft_np_palette[24];
  char draft_np_name[96];
  ccord_id draft_clan_guild;
  char draft_clan_tag[16];
  char draft_clan_badge[64];
  prof_img_draft img[2];
  bool pending;
  uint32_t sent;
  uint64_t saved_until_ms;
  char err[CCORD_PROFILE_FIELD_COUNT][200];
  char general[200];
  float scroll, content_h, view_h;
  ui_profile_item items[UI_PROFILE_ITEMS_MAX];
  size_t nitems;
  int awaiting;                    // image slot waiting for picked/dropped file, -1 none
  bool click_pending;
  float click_x, click_y;
  bool cb_done;
  ccord_profile_edit_result cb_res;
  ccord_rect bar, view;
  ccord_rect err_rect[CCORD_PROFILE_FIELD_COUNT];
  bool bar_visible;
  char pv_name[130], pv_pronouns[170], pv_bio[800];
  int pv_accent;
  prof_picker_state picker;
} profile_state;

// Cross-TU helpers between settings_profile.c and settings_profile_pickers.c
const ccord_user *sp_self_user(const ccord_ui *ui);
ccord_color sp_rgb(uint32_t v);
void sp_sync_color_drafts(void);
bool sp_ensure_img(ccord_ui *ui, ccord_renderer *r, prof_img_draft *d);
bool sp_apply_img_crop(ccord_ui *ui, prof_img_draft *d, int slot, float zoom, float pan_x, float pan_y);
const ccord_owned_collectible *sp_find_owned_deco(const ccord_ui *ui, ccord_id sku, const char *asset_or_url);
const ccord_owned_collectible *sp_find_owned_effect(const ccord_ui *ui, const char *effect_id_str);
const ccord_owned_collectible *sp_find_owned_frame(const ccord_ui *ui, ccord_id sku);
const ccord_owned_collectible *sp_find_owned_nameplate(const ccord_ui *ui, ccord_id sku, const char *asset);
void sp_draw_avatar_deco_cdn(ccord_ui *ui, ccord_renderer *r, const char *deco_url,
                             float cx, float cy, float avatar_d, bool animate);
void sp_draw_profile_effect_real(ccord_ui *ui, ccord_renderer *r, ccord_id uid,
                                 const ccord_profile_effect_config *cfg, const char *fallback_url,
                                 ccord_rect card, float radius, bool animate);
void sp_draw_nameplate_strip(ccord_ui *ui, ccord_renderer *r, ccord_rect row, float radius,
                             const char *asset, const char *palette,
                             const ccord_user *u, const char *display_name, bool animate);
void sp_picker_open(ccord_ui *ui, profile_state *s, prof_picker_kind kind, int crop_slot);
void sp_picker_close(ccord_ui *ui, profile_state *s);
void sp_picker_free(profile_state *s);
void sp_picker_draw(ccord_ui *ui, ccord_renderer *r, profile_state *s);
bool sp_picker_click(ccord_ui *ui, profile_state *s, float x, float y);
bool sp_picker_mouse_move(ccord_ui *ui, profile_state *s, float x, float y);
void sp_picker_mouse_up(ccord_ui *ui, profile_state *s);
bool sp_picker_wheel(ccord_ui *ui, profile_state *s, float wheel);
bool sp_picker_key(ccord_ui *ui, profile_state *s, const ccord_input_event *ev);
