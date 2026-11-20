// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Discord-hosted media verification (--verify-slice media-load).
//
// The mock's own answer for any media URL is a 1x1 PNG stub that the UI quietly replaces with a fallback pixel, which
// hides every real decode failure. This slice wraps the mock's vtable instead: fetch_media applies the live
// transport's host allow-list (as live_fetch_media_gated does) and forwards, and poll() swaps the bytes of a fixture
// URL's MEDIA_LOADED event for real GIF / PNG / MP4 bytes and delivers the JSON messages as MESSAGE_CREATE events.
// Discord's media proxy is imitated: a URL that asks for `width=` gets a small picture, one that does not gets the
// full-size original. A fixture passes only when the texture holds exactly the dimensions of the bytes served.
//
//   discord-hosted gif  GIF-picker favorites at a signed cdn.discordapp.com/attachments/..gif?ex=&is=&hm= URL and a
//                       media.discordapp.net one load next to a Tenor and a Giphy tile
//   embed media         timeline embeds (image, thumbnail, GIFV) load through their proxy_url
//   embed lightbox      clicking the embed image's hit rect opens the expanded view and its full picture loads
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/error.h>
#include <ccord/core/log.h>
#include <ccord/net/live_transport.h>
#include <ccord/store/expressions.h>

#include "ui_internal.h"
#include "verify_ui_parity.h"

#define ML_CHAN_TEXT 3121ULL   // the mock's text channel (verify_expressions.c EX_CHAN)
#define ML_MSG_BASE 1600000000000000000ull

// 50x30 yuv420p H.264 MP4, 3 frames; 16x8 animated GIF (the bytes verify_gif_favorites.c serves).
static const uint8_t k_ml_mp4[] = {
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
static const uint8_t k_ml_gif[] = {
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

// ---- fixtures -----------------------------------------------------------------------------------------------

typedef enum ml_kind { ML_PNG, ML_GIF, ML_MP4 } ml_kind;

typedef struct ml_fx {
  const char *name;
  const char *base;     // the URL up to its '?'; every request whose URL starts with it is answered
  ml_kind kind;
  int w, h;             // dimensions of the full-size picture served for the URL as it is
  int sw, sh;           // ... and of the screen-sized copy the proxy serves when the URL asks for width=
  // observed
  uint8_t *bytes, *small;
  size_t len, small_len;
  unsigned fetched, delivered;
  char url_small[2100], url_full[2100];   // the last URL answered with the screen-sized / the full-size picture
  char loaded_url[2100];   // picker: the URL a texture of the served dimensions was found under
} ml_fx;

static const char k_sig[] = "?ex=6740a1b2&is=673f5032&hm=8f3c2d1e9a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d&";

enum {
  FX_TENOR, FX_GIPHY, FX_CDN_GIF, FX_NET_GIF,           // GIF picker favorites
  FX_EMB_IMAGE, FX_EMB_THUMB, FX_EMB_HUGE, FX_EMB_GIFV, FX_EMB_NET, FX_EMB_NODIMS,   // timeline embeds
  FX_ATT_IMAGE,                                         // attachment image (lightbox control)
  FX_COUNT
};

static ml_fx fx[FX_COUNT] = {
  [FX_TENOR] = {"tenor", "https://media.tenor.com/Qw3rTyUiOpAAAAPo/ccord-dance.mp4", ML_MP4, 50, 30, 50, 30},
  [FX_GIPHY] = {"giphy", "https://media.giphy.com/media/Ab12Cd34Ef/giphy.mp4", ML_MP4, 50, 30, 50, 30},
  [FX_CDN_GIF] = {"cdn-attachment-gif",
                  "https://cdn.discordapp.com/attachments/1100000000000000001/1100000000000000003/wave.gif", ML_GIF, 16, 8,
                  16, 8},
  [FX_NET_GIF] = {"media-net-gif",
                  "https://media.discordapp.net/attachments/1100000000000000001/1100000000000000002/ship_it.gif", ML_GIF,
                  16, 8, 16, 8},
  [FX_EMB_IMAGE] = {"embed-image",
                    "https://images-ext-1.discordapp.net/external/Zq3Lm0pXr9T2uVwYb7KdEfGhIjNoPsAcBvCxDzEyFgH/https/"
                    "i.imgur.com/Xy9Zab1.png",
                    ML_PNG, 1200, 800, 160, 107},
  [FX_EMB_THUMB] = {"embed-thumbnail",
                    "https://images-ext-2.discordapp.net/external/Hh7Jj1Kk2Ll3Mm4Nn5Oo6Pp8Qq9Rr0SsTtUuVvWwXxYyZz/https/"
                    "example.org/og/card.jpg",
                    ML_PNG, 600, 600, 60, 60},
  [FX_EMB_HUGE] = {"embed-image-over-4096",
                   "https://media.discordapp.net/attachments/1100000000000000001/1100000000000000021/panorama.png", ML_PNG,
                   4500, 24, 300, 3},
  [FX_EMB_GIFV] = {"embed-gifv",
                   "https://images-ext-1.discordapp.net/external/Aa1Bb2Cc3Dd4Ee5Ff6Gg7Hh8Ii9Jj0KkLlMmNnOoPpQqRr/https/"
                   "media.tenor.com/Zk3x9PqLmNoAAAPo/party.mp4",
                   ML_MP4, 50, 30, 50, 30},
  [FX_EMB_NET] = {"embed-attachment-image",
                  "https://media.discordapp.net/attachments/1100000000000000001/1100000000000000022/screenshot.png",
                  ML_PNG, 900, 600, 120, 80},
  [FX_EMB_NODIMS] = {"embed-image-no-size",
                     "https://media.discordapp.net/attachments/1100000000000000001/1100000000000000024/wallpaper.png",
                     ML_PNG, 4400, 30, 80, 1},
  [FX_ATT_IMAGE] = {"attachment-image",
                    "https://cdn.discordapp.com/attachments/1100000000000000001/1100000000000000023/photo.png", ML_PNG, 90,
                    60, 90, 60},
};

// ---- tiny PNG writer (stored deflate: no zlib needed) --------------------------------------------------------

static uint32_t ml_crc(const uint8_t *p, size_t n, uint32_t crc)
{
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static void ml_be32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static size_t ml_chunk(uint8_t *out, const char *type, const uint8_t *data, size_t n)
{
  ml_be32(out, (uint32_t)n);
  memcpy(out + 4, type, 4);
  if (n) memcpy(out + 8, data, n);
  uint32_t c = ml_crc((const uint8_t *)type, 4, 0);
  c = ml_crc(data, n, c);
  ml_be32(out + 8 + n, c);
  return n + 12u;
}

// An RGBA PNG of one flat colour per row band, w x h.
static uint8_t *ml_png(int w, int h, size_t *out_len)
{
  size_t row = 1u + (size_t)w * 4u, raw = row * (size_t)h;
  uint8_t *img = malloc(raw);
  if (!img) return nullptr;
  for (int y = 0; y < h; y++) {
    uint8_t *p = img + (size_t)y * row;
    *p++ = 0;
    for (int x = 0; x < w; x++) {
      *p++ = (uint8_t)(40 + (x * 3) % 180);
      *p++ = (uint8_t)(60 + (y * 5) % 160);
      *p++ = 0xc0;
      *p++ = 0xff;
    }
  }
  size_t nblocks = (raw + 65534u) / 65535u;
  size_t zlen = 2u + raw + nblocks * 5u + 4u;
  uint8_t *z = malloc(zlen);
  uint8_t *png = malloc(8u + 25u + (zlen + 12u) + 12u);
  if (!z || !png) {
    free(img);
    free(z);
    free(png);
    return nullptr;
  }
  size_t zi = 0;
  z[zi++] = 0x78;
  z[zi++] = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t off = 0; off < raw; off += 65535u) {
    size_t n = raw - off < 65535u ? raw - off : 65535u;
    z[zi++] = off + n >= raw ? 1 : 0;
    z[zi++] = (uint8_t)(n & 0xffu);
    z[zi++] = (uint8_t)(n >> 8);
    z[zi++] = (uint8_t)(~n & 0xffu);
    z[zi++] = (uint8_t)((~n >> 8) & 0xffu);
    memcpy(z + zi, img + off, n);
    zi += n;
    for (size_t i = 0; i < n; i++) {
      a = (a + img[off + i]) % 65521u;
      b = (b + a) % 65521u;
    }
  }
  ml_be32(z + zi, (b << 16) | a);
  zi += 4;
  size_t pi = 0;
  memcpy(png, "\x89PNG\r\n\x1a\n", 8);
  pi = 8;
  uint8_t ihdr[13];
  ml_be32(ihdr, (uint32_t)w);
  ml_be32(ihdr + 4, (uint32_t)h);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = ihdr[11] = ihdr[12] = 0;
  pi += ml_chunk(png + pi, "IHDR", ihdr, 13);
  pi += ml_chunk(png + pi, "IDAT", z, zi);
  pi += ml_chunk(png + pi, "IEND", nullptr, 0);
  free(img);
  free(z);
  *out_len = pi;
  return png;
}

static bool ml_make_bytes(ml_fx *f)
{
  if (f->kind == ML_PNG) {
    f->bytes = ml_png(f->w, f->h, &f->len);
    f->small = ml_png(f->sw, f->sh, &f->small_len);
    return f->bytes && f->small;
  }
  const uint8_t *src = f->kind == ML_GIF ? k_ml_gif : k_ml_mp4;
  size_t n = f->kind == ML_GIF ? sizeof k_ml_gif : sizeof k_ml_mp4;
  f->bytes = malloc(n);
  if (!f->bytes) return false;
  memcpy(f->bytes, src, n);
  f->len = n;
  f->small = f->bytes;
  f->small_len = n;
  return true;
}

static void ml_free_bytes(void)
{
  for (size_t i = 0; i < FX_COUNT; i++) {
    if (fx[i].small != fx[i].bytes) free(fx[i].small);
    free(fx[i].bytes);
    fx[i].bytes = fx[i].small = nullptr;
  }
}

// ---- transport wrapper ---------------------------------------------------------------------------------------

static const ccord_verify_ui_parity_ctx *ml_ctx;
static unsigned ml_checks;

static struct {
  ccord_transport_vtable patched;
  ccord_status (*orig_fetch)(ccord_transport *, const char *, ccord_error *);
  bool (*orig_poll)(ccord_transport *, ccord_net_event *);
  ccord_live_event held;
  bool have_held, pending;
  unsigned refused;       // fetches the live allow-list refused
} ml_tr;

static struct {
  unsigned n;
  char line[6][320];
} ml_log;

static ml_fx *ml_find(const char *url, bool *sized)
{
  if (!url) return nullptr;
  for (size_t i = 0; i < FX_COUNT; i++) {
    size_t n = strlen(fx[i].base);
    if (strncmp(url, fx[i].base, n) == 0 && (url[n] == '\0' || url[n] == '?')) {
      if (sized) *sized = strstr(url, "width=") != nullptr;
      return &fx[i];
    }
  }
  return nullptr;
}

static ccord_status ml_fetch_media(ccord_transport *t, const char *url, ccord_error *err)
{
  ml_fx *f = ml_find(url, nullptr);
  // The mock accepts any URL; the live transport refuses what is not https:// on its host list, so the wrapper
  // applies the same gate and error.
  if (!ccord_live_media_url_allowed(url)) {
    ml_tr.refused++;
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "live: media url blocked");
  }
  if (f) f->fetched++;
  return ml_tr.orig_fetch(t, url, err);
}

static bool ml_poll(ccord_transport *t, ccord_net_event *out)
{
  if (ml_tr.orig_poll(t, out)) {
    if (out->type == CCORD_NET_MEDIA_LOADED && out->status == CCORD_OK) {
      bool sized = false;
      ml_fx *f = ml_find(out->media_url, &sized);
      if (f) {
        out->media_data = sized ? f->small : f->bytes;
        out->media_len = sized ? f->small_len : f->len;
        f->delivered++;
        snprintf(sized ? f->url_small : f->url_full, sizeof f->url_small, "%s", out->media_url);
      }
    }
    return true;
  }
  if (ml_tr.pending) {
    ml_tr.pending = false;
    *out = ml_tr.held.ev;
    return true;
  }
  return false;
}

static void ml_log_sink(ccord_log_level level, const char *line, void *ud)
{
  (void)ud;
  if (level == CCORD_LOG_WARN && strstr(line, "media:")) {
    if (ml_log.n < 6u) (void)snprintf(ml_log.line[ml_log.n], sizeof ml_log.line[0], "%s", line);
    ml_log.n++;
    return;
  }
  fprintf(stderr, "%s\n", line);
}

static void ml_end(void)
{
  ccord_log_set_sink(nullptr, nullptr);
  ccord_log_set_level(CCORD_LOG_INFO);
  if (ml_tr.have_held) ccord_live_event_free(&ml_tr.held);
  ml_tr.have_held = false;
  ml_free_bytes();
}

static int ml_fail(const char *what)
{
  printf("media-load: FAIL %s\n", what);
  for (unsigned i = 0; i < ml_log.n && i < 6u; i++) printf("media-load:   log: %s\n", ml_log.line[i]);
  fflush(stdout);
  ml_end();
  return 1;
}

[[gnu::format(printf, 1, 2)]] static int ml_failf(const char *fmt, ...)
{
  char buf[500];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return ml_fail(buf);
}

// ---- helpers --------------------------------------------------------------------------------------------------

static void ml_tick(int n) { ml_ctx->frames(ml_ctx->ud, n); }

static void ml_click(float x, float y)
{
  ccord_input_event ev = {.type = CCORD_IN_MOUSE_MOVE, .x = x, .y = y};
  ml_ctx->input(ml_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_DOWN, .button = 1, .clicks = 1, .x = x, .y = y};
  ml_ctx->input(ml_ctx->ud, &ev);
  ev = (ccord_input_event){.type = CCORD_IN_MOUSE_UP, .button = 1, .clicks = 1, .x = x, .y = y};
  ml_ctx->input(ml_ctx->ud, &ev);
}

static bool ml_inject(const char *json)
{
  if (ml_tr.have_held) ccord_live_event_free(&ml_tr.held);
  ml_tr.have_held = false;
  ccord_error err = {0};
  if (ccord_live_json_message(json, strlen(json), CCORD_NET_MESSAGE_CREATE, &ml_tr.held, &err) != CCORD_OK) {
    printf("media-load: (parse) %s\n", err.message);
    return false;
  }
  ml_tr.have_held = true;
  ml_tr.pending = true;
  return true;
}

// True when the renderer holds a texture under `url` of exactly w x h.
static bool ml_tex(const char *url, int w, int h)
{
  int tw = 0, th = 0;
  return ccord_render_has_texture(ml_ctx->renderer, url, &tw, &th) && tw == w && th == h;
}

// True when the texture is f's dimensions, or those dimensions scaled down to fit the decoder's size limit.
static bool ml_tex_like(const char *url, int w, int h)
{
  int tw = 0, th = 0;
  if (!ccord_render_has_texture(ml_ctx->renderer, url, &tw, &th) || tw <= 0 || th <= 0) return false;
  if (tw == w && th == h) return true;
  if (tw >= w || th >= h) return false;
  long d = (long)tw * h - (long)th * w;
  return (d < 0 ? -d : d) <= (long)(w > h ? w : h);
}

// A texture of fixture f's served pictures is held: the screen-sized copy, or the full-size one.
static bool ml_loaded(const ml_fx *f)
{
  return (f->url_small[0] && ml_tex_like(f->url_small, f->sw, f->sh)) ||
         (f->url_full[0] && ml_tex_like(f->url_full, f->w, f->h));
}

static bool ml_wait_loaded(const ml_fx *f, int max)
{
  for (int i = 0; i < max && !ml_loaded(f); i++) ml_tick(1);
  return ml_loaded(f);
}

static const ui_img_hit *ml_img_hit(const char *url)
{
  for (size_t i = 0; i < ml_ctx->ui->nimg_hits; i++) {
    if (strcmp(ml_ctx->ui->img_hits[i].urls[0], url) == 0) return &ml_ctx->ui->img_hits[i];
  }
  return nullptr;
}

static char ml_msg[6000];

static const char *ml_embed_msg(unsigned idx, unsigned long long chan, const char *embeds_json, const char *atts_json)
{
  (void)snprintf(ml_msg, sizeof ml_msg,
                 "{\"id\":\"%llu\",\"channel_id\":\"%llu\",\"author\":{\"id\":\"1003\",\"username\":\"Grace\"},"
                 "\"content\":\"media-load %u\",\"timestamp\":\"2025-01-01T00:00:%02u.000000+00:00\","
                 "\"attachments\":[%s],\"embeds\":[%s]}",
                 ML_MSG_BASE + idx, chan, idx, idx, atts_json ? atts_json : "", embeds_json ? embeds_json : "");
  return ml_msg;
}

// Opens the lightbox by clicking the hit rect recorded for `url` and waits for a texture under what the lightbox
// loads. Closes it again. `why` names the case in a failure.
static int ml_lightbox(const char *why, const char *url, int w, int h)
{
  const ui_img_hit *hit = nullptr;
  for (int i = 0; i < 60 && !hit; i++) {
    hit = ml_img_hit(url);
    if (!hit) ml_tick(1);
  }
  if (!hit) return ml_failf("%s: no image hit rect for the embed image URL", why);
  ccord_rect rc = hit->rect;
  ml_click(rc.x + rc.w * 0.5f, rc.y + rc.h * 0.5f);
  ml_tick(1);
  ccord_ui *ui = ml_ctx->ui;
  if (!ui->lightbox_open) return ml_failf("%s: clicking the image did not open the expanded view", why);
  if (strcmp(ui->lightbox_url, url) != 0) return ml_failf("%s: the expanded view holds a different URL", why);
  bool ready = false;
  for (int i = 0; i < 400 && !ready; i++) {
    ml_tick(1);
    ready = ml_tex_like(ui->lightbox_url, w, h);
    if (!ready && ui_media_failed(ui, ui->lightbox_url)) break;
  }
  if (!ready) {
    int tw = 0, th = 0;
    bool has = ccord_render_has_texture(ml_ctx->renderer, ui->lightbox_url, &tw, &th);
    return ml_failf("%s: the expanded view never got its picture (texture %s %dx%d, want %dx%d, failed=%d)", why,
                    has ? "held" : "missing", tw, th, w, h, ui_media_failed(ui, ui->lightbox_url) ? 1 : 0);
  }
  ml_click(ui->lightbox_close_btn.x + 16.0f, ui->lightbox_close_btn.y + 16.0f);
  ml_tick(2);
  if (ui->lightbox_open) return ml_failf("%s: the expanded view did not close", why);
  return 0;
}

// ---- GIF picker favorites ----------------------------------------------------------------------------------

static bool ml_open_favorites(ccord_ui *ui)
{
  ui_expr_inspect ins;
  ccord_rect card = {0, 0, 0, 0};
  for (int i = 0; i < 100 && card.w == 0.0f; i++) {
    ml_tick(1);
    if (!ui_expr_get_inspect(ui, &ins)) continue;
    for (size_t h = 0; h < ins.nhits; h++) {
      ui_expr_hit_info hi;
      if (ui_expr_hit_at(ui, h, &hi) && hi.kind == 4) card = hi.rect;
    }
  }
  if (card.w == 0.0f) return false;
  ml_click(card.x + card.w * 0.5f, card.y + card.h * 0.5f);
  ml_tick(1);
  return ui_expr_get_inspect(ui, &ins) && ins.gif_subview == 1;
}

static void ml_fav(ccord_store *store, const char *page, const ml_fx *f, const char *query, uint32_t format)
{
  ccord_gif_item it;
  memset(&it, 0, sizeof it);
  snprintf(it.url, sizeof it.url, "%s", page);
  snprintf(it.src, sizeof it.src, "%s%s", f->base, query);
  it.width = 320;
  it.height = 240;
  it.format = format;
  (void)ccord_store_toggle_favorite_gif(store, &it);
}

static int ml_picker(ccord_id chan)
{
  ccord_ui *ui = ml_ctx->ui;
  ccord_store *store = ml_ctx->store;
  ui_expr_picker_close(ui);
  ui_expr_picker_open(ui, 0, chan, CCORD_ID_NONE);
  for (int i = 0; i < 200; i++) {
    ml_tick(1);
    if (ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, nullptr) == CCORD_EXPR_BLOB_READY) break;
  }
  if (ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, nullptr) != CCORD_EXPR_BLOB_READY) {
    return ml_fail("picker: settings-proto did not load");
  }
  // Favorites as Discord stores them: a Tenor and a Giphy video, a GIF uploaded to Discord at a signed
  // cdn.discordapp.com attachment URL, and one at a media.discordapp.net URL.
  ml_fav(store, "https://tenor.com/view/ccord-dance-gif-100001", &fx[FX_TENOR], "", 2);
  ml_fav(store, "https://giphy.com/gifs/ccord-giphy-Ab12Cd34Ef", &fx[FX_GIPHY], "", 2);
  ml_fav(store, "https://tenor.com/view/ccord-wave-gif-100003", &fx[FX_CDN_GIF], k_sig, 1);
  ml_fav(store, "https://tenor.com/view/ccord-ship-it-gif-100002", &fx[FX_NET_GIF], k_sig, 1);
  if (!ml_open_favorites(ui)) return ml_fail("picker: the Favorites view did not open");
  for (int i = 0; i < 600; i++) {
    ml_tick(1);
    for (size_t k = FX_TENOR; k <= FX_NET_GIF; k++) {
      if (!fx[k].loaded_url[0]) {
        char url[2100];
        snprintf(url, sizeof url, "%s%s", fx[k].base, k >= FX_CDN_GIF ? k_sig : "");
        if (ml_tex(url, fx[k].w, fx[k].h)) snprintf(fx[k].loaded_url, sizeof fx[k].loaded_url, "%s", url);
      }
    }
    bool done = true;
    for (size_t k = FX_TENOR; k <= FX_NET_GIF; k++) done = done && fx[k].loaded_url[0];
    if (done) break;
  }
  for (size_t k = FX_TENOR; k <= FX_NET_GIF; k++) {
    if (!fx[k].loaded_url[0]) {
      return ml_failf("discord-hosted gif: picker tile %s never loaded (fetched=%u delivered=%u)", fx[k].name,
                      fx[k].fetched, fx[k].delivered);
    }
    ml_checks++;
  }
  ui_expr_picker_close(ui);
  ml_tick(2);
  printf("media-load: discord-hosted gif ok\n");
  return 0;
}

// ---- allow-list ----------------------------------------------------------------------------------------------

static int ml_gate(void)
{
  static const char *const yes[] = {
    "https://cdn.discordapp.com/attachments/1/2/a.gif?ex=1&is=2&hm=3&",
    "https://media.discordapp.net/attachments/1/2/a.gif?ex=1&is=2&hm=3&format=webp&width=256&height=128",
    "https://images-ext-1.discordapp.net/external/abc/https/i.imgur.com/x.png?width=640&height=360",
    "https://images-ext-2.discordapp.net/external/abc/https/example.org/x.png",
    "https://media.tenor.com/Qw3rTyUiOpAAAAPo/x.mp4",
    "https://media.giphy.com/media/Ab12Cd34Ef/giphy.mp4",
  };
  static const char *const no[] = {
    "http://cdn.discordapp.com/attachments/1/2/a.gif",
    "https://cdn.discordapp.com.evil.example/attachments/1/2/a.gif",
    "https://evil.example/cdn.discordapp.com/attachments/1/2/a.gif",
    "https://images-ext-3.discordapp.net/external/abc/https/example.org/x.png",
    "https://discordapp.com/attachments/1/2/a.gif",
    "https://gifs.example.org/media/Ab12Cd34Ef/giphy.gif?token=SECRET",
    "ftp://cdn.discordapp.com/attachments/1/2/a.gif",
    "//cdn.discordapp.com/attachments/1/2/a.gif",
  };
  for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) {
    if (!ccord_live_media_url_allowed(yes[i])) return ml_failf("allow-list: refuses a Discord media URL (%s)", yes[i]);
    ml_checks++;
  }
  for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
    if (ccord_live_media_url_allowed(no[i])) return ml_failf("allow-list: accepts %s", no[i]);
    ml_checks++;
  }
  // An expired signed attachment URL is refreshed (POST /attachments/refresh-urls) before it is given up on; only a
  // signed attachment URL on Discord's CDN hosts qualifies, and the fresh URL goes through the same allow-list.
  static const char *const stale[] = {
    "https://cdn.discordapp.com/attachments/1/2/a.gif?ex=1&is=2&hm=3&",
    "https://media.discordapp.net/attachments/1/2/a.gif?ex=1&is=2&hm=3&format=webp&width=256&height=128",
  };
  static const char *const fresh_not[] = {
    "https://cdn.discordapp.com/attachments/1/2/a.gif",                        // unsigned: never expires
    "https://cdn.discordapp.com/avatars/1/abc.png?ex=1&is=2&hm=3&",            // not an attachment
    "https://media.tenor.com/Qw3rTyUiOpAAAAPo/x.mp4?ex=1",                     // not Discord's CDN
    "http://cdn.discordapp.com/attachments/1/2/a.gif?ex=1&is=2&hm=3&",         // not https
    "https://images-ext-1.discordapp.net/external/abc/https/x.org/a.png?ex=1", // external proxy: nothing to refresh
  };
  for (size_t i = 0; i < sizeof stale / sizeof stale[0]; i++) {
    if (!ccord_live_media_url_refreshable(stale[i])) return ml_failf("refresh: a signed attachment URL is not refreshable");
    ml_checks++;
  }
  for (size_t i = 0; i < sizeof fresh_not / sizeof fresh_not[0]; i++) {
    if (ccord_live_media_url_refreshable(fresh_not[i])) return ml_failf("refresh: %s must not be refreshed", fresh_not[i]);
    ml_checks++;
  }
  ccord_buf body;
  ccord_buf_init(&body, 0);
  ccord_error be = {0};
  bool body_ok = ccord_live_media_refresh_body(stale[0], &body, &be) == CCORD_OK &&
                 strcmp(ccord_buf_cstr(&body), "{\"attachment_urls\":[\"https://cdn.discordapp.com/attachments/1/2/a.gif"
                                              "?ex=1&is=2&hm=3&\"]}") == 0;
  ccord_buf_free(&body);
  if (!body_ok) return ml_fail("refresh: request body is not {\"attachment_urls\":[url]}");
  char fresh[300];
  static const char answer[] =
    "{\"refreshed_urls\":[{\"original\":\"https://cdn.discordapp.com/attachments/1/2/a.gif?ex=1&is=2&hm=3&\","
    "\"refreshed\":\"https://cdn.discordapp.com/attachments/1/2/a.gif?ex=9a&is=9b&hm=9c&\"}]}";
  if (!ccord_live_media_refresh_parse(answer, sizeof answer - 1u, stale[0], fresh, sizeof fresh) ||
      strcmp(fresh, "https://cdn.discordapp.com/attachments/1/2/a.gif?ex=9a&is=9b&hm=9c&") != 0) {
    return ml_fail("refresh: the answer's refreshed URL was not read");
  }
  if (ccord_live_media_refresh_parse("{\"refreshed_urls\":[]}", 22, stale[0], fresh, sizeof fresh) ||
      ccord_live_media_refresh_parse("[]", 2, stale[0], fresh, sizeof fresh)) {
    return ml_fail("refresh: an answer without a refreshed URL was accepted");
  }
  ml_checks += 4;
  return 0;
}

// ---- the slice -------------------------------------------------------------------------------------------------

int ccord_verify_media_load_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  if (!ctx || !ctx->session || !ctx->store || !ctx->ui || !ctx->renderer || !ctx->transport || !ctx->frames ||
      !ctx->input) {
    printf("media-load: FAIL invalid context\n");
    return 1;
  }
  ml_ctx = ctx;
  ml_checks = 0;
  memset(&ml_log, 0, sizeof ml_log);
  for (size_t i = 0; i < FX_COUNT; i++) {
    fx[i].fetched = fx[i].delivered = 0;
    fx[i].url_small[0] = fx[i].url_full[0] = fx[i].loaded_url[0] = '\0';
    if (!ml_make_bytes(&fx[i])) return ml_fail("setup: out of memory building fixtures");
  }
  ccord_log_set_sink(ml_log_sink, nullptr);
  ccord_log_set_level(CCORD_LOG_WARN);

  ml_tr.orig_fetch = ctx->transport->vt->fetch_media;
  ml_tr.orig_poll = ctx->transport->vt->poll;
  ml_tr.patched = *ctx->transport->vt;
  ml_tr.patched.fetch_media = ml_fetch_media;
  ml_tr.patched.poll = ml_poll;
  ctx->transport->vt = &ml_tr.patched;

  ccord_ui *ui = ctx->ui;
  ccord_id chan = CCORD_ID_NONE;
  for (int i = 0; i < 400 && chan == CCORD_ID_NONE; i++) {
    ml_tick(1);
    if (ccord_session_state(ctx->session) == CCORD_CONN_CONNECTED) chan = ccord_store_selected_channel(ctx->store);
  }
  if (chan == CCORD_ID_NONE) return ml_fail("setup: mock did not connect and select a channel");
  ccord_error err = {0};
  if (ccord_session_select_channel(ctx->session, ML_CHAN_TEXT, &err) != CCORD_OK) {
    return ml_failf("setup: select the text channel: %s", err.message);
  }
  for (int i = 0; i < 100 && ccord_store_selected_channel(ctx->store) != ML_CHAN_TEXT; i++) ml_tick(1);
  chan = ccord_store_selected_channel(ctx->store);
  if (chan != ML_CHAN_TEXT) return ml_fail("setup: the mock text channel did not become selected");

  int rc = ml_gate();
  if (rc) return rc;
  rc = ml_picker(chan);
  if (rc) return rc;

  // ---- timeline embeds ----------------------------------------------------------------------------------------
  char url_sig[600], emb[2400], att[2000];
  unsigned msg = 1;
  const unsigned long long ch = (unsigned long long)chan;

  // 1. A rich embed: image + thumbnail, both through Discord's external-media proxy (no query, like a real one).
  (void)snprintf(emb, sizeof emb,
                 "{\"type\":\"rich\",\"title\":\"Card\",\"description\":\"an article\",\"url\":\"https://example.org/p\","
                 "\"image\":{\"url\":\"https://i.imgur.com/Xy9Zab1.png\",\"proxy_url\":\"%s\",\"width\":1200,\"height\":800},"
                 "\"thumbnail\":{\"url\":\"https://example.org/og/card.jpg\",\"proxy_url\":\"%s\",\"width\":600,"
                 "\"height\":600}}",
                 fx[FX_EMB_IMAGE].base, fx[FX_EMB_THUMB].base);
  if (!ml_inject(ml_embed_msg(msg++, ch, emb, nullptr))) return ml_fail("setup: embed message did not parse");
  if (!ml_wait_loaded(&fx[FX_EMB_IMAGE], 300)) {
    return ml_failf("embed media: the embed image never loaded through its proxy_url (fetched=%u delivered=%u)",
                    fx[FX_EMB_IMAGE].fetched, fx[FX_EMB_IMAGE].delivered);
  }
  if (!ml_wait_loaded(&fx[FX_EMB_THUMB], 300)) {
    return ml_failf("embed media: the embed thumbnail never loaded through its proxy_url (fetched=%u delivered=%u)",
                    fx[FX_EMB_THUMB].fetched, fx[FX_EMB_THUMB].delivered);
  }
  ml_checks += 2;
  // The expanded view of the embed image: the click, then the full picture (the proxy answers the plain URL with the
  // original).
  rc = ml_lightbox("embed image", fx[FX_EMB_IMAGE].base, fx[FX_EMB_IMAGE].w, fx[FX_EMB_IMAGE].h);
  if (rc) return rc;
  ml_checks++;

  // 2. A GIFV embed (a Tenor link, including the provider object Discord always sends): the clip plays framelessly
  // from video.proxy_url, its still comes from the thumbnail, and it is never rendered as a video card.
  (void)snprintf(emb, sizeof emb,
                 "{\"type\":\"gifv\",\"url\":\"https://tenor.com/view/party-gif-1\","
                 "\"provider\":{\"name\":\"Tenor\",\"url\":\"https://tenor.co\"},"
                 "\"thumbnail\":{\"url\":\"https://example.org/og/card.jpg\",\"proxy_url\":\"%s\",\"width\":600,"
                 "\"height\":600},"
                 "\"video\":{\"url\":\"https://media.tenor.com/Zk3x9PqLmNoAAAPo/party.mp4\",\"proxy_url\":\"%s\","
                 "\"width\":50,\"height\":30}}",
                 fx[FX_EMB_THUMB].base, fx[FX_EMB_GIFV].base);
  if (!ml_inject(ml_embed_msg(msg++, ch, emb, nullptr))) return ml_fail("setup: gifv message did not parse");
  if (!ml_wait_loaded(&fx[FX_EMB_GIFV], 300)) {
    return ml_failf("embed media: the GIFV clip never loaded through video.proxy_url (fetched=%u delivered=%u)",
                    fx[FX_EMB_GIFV].fetched, fx[FX_EMB_GIFV].delivered);
  }
  ml_checks++;
  printf("media-load: embed media ok\n");
  rc = ml_lightbox("embed gifv", fx[FX_EMB_GIFV].base, fx[FX_EMB_GIFV].w, fx[FX_EMB_GIFV].h);
  if (rc) return rc;
  ml_checks++;

  // 3. An image embed of a Discord attachment link: signed media.discordapp.net proxy_url, a picture wider than the
  // decoder's 4096 px limit (a panorama or a long screenshot).
  (void)snprintf(url_sig, sizeof url_sig, "%s%s", fx[FX_EMB_HUGE].base, k_sig);
  (void)snprintf(emb, sizeof emb,
                 "{\"type\":\"rich\",\"title\":\"Panorama\",\"url\":\"https://example.org/pano\","
                 "\"image\":{\"url\":\"%s\",\"proxy_url\":\"%s\",\"width\":4500,\"height\":24}}",
                 url_sig, url_sig);
  if (!ml_inject(ml_embed_msg(msg++, ch, emb, nullptr))) return ml_fail("setup: panorama message did not parse");
  if (!ml_wait_loaded(&fx[FX_EMB_HUGE], 300)) {
    return ml_failf("embed media: the signed attachment embed image never loaded (fetched=%u delivered=%u)",
                    fx[FX_EMB_HUGE].fetched, fx[FX_EMB_HUGE].delivered);
  }
  ml_checks++;

  // 3b. The same without advertised dimensions: the timeline cannot ask the proxy for a smaller copy, so the original
  // itself has to load there.
  (void)snprintf(url_sig, sizeof url_sig, "%s%s", fx[FX_EMB_NODIMS].base, k_sig);
  (void)snprintf(emb, sizeof emb,
                 "{\"type\":\"rich\",\"title\":\"Wallpaper\",\"url\":\"https://example.org/wall\","
                 "\"image\":{\"url\":\"%s\",\"proxy_url\":\"%s\"}}",
                 url_sig, url_sig);
  if (!ml_inject(ml_embed_msg(msg++, ch, emb, nullptr))) return ml_fail("setup: wallpaper message did not parse");
  if (!ml_wait_loaded(&fx[FX_EMB_NODIMS], 300)) {
    return ml_failf("embed media: an embed image without dimensions never loaded in the timeline (fetched=%u delivered=%u)",
                    fx[FX_EMB_NODIMS].fetched, fx[FX_EMB_NODIMS].delivered);
  }
  ml_checks++;
  (void)snprintf(url_sig, sizeof url_sig, "%s%s", fx[FX_EMB_HUGE].base, k_sig);
  rc = ml_lightbox("embed image over 4096 px", url_sig, fx[FX_EMB_HUGE].w, fx[FX_EMB_HUGE].h);
  if (rc) return rc;
  ml_checks++;

  // 4. A bare image embed (a pasted attachment link): the media is in `thumbnail`, signed proxy_url.
  (void)snprintf(url_sig, sizeof url_sig, "%s%s", fx[FX_EMB_NET].base, k_sig);
  (void)snprintf(emb, sizeof emb,
                 "{\"type\":\"image\",\"url\":\"https://cdn.discordapp.com/attachments/1100000000000000001/"
                 "1100000000000000022/screenshot.png\","
                 "\"thumbnail\":{\"url\":\"%s\",\"proxy_url\":\"%s\",\"width\":900,\"height\":600}}",
                 url_sig, url_sig);
  if (!ml_inject(ml_embed_msg(msg++, ch, emb, nullptr))) return ml_fail("setup: image embed message did not parse");
  if (!ml_wait_loaded(&fx[FX_EMB_NET], 300)) {
    return ml_failf("embed media: the bare image embed never loaded (fetched=%u delivered=%u)", fx[FX_EMB_NET].fetched,
                    fx[FX_EMB_NET].delivered);
  }
  ml_checks++;
  rc = ml_lightbox("bare image embed", url_sig, fx[FX_EMB_NET].w, fx[FX_EMB_NET].h);
  if (rc) return rc;
  ml_checks++;
  printf("media-load: embed lightbox ok\n");

  // 5. Control: an attachment image's lightbox, the path that already worked.
  (void)snprintf(url_sig, sizeof url_sig, "%s%s", fx[FX_ATT_IMAGE].base, k_sig);
  (void)snprintf(att, sizeof att,
                 "{\"id\":\"%llu\",\"filename\":\"photo.png\",\"content_type\":\"image/png\",\"size\":1234,"
                 "\"width\":90,\"height\":60,\"url\":\"%s\",\"proxy_url\":\"%s\"}",
                 ML_MSG_BASE + 900u, url_sig, url_sig);
  if (!ml_inject(ml_embed_msg(msg++, ch, nullptr, att))) return ml_fail("setup: attachment message did not parse");
  if (!ml_wait_loaded(&fx[FX_ATT_IMAGE], 300)) return ml_fail("attachment image never loaded");
  rc = ml_lightbox("attachment image", url_sig, fx[FX_ATT_IMAGE].w, fx[FX_ATT_IMAGE].h);
  if (rc) return rc;
  ml_checks += 2;

  // Nothing the UI asked for may have been refused by the host list.
  if (ml_tr.refused != 0) return ml_failf("allow-list: %u media request(s) were refused", ml_tr.refused);
  ml_checks++;
  (void)ui;
  ml_end();
  printf("media-load: PASS checks=%u\n", ml_checks);
  fflush(stdout);
  return 0;
}
