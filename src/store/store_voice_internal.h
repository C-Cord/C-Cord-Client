// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "store_internal.h"

#define VOICE_STATUS_MAX_CHARS 500u

typedef struct status_row {
  ccord_id channel_id;
  char *text;
} status_row;

typedef struct stream_row {
  ccord_stream_state state;
  uint32_t viewer_count;
} stream_row;

typedef struct audio_row {
  ccord_id user_id;
  uint8_t volume_pct;
  bool local_mute;
} audio_row;

struct voice_cache {
  ccord_call_state *calls;
  size_t ncalls, capcalls;
  ccord_stage_instance *stages;
  size_t nstages, capstages;
  status_row *status;
  size_t nstatus, capstatus;
  stream_row *streams;
  size_t nstreams, capstreams;
  ccord_activity_instance *activities;
  size_t nactivities, capactivities;
  audio_row *audio;
  size_t naudio, capaudio;
  ccord_dave_status dave;
  bool has_dave;
  ccord_rtc_stats rtc;
  bool has_rtc;
};

struct voice_cache *store_voice_cache_get(ccord_store *s, bool create);
bool store_voice_inline_text_ok(const char *text, size_t cap);
void store_voice_activity_remove_at(struct voice_cache *c, size_t i);
