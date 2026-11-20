// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Unified overlay & transient surface dispatch (retires ad-hoc click hijacks out of expr_picker.c).
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"
#include "ui_voice_surfaces.h"

bool ui_overlays_handle_surface_click(ccord_ui *ui, float mx, float my)
{
  if (!ui) return false;
  // Voice ring banner, DM call bar, header call button, and DAVE/RTC badge.
  if (ui_call_click(ui, mx, my)) return true;
  if (ui_dave_click(ui, mx, my)) return true;
  return false;
}
