// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
typedef enum ccord_key : uint8_t {
  CCORD_KEY_UNKNOWN, CCORD_KEY_RETURN, CCORD_KEY_ESCAPE, CCORD_KEY_BACKSPACE, CCORD_KEY_DELETE, CCORD_KEY_TAB,
  CCORD_KEY_LEFT, CCORD_KEY_RIGHT, CCORD_KEY_UP, CCORD_KEY_DOWN, CCORD_KEY_HOME, CCORD_KEY_END,
  CCORD_KEY_PAGEUP, CCORD_KEY_PAGEDOWN, CCORD_KEY_A, CCORD_KEY_C, CCORD_KEY_V, CCORD_KEY_X,
  CCORD_KEY_Z, CCORD_KEY_Y, CCORD_KEY_B, CCORD_KEY_I, CCORD_KEY_U, CCORD_KEY_S, CCORD_KEY_E,
  CCORD_KEY_F, CCORD_KEY_K, CCORD_KEY_P, CCORD_KEY_G, CCORD_KEY_M, CCORD_KEY_D, CCORD_KEY_T, CCORD_KEY_L,
  CCORD_KEY_SLASH, CCORD_KEY_Q, CCORD_KEY_BACK
} ccord_key;
enum { CCORD_MOD_SHIFT = 1, CCORD_MOD_CTRL = 2, CCORD_MOD_ALT = 4, CCORD_MOD_SUPER = 8 };
typedef enum ccord_input_type : uint8_t {
  CCORD_IN_NONE, CCORD_IN_QUIT, CCORD_IN_RESIZE, CCORD_IN_FOCUS, CCORD_IN_MOUSE_MOVE, CCORD_IN_MOUSE_DOWN,
  CCORD_IN_MOUSE_UP, CCORD_IN_MOUSE_LEAVE, CCORD_IN_WHEEL, CCORD_IN_KEY_DOWN, CCORD_IN_TEXT,
  // Values 11..16 are reserved for platform.h CCORD_EV_DROP_FILE / CCORD_EV_IME_PREEDIT / CCORD_EV_HOTKEY /
  // CCORD_EV_DROP_BEGIN / CCORD_EV_DROP_POSITION / CCORD_EV_DROP_COMPLETE.
  CCORD_IN_TOUCH_DOWN = 32,
  CCORD_IN_TOUCH_MOVE = 33,
  CCORD_IN_TOUCH_UP = 34,
  CCORD_IN_TOUCH_CANCEL = 35,
  CCORD_IN_APP_BACKGROUND = 36,
  CCORD_IN_APP_FOREGROUND = 37,
  CCORD_IN_APP_LOW_MEMORY = 38,
  CCORD_IN_RENDER_RESET = 39,
  CCORD_IN_SAFE_AREA = 40,
  CCORD_IN_KEYBOARD_INSET = 41,
} ccord_input_type;
typedef struct ccord_input_event {
  ccord_input_type type;
  uint8_t button;      // 1 left, 2 middle, 3 right, 4 back (X1), 5 forward (X2)
  uint8_t clicks;      // 1 single, 2 double
  uint8_t mods;        // CCORD_MOD_* bit set
  ccord_key key;
  float x, y;          // MOUSE_*/TOUCH_*: logical px. RESIZE: x=width y=height (logical px). WHEEL: x,y = pointer pos
  float wheel;         // WHEEL: positive = scroll content up (towards older messages), in "notches" (1.0 = one notch)
                       // KEYBOARD_INSET: logical px height of on-screen soft keyboard (0 when hidden)
  bool focused;        // FOCUS
  char text[32];       // TEXT: UTF-8, NUL-terminated, valid. IME_PREEDIT: composition string (empty = clear).
                       // HOTKEY: shortcut id. DROP_FILE: 31-byte path preview.
  int32_t ime_start;   // IME_PREEDIT: cursor offset within text, in codepoints (0 when unset)
  int32_t ime_length;  // IME_PREEDIT: selection length within text, in codepoints (0 when none)
  int64_t finger_id;   // TOUCH_*: stable touch pointer id
  uint64_t timestamp_ns; // TOUCH_*: monotonic event timestamp in nanoseconds (0 = unset, falls back to UI clock)
  float inset_top, inset_right, inset_bottom, inset_left;   // SAFE_AREA: logical px insets
} ccord_input_event;
typedef enum ccord_cursor_kind : uint8_t {
  CCORD_CURSOR_DEFAULT,
  CCORD_CURSOR_POINTER,
  CCORD_CURSOR_TEXT,
  CCORD_CURSOR_NS_RESIZE,
  CCORD_CURSOR_EW_RESIZE,
  CCORD_CURSOR_NWSE_RESIZE,
  CCORD_CURSOR_NESW_RESIZE,
  CCORD_CURSOR_MOVE,
  CCORD_CURSOR_NOT_ALLOWED,
  CCORD_CURSOR_COUNT
} ccord_cursor_kind;
