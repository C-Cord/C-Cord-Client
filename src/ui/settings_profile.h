// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Settings > Profile: edit the own Discord profile inside C-Cord (display name, pronouns, About Me, avatar, banner,
// accent colour) with a live preview card. Password, email, username and two-factor changes are NOT edited here: the
// page only links to discord.com for them (no input exists for those).
//
// The page keeps drafts next to the saved profile (read from the store). A field that differs from the saved profile
// is "changed"; the unsaved-changes bar offers Reset and Save Changes. Save sends only the changed fields through
// ccord_session_edit_profile, shows a pending state, then a brief success state; a per-field error from Discord is
// drawn under its field and the drafts stay in place. The code is in settings_profile.c; settings.c owns the page
// enum, the sidebar entry and the dispatch.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/render/render.h>
#include <ccord/ui/ui.h>

// The sidebar page index of "Profile" (settings.c static-asserts SP_PROFILE_EDIT equals it).
#define UI_SETTINGS_PAGE_PROFILE 17
// ui->account_focus_field values UI_PROFILE_FOCUS_BASE + field mean a text field of this page has the keyboard.
#define UI_PROFILE_FOCUS_BASE 20
#define UI_PROFILE_WEB_URL "https://discord.com/channels/@me"

typedef enum ui_profile_kind : uint8_t {
  UI_PROF_NAME,           // single-line text fields (arg = field index 0..3: name, pronouns, about me, hex)
  UI_PROF_PRONOUNS,
  UI_PROF_BIO,
  UI_PROF_ACCENT_HEX,
  UI_PROF_AVATAR_CHANGE,
  UI_PROF_AVATAR_REMOVE,
  UI_PROF_BANNER_CHANGE,
  UI_PROF_BANNER_REMOVE,
  UI_PROF_ACCENT_SWATCH,  // arg = preset index
  UI_PROF_ACCENT_DEFAULT,
  UI_PROF_SAVE,
  UI_PROF_RESET,
  UI_PROF_WEB,            // "Open discord.com"
} ui_profile_kind;

#define UI_PROFILE_ITEMS_MAX 40u

// One hit-testable control, recorded by the last draw.
typedef struct ui_profile_item {
  ui_profile_kind kind;
  int arg;
  ccord_rect rect;
  char label[40];
} ui_profile_item;

// Field bits, the same numbering as ccord_profile_field (display name 0, avatar 1, banner 2, about me 3, pronouns 4,
// accent 5).
typedef struct ui_profile_debug {
  bool live;
  uint32_t changed;                  // bit (1u << ccord_profile_field) per field that differs from the saved profile
  bool pending, saved, bar_visible;
  bool has_error[6];
  char error[6][200];                // per-field error text as drawn under the field
  ccord_rect error_rect[6];          // where each error was drawn (zero size when none)
  char general[200];                 // error shown in the bar ("" none)
  char preview_name[130];            // what the preview card last drew
  char preview_pronouns[170];
  char preview_bio[800];
  int preview_accent;                // -1 = none
  int avatar_mode, banner_mode;      // 0 keep, 1 remove, 2 new image chosen
  ccord_rect bar, view;
  float scroll, max_scroll;
  const char *text[4];               // current text of name, pronouns, about me, hex
} ui_profile_debug;

void ui_settings_profile_draw(ccord_ui *ui, ccord_renderer *r, ccord_rect content, ccord_rect panel);
bool ui_settings_profile_click(ccord_ui *ui, ccord_rect content, ccord_rect panel, float x, float y);
// Keyboard and text input for the focused field; true when consumed.
bool ui_settings_profile_key(ccord_ui *ui, const ccord_input_event *ev);
bool ui_settings_profile_wheel(ccord_ui *ui, float wheel);
// From ui_settings_scroll_tick: applies Discord's answer, ends the success state. True while it needs more frames.
bool ui_settings_profile_tick(ccord_ui *ui);
// Frees the drafts (settings closed, or the ui destroyed).
void ui_settings_profile_release(ccord_ui *ui);
// True while a field of this page wants OS text input.
bool ui_settings_profile_wants_text(const ccord_ui *ui);
// Opens Settings on the Profile page (the account card's Edit Profile row).
void ui_settings_open_profile(ccord_ui *ui);

// Picked/dropped image file for the avatar or banner that asked for one (Change was clicked): ccord_ui_attach_bytes
// forwards the platform's file-dialog and drop events here. True when consumed.
bool ui_settings_profile_take_image(ccord_ui *ui, const char *filename, const void *data, size_t size);
// Reads `path` and uses it as the new avatar (banner = false) or banner. False (with a notice) when it cannot be
// read or is not a PNG/JPEG/GIF/WebP of at most 10 MiB.
bool ui_settings_profile_set_image_file(ccord_ui *ui, bool banner, const char *path);

// Test seams.
size_t ui_settings_profile_items(const ccord_ui *ui, const ui_profile_item **out);
void ui_settings_profile_debug(const ccord_ui *ui, ui_profile_debug *out);
