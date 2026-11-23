// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Headless GIF-picker Favorites verification; see verify_gif_favorites.h.
//
// The mock transport's own media answer is a 1x1 PNG stub that the UI quietly replaces with a fallback pixel, which
// would hide a failed decode. This slice therefore wraps the mock's vtable: fetch_media is forwarded (so the request
// is the UI's real one) and poll() swaps the bytes of a fixture URL's MEDIA_LOADED event for a real tiny MP4 / GIF.
// A tile counts as passed only when its texture has exactly the dimensions of those served bytes.
#include "verify_gif_favorites.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/core/log.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/live_transport.h>
#include <ccord/store/expressions.h>

#include "ui_internal.h"

#define GF_TICK_MAX 900
#define GF_CHAN_TEXT 3121ULL   // the mock's text channel (verify_expressions.c EX_CHAN)

// 50x30 yuv420p H.264 MP4, 3 frames (ffmpeg testsrc, libx264 -crf 40).
static const uint8_t k_gf_mp4[] = {
  0x00, 0x00, 0x00, 0x20, 0x66, 0x74, 0x79, 0x70, 0x69, 0x73, 0x6f, 0x6d,
  0x00, 0x00, 0x02, 0x00, 0x69, 0x73, 0x6f, 0x6d, 0x69, 0x73, 0x6f, 0x32,
  0x61, 0x76, 0x63, 0x31, 0x6d, 0x70, 0x34, 0x31, 0x00, 0x00, 0x03, 0x4e,
  0x6d, 0x6f, 0x6f, 0x76, 0x00, 0x00, 0x00, 0x6c, 0x6d, 0x76, 0x68, 0x64,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x03, 0xe8, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x01, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x79, 0x74, 0x72, 0x61, 0x6b,
  0x00, 0x00, 0x00, 0x5c, 0x74, 0x6b, 0x68, 0x64, 0x00, 0x00, 0x00, 0x03,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
  0x00, 0x32, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24,
  0x65, 0x64, 0x74, 0x73, 0x00, 0x00, 0x00, 0x1c, 0x65, 0x6c, 0x73, 0x74,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c,
  0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0xf1,
  0x6d, 0x64, 0x69, 0x61, 0x00, 0x00, 0x00, 0x20, 0x6d, 0x64, 0x68, 0x64,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x55, 0xc4, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x2d, 0x68, 0x64, 0x6c, 0x72, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x76, 0x69, 0x64, 0x65, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x69, 0x64, 0x65,
  0x6f, 0x48, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x72, 0x00, 0x00, 0x00, 0x01,
  0x9c, 0x6d, 0x69, 0x6e, 0x66, 0x00, 0x00, 0x00, 0x14, 0x76, 0x6d, 0x68,
  0x64, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x24, 0x64, 0x69, 0x6e, 0x66, 0x00, 0x00, 0x00,
  0x1c, 0x64, 0x72, 0x65, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x0c, 0x75, 0x72, 0x6c, 0x20, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x01, 0x5c, 0x73, 0x74, 0x62, 0x6c, 0x00, 0x00, 0x00,
  0xc0, 0x73, 0x74, 0x73, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0xb0, 0x61, 0x76, 0x63, 0x31, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00,
  0x1e, 0x00, 0x48, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x14, 0x4c, 0x61, 0x76, 0x63, 0x36, 0x33, 0x2e, 0x31,
  0x2e, 0x31, 0x30, 0x32, 0x20, 0x6c, 0x69, 0x62, 0x78, 0x32, 0x36, 0x34,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x18, 0xff, 0xff, 0x00, 0x00, 0x00, 0x36, 0x61, 0x76, 0x63, 0x43, 0x01,
  0x64, 0x00, 0x0a, 0xff, 0xe1, 0x00, 0x19, 0x67, 0x64, 0x00, 0x0a, 0xac,
  0xd9, 0x44, 0x5e, 0x22, 0xb0, 0x11, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
  0x00, 0x03, 0x00, 0x14, 0x0f, 0x12, 0x25, 0x96, 0x01, 0x00, 0x06, 0x68,
  0xeb, 0xe0, 0xe4, 0xb2, 0x2c, 0xfd, 0xf8, 0xf8, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x70, 0x61, 0x73, 0x70, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x14, 0x62, 0x74, 0x72, 0x74, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x67, 0x8a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x18, 0x73, 0x74, 0x74, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
  0x14, 0x73, 0x74, 0x73, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18, 0x63, 0x74, 0x74,
  0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x73, 0x74, 0x73,
  0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x20, 0x73, 0x74, 0x73, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0xc0, 0x00, 0x00, 0x00,
  0x0e, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x14, 0x73, 0x74, 0x63,
  0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03,
  0x7e, 0x00, 0x00, 0x00, 0x61, 0x75, 0x64, 0x74, 0x61, 0x00, 0x00, 0x00,
  0x59, 0x6d, 0x65, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x21, 0x68, 0x64, 0x6c, 0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x6d, 0x64, 0x69, 0x72, 0x61, 0x70, 0x70, 0x6c, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x69, 0x6c,
  0x73, 0x74, 0x00, 0x00, 0x00, 0x24, 0xa9, 0x74, 0x6f, 0x6f, 0x00, 0x00,
  0x00, 0x1c, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x00, 0x00, 0x4c, 0x61, 0x76, 0x66, 0x36, 0x33, 0x2e, 0x31, 0x2e, 0x31,
  0x30, 0x32, 0x00, 0x00, 0x00, 0x08, 0x66, 0x72, 0x65, 0x65, 0x00, 0x00,
  0x03, 0xea, 0x6d, 0x64, 0x61, 0x74, 0x00, 0x00, 0x02, 0xae, 0x06, 0x05,
  0xff, 0xff, 0xaa, 0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7, 0x96,
  0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef, 0x78, 0x32, 0x36, 0x34, 0x20,
  0x2d, 0x20, 0x63, 0x6f, 0x72, 0x65, 0x20, 0x31, 0x36, 0x35, 0x20, 0x72,
  0x33, 0x32, 0x32, 0x32, 0x20, 0x62, 0x33, 0x35, 0x36, 0x30, 0x35, 0x61,
  0x20, 0x2d, 0x20, 0x48, 0x2e, 0x32, 0x36, 0x34, 0x2f, 0x4d, 0x50, 0x45,
  0x47, 0x2d, 0x34, 0x20, 0x41, 0x56, 0x43, 0x20, 0x63, 0x6f, 0x64, 0x65,
  0x63, 0x20, 0x2d, 0x20, 0x43, 0x6f, 0x70, 0x79, 0x6c, 0x65, 0x66, 0x74,
  0x20, 0x32, 0x30, 0x30, 0x33, 0x2d, 0x32, 0x30, 0x32, 0x35, 0x20, 0x2d,
  0x20, 0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77, 0x77, 0x77, 0x2e,
  0x76, 0x69, 0x64, 0x65, 0x6f, 0x6c, 0x61, 0x6e, 0x2e, 0x6f, 0x72, 0x67,
  0x2f, 0x78, 0x32, 0x36, 0x34, 0x2e, 0x68, 0x74, 0x6d, 0x6c, 0x20, 0x2d,
  0x20, 0x6f, 0x70, 0x74, 0x69, 0x6f, 0x6e, 0x73, 0x3a, 0x20, 0x63, 0x61,
  0x62, 0x61, 0x63, 0x3d, 0x31, 0x20, 0x72, 0x65, 0x66, 0x3d, 0x33, 0x20,
  0x64, 0x65, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x3d, 0x31, 0x3a, 0x30, 0x3a,
  0x30, 0x20, 0x61, 0x6e, 0x61, 0x6c, 0x79, 0x73, 0x65, 0x3d, 0x30, 0x78,
  0x33, 0x3a, 0x30, 0x78, 0x31, 0x31, 0x33, 0x20, 0x6d, 0x65, 0x3d, 0x68,
  0x65, 0x78, 0x20, 0x73, 0x75, 0x62, 0x6d, 0x65, 0x3d, 0x37, 0x20, 0x70,
  0x73, 0x79, 0x3d, 0x31, 0x20, 0x70, 0x73, 0x79, 0x5f, 0x72, 0x64, 0x3d,
  0x31, 0x2e, 0x30, 0x30, 0x3a, 0x30, 0x2e, 0x30, 0x30, 0x20, 0x6d, 0x69,
  0x78, 0x65, 0x64, 0x5f, 0x72, 0x65, 0x66, 0x3d, 0x31, 0x20, 0x6d, 0x65,
  0x5f, 0x72, 0x61, 0x6e, 0x67, 0x65, 0x3d, 0x31, 0x36, 0x20, 0x63, 0x68,
  0x72, 0x6f, 0x6d, 0x61, 0x5f, 0x6d, 0x65, 0x3d, 0x31, 0x20, 0x74, 0x72,
  0x65, 0x6c, 0x6c, 0x69, 0x73, 0x3d, 0x31, 0x20, 0x38, 0x78, 0x38, 0x64,
  0x63, 0x74, 0x3d, 0x31, 0x20, 0x63, 0x71, 0x6d, 0x3d, 0x30, 0x20, 0x64,
  0x65, 0x61, 0x64, 0x7a, 0x6f, 0x6e, 0x65, 0x3d, 0x32, 0x31, 0x2c, 0x31,
  0x31, 0x20, 0x66, 0x61, 0x73, 0x74, 0x5f, 0x70, 0x73, 0x6b, 0x69, 0x70,
  0x3d, 0x31, 0x20, 0x63, 0x68, 0x72, 0x6f, 0x6d, 0x61, 0x5f, 0x71, 0x70,
  0x5f, 0x6f, 0x66, 0x66, 0x73, 0x65, 0x74, 0x3d, 0x2d, 0x32, 0x20, 0x74,
  0x68, 0x72, 0x65, 0x61, 0x64, 0x73, 0x3d, 0x31, 0x20, 0x6c, 0x6f, 0x6f,
  0x6b, 0x61, 0x68, 0x65, 0x61, 0x64, 0x5f, 0x74, 0x68, 0x72, 0x65, 0x61,
  0x64, 0x73, 0x3d, 0x31, 0x20, 0x73, 0x6c, 0x69, 0x63, 0x65, 0x64, 0x5f,
  0x74, 0x68, 0x72, 0x65, 0x61, 0x64, 0x73, 0x3d, 0x30, 0x20, 0x6e, 0x72,
  0x3d, 0x30, 0x20, 0x64, 0x65, 0x63, 0x69, 0x6d, 0x61, 0x74, 0x65, 0x3d,
  0x31, 0x20, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x6c, 0x61, 0x63, 0x65, 0x64,
  0x3d, 0x30, 0x20, 0x62, 0x6c, 0x75, 0x72, 0x61, 0x79, 0x5f, 0x63, 0x6f,
  0x6d, 0x70, 0x61, 0x74, 0x3d, 0x30, 0x20, 0x63, 0x6f, 0x6e, 0x73, 0x74,
  0x72, 0x61, 0x69, 0x6e, 0x65, 0x64, 0x5f, 0x69, 0x6e, 0x74, 0x72, 0x61,
  0x3d, 0x30, 0x20, 0x62, 0x66, 0x72, 0x61, 0x6d, 0x65, 0x73, 0x3d, 0x33,
  0x20, 0x62, 0x5f, 0x70, 0x79, 0x72, 0x61, 0x6d, 0x69, 0x64, 0x3d, 0x32,
  0x20, 0x62, 0x5f, 0x61, 0x64, 0x61, 0x70, 0x74, 0x3d, 0x31, 0x20, 0x62,
  0x5f, 0x62, 0x69, 0x61, 0x73, 0x3d, 0x30, 0x20, 0x64, 0x69, 0x72, 0x65,
  0x63, 0x74, 0x3d, 0x31, 0x20, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x62,
  0x3d, 0x31, 0x20, 0x6f, 0x70, 0x65, 0x6e, 0x5f, 0x67, 0x6f, 0x70, 0x3d,
  0x30, 0x20, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x70, 0x3d, 0x32, 0x20,
  0x6b, 0x65, 0x79, 0x69, 0x6e, 0x74, 0x3d, 0x32, 0x35, 0x30, 0x20, 0x6b,
  0x65, 0x79, 0x69, 0x6e, 0x74, 0x5f, 0x6d, 0x69, 0x6e, 0x3d, 0x31, 0x30,
  0x20, 0x73, 0x63, 0x65, 0x6e, 0x65, 0x63, 0x75, 0x74, 0x3d, 0x34, 0x30,
  0x20, 0x69, 0x6e, 0x74, 0x72, 0x61, 0x5f, 0x72, 0x65, 0x66, 0x72, 0x65,
  0x73, 0x68, 0x3d, 0x30, 0x20, 0x72, 0x63, 0x5f, 0x6c, 0x6f, 0x6f, 0x6b,
  0x61, 0x68, 0x65, 0x61, 0x64, 0x3d, 0x34, 0x30, 0x20, 0x72, 0x63, 0x3d,
  0x63, 0x72, 0x66, 0x20, 0x6d, 0x62, 0x74, 0x72, 0x65, 0x65, 0x3d, 0x31,
  0x20, 0x63, 0x72, 0x66, 0x3d, 0x34, 0x30, 0x2e, 0x30, 0x20, 0x71, 0x63,
  0x6f, 0x6d, 0x70, 0x3d, 0x30, 0x2e, 0x36, 0x30, 0x20, 0x71, 0x70, 0x6d,
  0x69, 0x6e, 0x3d, 0x30, 0x20, 0x71, 0x70, 0x6d, 0x61, 0x78, 0x3d, 0x36,
  0x39, 0x20, 0x71, 0x70, 0x73, 0x74, 0x65, 0x70, 0x3d, 0x34, 0x20, 0x69,
  0x70, 0x5f, 0x72, 0x61, 0x74, 0x69, 0x6f, 0x3d, 0x31, 0x2e, 0x34, 0x30,
  0x20, 0x61, 0x71, 0x3d, 0x31, 0x3a, 0x31, 0x2e, 0x30, 0x30, 0x00, 0x80,
  0x00, 0x00, 0x01, 0x0a, 0x65, 0x88, 0x84, 0x00, 0x57, 0xd3, 0x20, 0x07,
  0x12, 0x9f, 0x9a, 0x5f, 0xff, 0x7c, 0x44, 0xb9, 0xe5, 0xdc, 0x5e, 0xe2,
  0x55, 0x23, 0x7e, 0x45, 0x89, 0x78, 0xf6, 0x15, 0xff, 0x28, 0x2b, 0x6e,
  0xe3, 0xfe, 0x16, 0xd6, 0x77, 0x9f, 0x7c, 0x2e, 0xd1, 0x56, 0x79, 0x6d,
  0x8b, 0x3f, 0xdc, 0xad, 0x5f, 0x0d, 0x29, 0x0f, 0x5e, 0x0d, 0x8c, 0x93,
  0x3b, 0xa6, 0x12, 0x75, 0x3e, 0x47, 0x01, 0xd5, 0x8d, 0x40, 0x4a, 0x66,
  0x7b, 0x94, 0x71, 0x09, 0x33, 0x71, 0x79, 0xc4, 0x48, 0xe4, 0xab, 0x3f,
  0xcc, 0x3e, 0xdb, 0xd9, 0xce, 0xe1, 0x1b, 0x04, 0xa3, 0x20, 0x61, 0xbf,
  0x5d, 0x0a, 0x64, 0xa6, 0xba, 0x14, 0xf0, 0x38, 0x5b, 0x02, 0x37, 0x05,
  0x24, 0xa4, 0xab, 0x00, 0x3a, 0x89, 0x9c, 0x1f, 0xd9, 0x46, 0xdb, 0xea,
  0x90, 0x63, 0xce, 0x25, 0xac, 0x79, 0xfd, 0x68, 0xca, 0x07, 0xdc, 0x91,
  0x69, 0x0a, 0x58, 0x09, 0xd3, 0xed, 0xe3, 0x86, 0x1e, 0x39, 0x7c, 0x70,
  0x7c, 0x44, 0x82, 0x2f, 0x38, 0xf0, 0x26, 0x9c, 0xfb, 0xdd, 0xc3, 0xb2,
  0x49, 0x27, 0x5b, 0x87, 0xff, 0xf9, 0xe3, 0x2b, 0xef, 0x5a, 0xc7, 0xde,
  0xfd, 0x5d, 0x69, 0x95, 0x19, 0xe5, 0xd7, 0x1c, 0xa6, 0x79, 0x03, 0xb7,
  0x33, 0xb9, 0x5a, 0xf4, 0x12, 0x23, 0xc6, 0x86, 0x96, 0x83, 0x1b, 0x83,
  0xbc, 0x16, 0x63, 0x4d, 0x89, 0x90, 0x7c, 0x3a, 0xe2, 0xe8, 0x4b, 0xf2,
  0x40, 0xaa, 0x71, 0x28, 0x99, 0xb3, 0xd8, 0x3a, 0x12, 0x6b, 0x8b, 0xcd,
  0xa8, 0x1f, 0x93, 0xdd, 0x1c, 0x79, 0x49, 0xdf, 0xa1, 0x25, 0x2a, 0xef,
  0xfb, 0xe4, 0x13, 0xeb, 0xd7, 0x68, 0x44, 0x9f, 0x7e, 0x92, 0x1f, 0xf3,
  0xa1, 0x4b, 0xa4, 0xc0, 0x8a, 0x5e, 0x2f, 0xa3, 0x6f, 0x22, 0xf1, 0x84,
  0xc9, 0x1f, 0x18, 0xf7, 0xeb, 0xeb, 0x5f, 0x98, 0x39, 0x68, 0x89, 0xa6,
  0xc6, 0xee, 0xaa, 0x71, 0x1a, 0x21, 0x00, 0x00, 0x00, 0x0a, 0x41, 0x9a,
  0x21, 0x6c, 0x47, 0x7f, 0xc1, 0xd4, 0x13, 0x49, 0x00, 0x00, 0x00, 0x10,
  0x41, 0x9a, 0x42, 0x3c, 0x21, 0x93, 0x29, 0x84, 0x25, 0xff, 0xbb, 0x5a,
  0xde, 0xd6, 0xcd, 0x81,
};

// 16x8 animated GIF (same bytes the mock serves for video URLs).
static const uint8_t k_gf_gif[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
  0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x00, 0x08, 0x15,
  0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1,
  0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00,
  0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c, 0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08,
  0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03, 0x02, 0x00, 0x21, 0xf9, 0x04, 0x08,
  0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x08, 0x00, 0x81, 0x00, 0x00,
  0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x15, 0x00, 0x01, 0x08, 0x1c,
  0x48, 0xb0, 0xa0, 0xc1, 0x83, 0x08, 0x13, 0x2a, 0x5c, 0xc8, 0xb0, 0xa1, 0xc3, 0x87, 0x08, 0x03,
  0x02, 0x00, 0x3b,
};

typedef struct gf_fixture {
  const char *name;
  const char *page_url;
  const char *src;
  uint32_t format;                  // Discord media format: 1 IMAGE, 2 VIDEO
  uint32_t width, height;           // dimensions the favorite advertises
  const uint8_t *bytes;             // what the mock serves for src
  size_t len;
  int tex_w, tex_h;                 // dimensions of the decoded bytes
  // observed
  unsigned fetched;                 // fetch_media calls the UI made for src
  unsigned delivered;               // MEDIA_LOADED events swapped to `bytes`
  bool done, ok;
  // set per fixture (designated) or by the slice setup
  bool third_party;                 // a GIF-provider host (Giphy, Imgur, tenor.co) the official client loads
  bool refused;                     // a host the live transport does not allow: the load must fail with one log line
  char load[400];                   // the URL the picker loads: src, with "https:" put in front of a "//host/..." src
} gf_fixture;

static gf_fixture gf_fx[] = {
  {"tenor-mp4", "https://tenor.com/view/ccord-dance-gif-100001",
   "https://media.tenor.com/Qw3rTyUiOpAAAAPo/ccord-dance.mp4", 2, 320, 240, k_gf_mp4, sizeof k_gf_mp4, 50, 30, 0, 0, false, false, .refused = false},
  {"discord-net-gif", "https://tenor.com/view/ccord-ship-it-gif-100002",
   "https://media.discordapp.net/attachments/1100000000000000001/1100000000000000002/ship_it.gif", 1, 320, 220,
   k_gf_gif, sizeof k_gf_gif, 16, 8, 0, 0, false, false, .refused = false},
  {"cdn-attachment-query", "https://tenor.com/view/ccord-wave-gif-100003",
   "https://cdn.discordapp.com/attachments/1100000000000000001/1100000000000000003/wave.gif"
   "?ex=6740a1b2&is=673f5032&hm=8f3c2d1e9a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d&",
   1, 320, 240, k_gf_gif, sizeof k_gf_gif, 16, 8, 0, 0, false, false, .refused = false},
  {"tenor-video-no-ext", "https://tenor.com/view/ccord-party-gif-100004",
   "https://media.tenor.com/Zk3x9PqLmNoAAAPo/ccord-party", 2, 320, 240, k_gf_mp4, sizeof k_gf_mp4, 50, 30, 0, 0,
   false, false, .refused = false},
  // A scheme-less src, as a settings-proto might carry one: loaded as https://, stored untouched.
  {"tenor-schemeless", "https://tenor.com/view/ccord-slide-gif-100005",
   "//media.tenor.com/Hn7vBcXzAsAAAAPo/ccord-slide.mp4", 2, 320, 240, k_gf_mp4, sizeof k_gf_mp4, 50, 30, 0, 0, false,
   false, .refused = false},
  // Third-party GIF providers, loaded as in the official client (Giphy, Imgur, legacy tenor.co).
  {"giphy-mp4", "https://tenor.com/view/ccord-giphy-gif-100006",
   "https://media.giphy.com/media/Ab12Cd34Ef/giphy.mp4", 2, 320, 240, k_gf_mp4, sizeof k_gf_mp4, 50, 30, 0, 0, false,
   false, .third_party = true},
  {"imgur-mp4", "https://tenor.com/view/ccord-imgur-gif-100007", "https://i.imgur.com/Xy9Zab1.mp4", 2, 320, 240,
   k_gf_mp4, sizeof k_gf_mp4, 50, 30, 0, 0, false, false, .third_party = true},
  {"tenor-co-video-no-ext", "https://tenor.com/view/ccord-tenorco-gif-100008",
   "https://media.tenor.co/Lm4nOpQrStAAAPo/ccord-tenor-co", 2, 320, 240, k_gf_mp4, sizeof k_gf_mp4, 50, 30, 0, 0,
   false, false, .third_party = true},
  // A host outside the live transport's media allow-list (the path carries a token that must never be logged).
  {"example-off-allowlist", "https://tenor.com/view/ccord-offlist-gif-100009",
   "https://gifs.example.org/media/Ab12Cd34Ef/giphy.gif?token=SECRET-TOKEN-42&sig=abc", 1, 320, 240, k_gf_gif,
   sizeof k_gf_gif, 16, 8, 0, 0, false, false, .refused = true},
};
#define GF_NFX (sizeof gf_fx / sizeof gf_fx[0])

// Each favorite's wire order, deliberately out of fixture order: the Favorites tab must sort by order, highest first,
// not by the order the fixtures are listed in. Fixture 7 (tenor-co) is the newest, then 5 (giphy), then 2 (cdn).
static const uint32_t gf_orders[GF_NFX] = {30, 10, 50, 20, 40, 60, 5, 70, 15};

static const ccord_verify_gif_favorites_ctx *gf_ctx;
static unsigned gf_checks;

// ---- transport wrapper ------------------------------------------------------------------------------------

typedef struct gf_tr_state {
  ccord_transport_vtable patched;
  ccord_status (*orig_fetch)(ccord_transport *, const char *, ccord_error *);
  bool (*orig_poll)(ccord_transport *, ccord_net_event *);
  ccord_status (*orig_patch_settings)(ccord_transport *, const uint8_t *, size_t, ccord_error *);
  unsigned patches;                // settings-proto PATCHes the app sent (the slice's own seeds included)
  uint8_t last_patch[16384];       // the last settings-proto body, to prove which favorites it carried
  size_t last_patch_len;
} gf_tr_state;

static gf_tr_state *gf_tr;

static gf_fixture *gf_find(const char *url)
{
  if (!url) return nullptr;
  for (size_t i = 0; i < GF_NFX; i++) {
    if (strcmp(gf_fx[i].load, url) == 0) return &gf_fx[i];
  }
  return nullptr;
}

static ccord_status gf_fetch_media(ccord_transport *t, const char *url, ccord_error *err)
{
  gf_fixture *f = gf_find(url);
  if (f) f->fetched++;
  // The mock accepts any URL; the live transport refuses what is not https:// on its host list (live_fetch_media,
  // live_transport.c), so the wrapper applies the same gate and error.
  if (!ccord_live_media_url_allowed(url)) return ccord_error_set(err, CCORD_ERR_PERMISSION, "live: media url blocked");
  return gf_tr->orig_fetch(t, url, err);
}

static bool gf_poll(ccord_transport *t, ccord_net_event *out)
{
  if (!gf_tr->orig_poll(t, out)) return false;
  if (out->type == CCORD_NET_MEDIA_LOADED && out->status == CCORD_OK) {
    gf_fixture *f = gf_find(out->media_url);
    if (f) {
      out->media_data = f->bytes;
      out->media_len = f->len;
      f->delivered++;
    }
  }
  return true;
}

// Every settings-proto PATCH goes through here: the count and body are the evidence that starring a GIF saved it to
// Discord (the picker's star must reach ccord_session_toggle_favorite_gif, not the local store).
static ccord_status gf_patch_settings_proto(ccord_transport *t, const uint8_t *proto, size_t proto_len,
                                            ccord_error *err)
{
  gf_tr->patches++;
  gf_tr->last_patch_len = 0;
  if (proto && proto_len <= sizeof gf_tr->last_patch) {
    memcpy(gf_tr->last_patch, proto, proto_len);
    gf_tr->last_patch_len = proto_len;
  }
  return gf_tr->orig_patch_settings(t, proto, proto_len, err);
}

// Decodes the last settings-proto PATCH. *count is its favorite_gifs entry count, has_url/url_order report one URL in
// it, and *top_order is the highest order it carries.
static bool gf_last_patch_gifs(size_t *count, const char *url, bool *has_url, unsigned long *url_order,
                               unsigned long *top_order)
{
  *count = 0;
  *has_url = false;
  *url_order = 0;
  *top_order = 0;
  if (gf_tr->last_patch_len == 0) return false;
  ccord_user_expressions ux = {0};
  ccord_error e = {0};
  bool ok = ccord_user_expressions_decode(gf_tr->last_patch, gf_tr->last_patch_len, &ux, &e) == CCORD_OK;
  if (ok) {
    *count = ux.gif_count;
    for (size_t i = 0; i < ux.gif_count; i++) {
      if (ux.gifs[i].order > *top_order) *top_order = ux.gifs[i].order;
      if (url && ux.gifs[i].url && strcmp(ux.gifs[i].url, url) == 0) {
        *has_url = true;
        *url_order = ux.gifs[i].order;
      }
    }
  }
  ccord_user_expressions_free(&ux);
  return ok;
}

// ---- warning capture ----------------------------------------------------------------------------------------

static struct {
  unsigned n;                       // `media:` warnings seen
  char line[8][320];
} gf_log;

static void gf_log_sink(ccord_log_level level, const char *line, void *ud)
{
  (void)ud;
  if (level == CCORD_LOG_WARN && strstr(line, "media:")) {
    if (gf_log.n < 8u) (void)snprintf(gf_log.line[gf_log.n], sizeof gf_log.line[0], "%s", line);
    gf_log.n++;
    return;
  }
  fprintf(stderr, "%s\n", line);
}

static void gf_log_end(void)
{
  ccord_log_set_sink(nullptr, nullptr);
  ccord_log_set_level(CCORD_LOG_INFO);
}

// ---- helpers ------------------------------------------------------------------------------------------------

static int gf_fail(const char *what)
{
  gf_log_end();
  printf("gif-favorites: FAIL %s\n", what);
  fflush(stdout);
  return 1;
}

[[gnu::format(printf, 1, 2)]] static int gf_failf(const char *fmt, ...)
{
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return gf_fail(buf);
}

static void gf_tick(int n) { gf_ctx->frames(gf_ctx->ud, n); }

static void gf_click_button(float x, float y, uint8_t button)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  gf_ctx->input(gf_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = button, .clicks = 1, .x = x, .y = y};
  gf_ctx->input(gf_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = button, .clicks = 1, .x = x, .y = y};
  gf_ctx->input(gf_ctx->ud, &ev);
}

static void gf_click(float x, float y) { gf_click_button(x, y, 1); }

static void gf_wheel(float x, float y, float notches)
{
  ccord_input_event ev = {.type = CCORD_IN_WHEEL, .x = x, .y = y, .wheel = notches};
  gf_ctx->input(gf_ctx->ud, &ev);
}

// ---- picker tile order ----------------------------------------------------------------------------------------

// A GIF tile as the picker placed it in a frame: its favorite's order, or -1 when the store does not hold it.
typedef struct {
  char url[CCORD_URL_MAX_BYTES + 1];
  long long order;
} gf_tile;
#define GF_TILE_MAX 32u
static char gf_order_bad[640];   // the first newest-first violation seen by gf_scan_order (empty when none)

static long long gf_fav_order(const ccord_store *store, const char *url)
{
  for (size_t i = 0, n = ccord_store_favorite_gif_count(store); i < n; i++) {
    const ccord_gif_item *c = ccord_store_favorite_gif_at(store, i);
    if (c && strcmp(c->url, url) == 0) return (long long)c->order;
  }
  return -1;
}

static long long gf_top_order(const ccord_store *store)
{
  long long top = -1;
  for (size_t i = 0, n = ccord_store_favorite_gif_count(store); i < n; i++) {
    const ccord_gif_item *c = ccord_store_favorite_gif_at(store, i);
    if (c && (long long)c->order > top) top = (long long)c->order;
  }
  return top;
}

// The GIF tiles drawn in the last frame, in placement order (the picker writes its hit records in draw order).
static size_t gf_read_tiles(const ccord_ui *ui, const ccord_store *store, gf_tile *out, size_t cap)
{
  ui_expr_inspect ins;
  size_t n = 0;
  if (!ui_expr_get_inspect(ui, &ins)) return 0;
  for (size_t h = 0; h < ins.nhits && n < cap; h++) {
    ui_expr_hit_info hi;
    if (!ui_expr_hit_at(ui, h, &hi) || hi.kind != 2) continue;   // kind 2 = GIF tile
    (void)snprintf(out[n].url, sizeof out[n].url, "%s", hi.url);
    out[n].order = gf_fav_order(store, hi.url);
    n++;
  }
  return n;
}

// Newest first: every tile drawn is a favorite, and each tile drawn after another has a strictly lower order.
// The first violation is kept in gf_order_bad.
static void gf_scan_order(const ccord_ui *ui, const ccord_store *store)
{
  gf_tile seq[GF_TILE_MAX];
  size_t n = gf_read_tiles(ui, store, seq, GF_TILE_MAX);
  for (size_t i = 0; i < n && gf_order_bad[0] == '\0'; i++) {
    if (seq[i].order < 0) {
      (void)snprintf(gf_order_bad, sizeof gf_order_bad, "order newest-first: tile %.120s is drawn but is not a favorite",
                     seq[i].url);
    } else if (i > 0 && seq[i - 1].order <= seq[i].order) {
      (void)snprintf(gf_order_bad, sizeof gf_order_bad,
                     "order newest-first: tile %.120s (order %lld) is drawn after %.120s (order %lld) in one frame",
                     seq[i].url, seq[i].order, seq[i - 1].url, seq[i - 1].order);
    }
  }
}

// Scrolls the picker grid back to the top and waits until it is there (the scroll may be eased over frames).
static bool gf_scroll_top(const ccord_ui *ui)
{
  ui_expr_inspect ins;
  for (int i = 0; i < GF_TICK_MAX; i++) {
    if (!ui_expr_get_inspect(ui, &ins)) return false;
    if (ins.grid_scroll_y <= 0.0f) return true;
    if (i % 30 == 0) gf_wheel(ins.grid_view.x + ins.grid_view.w * 0.5f, ins.grid_view.y + ins.grid_view.h * 0.5f, 10.0f);
    gf_tick(1);
  }
  return false;
}

// The GIF tile drawn for `url` in the last frame, when its whole star button sits inside the scroll viewport (a click on
// the star centre then reaches the picker instead of the clip edge).
static bool gf_tile_by_url(const ccord_ui *ui, const char *url, ui_expr_hit_info *out)
{
  ui_expr_inspect ins;
  if (!ui_expr_get_inspect(ui, &ins)) return false;
  for (size_t h = 0; h < ins.nhits; h++) {
    ui_expr_hit_info hi;
    if (!ui_expr_hit_at(ui, h, &hi) || hi.kind != 2 || strcmp(hi.url, url) != 0) continue;
    const ccord_rect s = hi.star_rect, v = ins.grid_view;
    if (s.w <= 0.0f || s.x < v.x || s.y < v.y || s.x + s.w > v.x + v.w || s.y + s.h > v.y + v.h) return false;
    *out = hi;
    return true;
  }
  return false;
}

// An un-favorited trending GIF tile that is clickable right now (skipping `except`).
static bool gf_trend_tile(const ccord_ui *ui, const ccord_store *store, const char *except, ui_expr_hit_info *out)
{
  for (size_t i = 0, n = ccord_store_gif_trending_count(store); i < n; i++) {
    const ccord_gif_item *g = ccord_store_gif_trending_at(store, i);
    if (!g || ccord_store_is_favorite_gif(store, g->url)) continue;
    if (except && strcmp(except, g->url) == 0) continue;
    if (gf_tile_by_url(ui, g->url, out)) return true;
  }
  return false;
}

// Scrolls the Home grid down until such a trending tile is drawn and clickable.
static bool gf_reveal_trend(const ccord_ui *ui, const ccord_store *store, const char *except, ui_expr_hit_info *out)
{
  for (int tick = 0; tick < GF_TICK_MAX; tick++) {
    if (gf_trend_tile(ui, store, except, out)) return true;
    if (tick % 10 == 9) {
      ui_expr_inspect ins;
      if (ui_expr_get_inspect(ui, &ins)) {
        gf_wheel(ins.grid_view.x + ins.grid_view.w * 0.5f, ins.grid_view.y + ins.grid_view.h * 0.5f, -2.0f);
      }
    }
    gf_tick(1);
  }
  return false;
}

// Clicks the Favorites card on the Home view and waits for the Favorites subview.
static bool gf_open_favorites(ccord_ui *ui)
{
  ui_expr_inspect ins;
  ccord_rect card = {0, 0, 0, 0};
  for (int i = 0; i < 100 && card.w == 0.0f; i++) {
    gf_tick(1);
    if (!ui_expr_get_inspect(ui, &ins)) continue;
    for (size_t h = 0; h < ins.nhits; h++) {
      ui_expr_hit_info hi;
      if (ui_expr_hit_at(ui, h, &hi) && hi.kind == 4) card = hi.rect;
    }
  }
  if (card.w == 0.0f) return false;
  gf_click(card.x + card.w * 0.5f, card.y + card.h * 0.5f);
  gf_tick(1);
  return ui_expr_get_inspect(ui, &ins) && ins.gif_subview == 1;
}

// Minimal protobuf writer: FrecencyUserSettings root field 2 = favorite_gifs { repeated field 1 = entry
// { 1: url, 2: gif { 1: format, 2: src, 3: width, 4: height, 5: order } } } (discord_proto.c parse_gif_value).
static size_t gf_varint(uint8_t *p, uint64_t v)
{
  size_t n = 0;
  do {
    p[n] = (uint8_t)((v & 0x7fu) | (v > 0x7fu ? 0x80u : 0u));
    v >>= 7;
    n++;
  } while (v != 0);
  return n;
}

static size_t gf_bytes(uint8_t *p, unsigned field, const void *data, size_t len)
{
  size_t n = gf_varint(p, ((uint64_t)field << 3u) | 2u);
  n += gf_varint(p + n, len);
  memcpy(p + n, data, len);
  return n + len;
}

static size_t gf_uint(uint8_t *p, unsigned field, uint64_t v)
{
  size_t n = gf_varint(p, (uint64_t)field << 3u);
  return n + gf_varint(p + n, v);
}

static size_t gf_build_settings(uint8_t *out, const uint32_t *orders)
{
  uint8_t gifs[4096];
  size_t gn = 0;
  for (size_t i = 0; i < GF_NFX; i++) {
    const gf_fixture *f = &gf_fx[i];
    uint8_t gif[512], ent[1024];
    size_t n = 0;
    n += gf_uint(gif + n, 1, f->format);
    n += gf_bytes(gif + n, 2, f->src, strlen(f->src));
    n += gf_uint(gif + n, 3, f->width);
    n += gf_uint(gif + n, 4, f->height);
    n += gf_uint(gif + n, 5, orders[i]);
    size_t en = 0;
    en += gf_bytes(ent + en, 1, f->page_url, strlen(f->page_url));
    en += gf_bytes(ent + en, 2, gif, n);
    gn += gf_bytes(gifs + gn, 1, ent, en);
  }
  return gf_bytes(out, 2, gifs, gn);
}

// ---- the slice ----------------------------------------------------------------------------------------------

static int gf_run_body(const ccord_verify_gif_favorites_ctx *ctx)
{
  gf_ctx = ctx;
  for (size_t i = 0; i < GF_NFX; i++) {
    const char *src = gf_fx[i].src;
    (void)snprintf(gf_fx[i].load, sizeof gf_fx[i].load, "%s%s", src[0] == '/' && src[1] == '/' ? "https:" : "", src);
  }
  ccord_log_set_sink(gf_log_sink, nullptr);
  ccord_log_set_level(CCORD_LOG_WARN);
  memset(&gf_log, 0, sizeof gf_log);
  ccord_store *store = ctx->store;
  ccord_ui *ui = ctx->ui;
  ccord_renderer *r = ctx->renderer;
  ccord_error err = {0};

  gf_tr->orig_fetch = ctx->transport->vt->fetch_media;
  gf_tr->orig_poll = ctx->transport->vt->poll;
  gf_tr->orig_patch_settings = ctx->transport->vt->patch_settings_proto;
  gf_tr->patched = *ctx->transport->vt;
  gf_tr->patched.fetch_media = gf_fetch_media;
  gf_tr->patched.poll = gf_poll;
  gf_tr->patched.patch_settings_proto = gf_patch_settings_proto;
  gf_tr->patches = 0;
  gf_tr->last_patch_len = 0;
  ctx->transport->vt = &gf_tr->patched;

  // The mock connects and auto-selects a channel; wait for that like the other mock slices do.
  ccord_id chan = CCORD_ID_NONE;
  for (int i = 0; i < 400 && chan == CCORD_ID_NONE; i++) {
    gf_tick(1);
    if (ccord_session_state(ctx->session) == CCORD_CONN_CONNECTED) chan = ccord_store_selected_channel(store);
  }
  if (chan == CCORD_ID_NONE) return gf_fail("setup: mock did not connect and select a channel");
  // The mock auto-selects a forum channel, and a forum's post list takes clicks under an open picker before the picker
  // does. Open the picker over the mock's text channel (the channel the other picker slices use) instead.
  if (ccord_session_select_channel(ctx->session, GF_CHAN_TEXT, &err) != CCORD_OK) {
    return gf_failf("setup: select the text channel: %s", err.message);
  }
  for (int i = 0; i < 100 && ccord_store_selected_channel(store) != GF_CHAN_TEXT; i++) gf_tick(1);
  chan = ccord_store_selected_channel(store);
  if (chan != GF_CHAN_TEXT) return gf_fail("setup: the mock text channel did not become selected");

  // 1. Open the GIF picker: the session requests the settings-proto (the mock's seeded favorites).
  ui_expr_picker_close(ui);
  ui_expr_picker_open(ui, 0, chan, CCORD_ID_NONE);
  for (int i = 0; i < 200; i++) {
    gf_tick(1);
    if (ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY) break;
  }
  if (ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, nullptr) != CCORD_EXPR_BLOB_READY) {
    return gf_fail("settings-proto did not load");
  }
  gf_checks++;

  // 2. Deliver the real-shaped favorites exactly as the server would: a settings-proto PATCH makes the mock answer
  // with the raw protobuf, which the session decodes (discord_proto) into the store.
  uint8_t raw[8192];
  size_t rn = gf_build_settings(raw, gf_orders);
  if (ccord_transport_patch_settings_proto(ctx->transport, raw, rn, &err) != CCORD_OK) {
    return gf_failf("settings-proto patch refused: %s", err.message);
  }
  for (int i = 0; i < 200; i++) {
    gf_tick(1);
    bool all = true;
    for (size_t k = 0; k < GF_NFX; k++) all = all && ccord_store_is_favorite_gif(store, gf_fx[k].page_url);
    if (all) break;
  }

  // 3. Favorites parse: every favorite is in the store with the src/format/size that was on the wire.
  size_t nfav = ccord_store_favorite_gif_count(store);
  for (size_t k = 0; k < GF_NFX; k++) {
    const gf_fixture *f = &gf_fx[k];
    const ccord_gif_item *it = nullptr;
    for (size_t i = 0; i < nfav && !it; i++) {
      const ccord_gif_item *c = ccord_store_favorite_gif_at(store, i);
      if (c && strcmp(c->url, f->page_url) == 0) it = c;
    }
    if (!it) return gf_failf("favorites parse: %s is not in the store", f->name);
    if (strcmp(it->src, f->src) != 0) return gf_failf("favorites parse: %s src is '%s'", f->name, it->src);
    if (it->format != f->format || it->width != f->width || it->height != f->height) {
      return gf_failf("favorites parse: %s format/size differ from the wire", f->name);
    }
    gf_checks++;
  }

  // 4. URL allow-list: the live transport refuses media hosts it does not know; these are the shapes Discord sends.
  for (size_t k = 0; k < GF_NFX; k++) {
    bool allowed = ccord_live_media_url_allowed(gf_fx[k].load);
    if (allowed == gf_fx[k].refused) {
      return gf_failf("url rewrite/allow-list: live transport %s %s (%s)", allowed ? "would load" : "would refuse",
                      gf_fx[k].name, gf_fx[k].load);
    }
    gf_checks++;
  }

  // 5. Favorites view through the real picker: click the Favorites card, then pump frames, scrolling the grid so
  // every tile is drawn at least once (off-screen tiles are culled and never request their media).
  ui_expr_inspect ins;
  ccord_rect fav_card = {0, 0, 0, 0};
  for (int i = 0; i < 100 && fav_card.w == 0.0f; i++) {
    gf_tick(1);
    if (!ui_expr_get_inspect(ui, &ins)) continue;
    for (size_t h = 0; h < ins.nhits; h++) {
      ui_expr_hit_info hi;
      if (ui_expr_hit_at(ui, h, &hi) && hi.kind == 4) fav_card = hi.rect;
    }
  }
  if (fav_card.w == 0.0f) return gf_fail("picker: Favorites card missing");
  gf_click(fav_card.x + fav_card.w * 0.5f, fav_card.y + fav_card.h * 0.5f);
  gf_tick(1);
  if (!ui_expr_get_inspect(ui, &ins) || ins.gif_subview != 1) return gf_fail("picker: Favorites view did not open");
  gf_checks++;

  // 5a. Newest first: the first tile drawn is the highest order, and the tiles of this frame are in order.
  gf_tile top_seq[GF_TILE_MAX];
  size_t ntop = gf_read_tiles(ui, store, top_seq, GF_TILE_MAX);
  gf_scan_order(ui, store);
  if (gf_order_bad[0] != '\0') return gf_fail(gf_order_bad);
  if (ntop == 0 || top_seq[0].order != gf_top_order(store)) {
    return gf_failf("order newest-first: first tile is order %lld, the highest favorite is order %lld",
                    ntop ? top_seq[0].order : -1LL, gf_top_order(store));
  }
  gf_checks += 2;

  size_t pending = GF_NFX;
  int scrolls = 0;
  for (int tick = 0; tick < GF_TICK_MAX && pending > 0; tick++) {
    gf_tick(1);
    gf_scan_order(ui, store);   // every frame of the scroll: tiles drawn in one frame stay newest first
    pending = 0;
    for (size_t k = 0; k < GF_NFX; k++) {
      gf_fixture *f = &gf_fx[k];
      if (f->done) continue;
      int w = 0, h = 0;
      if (ccord_render_has_texture(r, f->load, &w, &h)) {
        f->done = true;
        f->ok = !f->refused && w == f->tex_w && h == f->tex_h && f->delivered > 0u;
      } else if (ui_media_failed(ui, f->load)) {
        f->done = true;
        f->ok = f->refused && f->delivered == 0u;   // a refused host must fail, and must never reach the transport
      } else {
        pending++;
      }
    }
    // Scroll on when some tile has not even been requested (it is below the fold).
    if (pending > 0 && tick % 40 == 39 && scrolls < 12 && ui_expr_get_inspect(ui, &ins)) {
      bool unrequested = false;
      for (size_t k = 0; k < GF_NFX; k++) unrequested = unrequested || (!gf_fx[k].done && gf_fx[k].fetched == 0u);
      if (unrequested) {
        gf_wheel(ins.grid_view.x + ins.grid_view.w * 0.5f, ins.grid_view.y + ins.grid_view.h * 0.5f, -3.0f);
        scrolls++;
      }
    }
  }

  if (gf_order_bad[0] != '\0') return gf_fail(gf_order_bad);
  gf_checks++;

  // 6. Verdict per tile: name the stage that lost it.
  for (size_t k = 0; k < GF_NFX; k++) {
    const gf_fixture *f = &gf_fx[k];
    int w = 0, h = 0;
    bool has = ccord_render_has_texture(r, f->load, &w, &h);
    char buf[640];   // a fixture URL is up to 399 bytes
    if (f->ok) {
      gf_checks++;
      continue;
    }
    if (f->refused) {
      (void)snprintf(buf, sizeof buf, "refusal: %s (%s) %s", f->name, f->load,
                     has ? "got a texture although the live transport refuses its host"
                         : "was not marked failed (ui_media_failed=0)");
    } else if (f->fetched == 0u) {
      (void)snprintf(buf, sizeof buf, "request: %s was never fetched (media_failed=%d) src=%s", f->name,
                     ui_media_failed(ui, f->load) ? 1 : 0, f->load);
    } else if (f->delivered == 0u) {
      (void)snprintf(buf, sizeof buf, "content: %s was requested %u time(s) but the mock delivered nothing", f->name,
                     f->fetched);
    } else if (!has) {
      (void)snprintf(buf, sizeof buf,
                     "decode: %s delivered %zu bytes (%s) but no texture was made (ui_media_failed=%d, saved with "
                     "VIDEO format=%d)",
                     f->name, f->len, f->bytes == k_gf_mp4 ? "mp4" : "gif", ui_media_failed(ui, f->load) ? 1 : 0,
                     f->format == 2u);
    } else {
      (void)snprintf(buf, sizeof buf, "decode: %s texture is %dx%d, expected %dx%d from the served bytes", f->name, w,
                     h, f->tex_w, f->tex_h);
    }
    return gf_fail(buf);
  }

  // 7. The stored favorites are untouched by the load-time https:// fix-up (a star click would write them back).
  nfav = ccord_store_favorite_gif_count(store);
  for (size_t k = 0; k < GF_NFX; k++) {
    for (size_t i = 0; i < nfav; i++) {
      const ccord_gif_item *c = ccord_store_favorite_gif_at(store, i);
      if (c && strcmp(c->url, gf_fx[k].page_url) == 0 && strcmp(c->src, gf_fx[k].src) != 0) {
        return gf_failf("stored src: %s was rewritten to '%s'", gf_fx[k].name, c->src);
      }
    }
  }
  gf_checks++;

  // 8. One `media:` warning per failed load: the refused host, its host name only, no path, token or query.
  unsigned want = 0;
  for (size_t k = 0; k < GF_NFX; k++) want += gf_fx[k].refused ? 1u : 0u;
  for (int i = 0; i < 30; i++) gf_tick(1);   // a failed tile is drawn on every frame; it must not log again
  if (gf_log.n != want) {
    return gf_failf("log: expected %u media: warning(s) for the refused host(s), got %u (first: %s)", want, gf_log.n,
                    gf_log.n ? gf_log.line[0] : "none");
  }
  for (size_t k = 0; k < GF_NFX; k++) {
    if (!gf_fx[k].refused) continue;
    const char *ln = gf_log.line[0];
    if (!strstr(ln, "refused") || !strstr(ln, "host=gifs.example.org") || strstr(ln, "SECRET") || strchr(ln, '?') ||
        strstr(ln, "giphy.gif") || strstr(ln, "token") || strstr(ln, "https:")) {
      return gf_failf("log: warning for %s must name the reason and the bare host only: %s", gf_fx[k].name, ln);
    }
    printf("gif-favorites: %s logged: %s\n", gf_fx[k].name, ln);
    gf_checks++;
  }
  gf_log_end();

  // 9. Third-party GIF providers: each passed the real allow-list and drew a texture of the served size.
  bool third_ok = true;
  unsigned third_n = 0;
  for (size_t k = 0; k < GF_NFX; k++) {
    if (!gf_fx[k].third_party) continue;
    third_n++;
    third_ok = third_ok && ccord_live_media_url_allowed(gf_fx[k].load) && gf_fx[k].ok;
  }
  printf("gif_third_party_hosts_ok=%d\n", third_ok && third_n > 0u ? 1 : 0);
  if (!third_ok || third_n == 0u) return gf_fail("third-party hosts: a Giphy/Imgur/tenor.co fixture did not load");
  gf_checks++;

  // 10. A settings update that swaps two orders swaps their tiles, and a GIF favorited now is drawn first. Both are
  // checked at the top of the grid, where the first tiles are the newest favorites.
  if (!gf_scroll_top(ui)) return gf_fail("order newest-first: the Favorites grid did not scroll to the top");
  gf_tile first_seq[GF_TILE_MAX];
  size_t nfirst = gf_read_tiles(ui, store, first_seq, GF_TILE_MAX);
  if (nfirst < 2 || strcmp(first_seq[0].url, gf_fx[7].page_url) != 0 || strcmp(first_seq[1].url, gf_fx[5].page_url) != 0) {
    return gf_fail("order newest-first: the two newest favorites are not the first two tiles");
  }
  uint32_t swapped[GF_NFX];
  memcpy(swapped, gf_orders, sizeof swapped);
  swapped[7] = gf_orders[5];
  swapped[5] = gf_orders[7];
  uint8_t raw2[8192];
  size_t rn2 = gf_build_settings(raw2, swapped);
  if (ccord_transport_patch_settings_proto(ctx->transport, raw2, rn2, &err) != CCORD_OK) {
    return gf_failf("settings-proto swap refused: %s", err.message);
  }
  for (int i = 0; i < 200 && gf_fav_order(store, gf_fx[7].page_url) != (long long)swapped[7]; i++) gf_tick(1);
  if (gf_fav_order(store, gf_fx[7].page_url) != (long long)swapped[7] ||
      gf_fav_order(store, gf_fx[5].page_url) != (long long)swapped[5]) {
    return gf_fail("order update: the settings update did not re-order the two favorites");
  }
  gf_checks++;
  gf_tick(1);
  if (!gf_scroll_top(ui)) return gf_fail("order newest-first: the Favorites grid did not scroll to the top");
  gf_tile swap_seq[GF_TILE_MAX];
  size_t nswap = gf_read_tiles(ui, store, swap_seq, GF_TILE_MAX);
  gf_scan_order(ui, store);
  if (gf_order_bad[0] != '\0') return gf_fail(gf_order_bad);
  if (nswap < 2 || strcmp(swap_seq[0].url, gf_fx[5].page_url) != 0 || strcmp(swap_seq[1].url, gf_fx[7].page_url) != 0) {
    return gf_fail("order update: the settings update did not swap the two tiles");
  }
  gf_checks += 2;

  // A GIF favorited now carries the highest order and is drawn first. This adds through the store directly, so the
  // store's own newest-first rule is checked without the transport in the way.
  ccord_gif_item fresh = {.width = 320, .height = 240};
  snprintf(fresh.url, sizeof fresh.url, "https://tenor.com/view/ccord-fresh-gif-100010");
  snprintf(fresh.src, sizeof fresh.src, "https://media.tenor.com/Fr35hF4vOrD/ccord-fresh.mp4");
  if (!ccord_store_toggle_favorite_gif(store, &fresh)) return gf_fail("order newest-first: the new favorite was not stored");
  gf_tick(1);
  if (!gf_scroll_top(ui)) return gf_fail("order newest-first: the Favorites grid did not scroll to the top");
  gf_tile fresh_seq[GF_TILE_MAX];
  size_t nfresh = gf_read_tiles(ui, store, fresh_seq, GF_TILE_MAX);
  gf_scan_order(ui, store);
  if (gf_order_bad[0] != '\0') return gf_fail(gf_order_bad);
  if (nfresh == 0 || strcmp(fresh_seq[0].url, fresh.url) != 0) {
    return gf_fail("order newest-first: a GIF favorited now is not the first tile");
  }
  gf_checks += 2;

  // 11. Starring in the picker saves the favorite to Discord: the star on a trending tile goes through
  // ccord_session_toggle_favorite_gif, which PATCHes the settings proto once and carries the new list with the GIF as
  // the highest order. (A picker that called the store directly would send nothing and fail right here.)
  ui_expr_picker_close(ui);
  ui_expr_picker_open(ui, 0, chan, CCORD_ID_NONE);
  for (int i = 0; i < 300 && ccord_store_gif_trending_count(store) == 0; i++) gf_tick(1);
  if (ccord_store_gif_trending_count(store) == 0) return gf_fail("star click: the mock trending GIFs did not load");
  ui_expr_hit_info tile_a;
  if (!gf_reveal_trend(ui, store, nullptr, &tile_a)) return gf_fail("star click: no clickable trending GIF tile");
  unsigned n_before = gf_tr->patches;
  size_t fav_before = ccord_store_favorite_gif_count(store);
  gf_click(tile_a.star_rect.x + tile_a.star_rect.w * 0.5f, tile_a.star_rect.y + tile_a.star_rect.h * 0.5f);
  gf_tick(1);
  if (gf_tr->patches != n_before + 1u) {
    return gf_failf("star click: expected exactly 1 settings-proto PATCH, got %u", gf_tr->patches - n_before);
  }
  gf_checks++;
  {
    size_t count = 0;
    bool has = false;
    unsigned long order = 0, top = 0;
    if (!gf_last_patch_gifs(&count, tile_a.url, &has, &order, &top)) {
      return gf_fail("star click: the PATCH body is not a readable settings proto");
    }
    if (!has) return gf_failf("star click: the PATCH does not carry the starred GIF %.90s", tile_a.url);
    if (order != top || top == 0) {
      return gf_failf("star click: the starred GIF is order %lu in the PATCH, not the highest (%lu)", order, top);
    }
    if (count != fav_before + 1u) {
      return gf_failf("star click: the PATCH carries %zu favorite(s), expected %zu", count, fav_before + 1u);
    }
    gf_checks += 2;
  }
  if (!ccord_store_is_favorite_gif(store, tile_a.url) || ccord_store_favorite_gif_count(store) != fav_before + 1u) {
    return gf_failf("star click: %.90s is not a favorite after the PATCH", tile_a.url);
  }
  gf_checks++;

  // 12. The right-click on a tile is the same star action and must also reach Discord.
  ui_expr_hit_info tile_b;
  if (!gf_reveal_trend(ui, store, tile_a.url, &tile_b)) return gf_fail("right-click: no second clickable trending GIF tile");
  unsigned rb_before = gf_tr->patches;
  size_t fav_rb = ccord_store_favorite_gif_count(store);
  gf_click_button(tile_b.rect.x + tile_b.rect.w * 0.5f, tile_b.rect.y + tile_b.rect.h * 0.5f, 3);
  gf_tick(1);
  if (gf_tr->patches != rb_before + 1u) {
    return gf_failf("right-click: expected exactly 1 settings-proto PATCH, got %u", gf_tr->patches - rb_before);
  }
  gf_checks++;
  {
    size_t count = 0;
    bool has = false;
    unsigned long order = 0, top = 0;
    if (!gf_last_patch_gifs(&count, tile_b.url, &has, &order, &top)) {
      return gf_fail("right-click: the PATCH body is not a readable settings proto");
    }
    if (!has) return gf_failf("right-click: the PATCH does not carry the right-clicked GIF %.90s", tile_b.url);
    if (order != top || top == 0) {
      return gf_failf("right-click: the right-clicked GIF is order %lu in the PATCH, not the highest (%lu)", order, top);
    }
    if (count != fav_rb + 1u) {
      return gf_failf("right-click: the PATCH carries %zu favorite(s), expected %zu", count, fav_rb + 1u);
    }
    gf_checks += 2;
  }
  if (!ccord_store_is_favorite_gif(store, tile_b.url) || ccord_store_favorite_gif_count(store) != fav_rb + 1u) {
    return gf_failf("right-click: %.90s is not a favorite after the PATCH", tile_b.url);
  }
  gf_checks++;

  // 13. The GIF starred in the picker is the first favorite on the Favorites tab, the right-clicked one second.
  if (!gf_scroll_top(ui)) return gf_fail("star click: the Home grid did not scroll to the top");
  if (!gf_open_favorites(ui)) return gf_fail("star click: the Favorites view did not open");
  gf_tick(1);
  gf_tile star_seq[GF_TILE_MAX];
  size_t nstar = gf_read_tiles(ui, store, star_seq, GF_TILE_MAX);
  gf_scan_order(ui, store);
  if (gf_order_bad[0] != '\0') return gf_fail(gf_order_bad);
  if (nstar < 2 || strcmp(star_seq[0].url, tile_b.url) != 0 || strcmp(star_seq[1].url, tile_a.url) != 0) {
    return gf_fail("star click: the GIFs starred in the picker are not the two newest favorites in order");
  }
  gf_checks += 2;

  // 14. Un-starring the first tile sends one more PATCH that no longer carries it, and the tile leaves Favorites.
  ui_expr_hit_info first;
  if (!gf_tile_by_url(ui, tile_b.url, &first)) return gf_fail("un-star click: the first Favorites tile is not clickable");
  unsigned un_before = gf_tr->patches;
  size_t fav_un = ccord_store_favorite_gif_count(store);
  gf_click(first.star_rect.x + first.star_rect.w * 0.5f, first.star_rect.y + first.star_rect.h * 0.5f);
  gf_tick(1);
  if (gf_tr->patches != un_before + 1u) {
    return gf_failf("un-star click: expected exactly 1 settings-proto PATCH, got %u", gf_tr->patches - un_before);
  }
  gf_checks++;
  {
    size_t count = 0;
    bool has = false;
    unsigned long order = 0, top = 0;
    if (!gf_last_patch_gifs(&count, tile_b.url, &has, &order, &top)) {
      return gf_fail("un-star click: the PATCH body is not a readable settings proto");
    }
    if (has) return gf_failf("un-star click: the PATCH still carries %.90s", tile_b.url);
    if (count != fav_un - 1u) {
      return gf_failf("un-star click: the PATCH carries %zu favorite(s), expected %zu", count, fav_un - 1u);
    }
    gf_checks += 2;
  }
  if (ccord_store_is_favorite_gif(store, tile_b.url) || ccord_store_favorite_gif_count(store) != fav_un - 1u) {
    return gf_failf("un-star click: %.90s is still a favorite after the PATCH", tile_b.url);
  }
  gf_checks++;
  gf_tick(1);
  {
    gf_tile un_seq[GF_TILE_MAX];
    size_t nun = gf_read_tiles(ui, store, un_seq, GF_TILE_MAX);
    gf_scan_order(ui, store);
    if (gf_order_bad[0] != '\0') return gf_fail(gf_order_bad);
    if (nun == 0 || strcmp(un_seq[0].url, tile_a.url) != 0) {
      return gf_fail("un-star click: the un-starred GIF is still drawn first in Favorites");
    }
  }
  gf_checks++;

  printf("gif-favorites: order newest-first ok\n");

  printf("gif-favorites: PASS checks=%u\n", gf_checks);
  fflush(stdout);
  return 0;
}

int ccord_verify_gif_favorites_run(const ccord_verify_gif_favorites_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->transport || !ctx->frames ||
      !ctx->input) {
    return gf_fail("invalid context");
  }
  const ccord_transport_vtable *saved_vt = ctx->transport->vt;
  gf_tr = calloc(1, sizeof *gf_tr);
  if (!gf_tr) return gf_fail("out of memory");
  int rc = gf_run_body(ctx);
  if (ctx->transport->vt == &gf_tr->patched) ctx->transport->vt = saved_vt;
  free(gf_tr);
  gf_tr = nullptr;
  return rc;
}
