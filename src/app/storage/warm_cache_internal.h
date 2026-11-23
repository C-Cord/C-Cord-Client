// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "warm_cache.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/ini.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/sha256.h>
#include <ccord/model/notify.h>
#include <ccord/net/live_transport.h>
#include <ccord/store/notify.h>
#include <ccord/store/roles.h>

#ifndef CCORD_LIVE_ACCOUNT
#define CCORD_LIVE_ACCOUNT 0
#endif

#define WARM_MAGIC "CCWARM"
#define WARM_HEADER_MAX 160u
#define WARM_MAX_LOAD_USERS 100000u   // the model's own snapshot limit

struct ccord_warm {
  char data_dir[1024];
  char dir[1100];
  char fp[33];
  char app_version[32];
  int64_t (*now_ms)(void *ud);
  void *clock_ud;
  bool enabled;
  bool forgotten;
  size_t max_bytes;
  ccord_id bound_self;       // the account this directory belongs to once known; saves for another one are refused
  uint64_t saved_rev;        // store revision of the last save
  bool saved_once;
  uint64_t last_save_mono;   // monotonic ms of the last save
  uint64_t connected_since;  // monotonic ms the session was first seen connected and fresh (0 = not yet)
  ccord_id deferred_hist_ids[CCORD_WARM_MAX_HISTORY_CHANNELS * 4];
  size_t deferred_hist_count;
  size_t deferred_hist_seeded;
};

// Shared internal helpers (warm_cache.c)
static inline const char *nz(const char *s) { return s ? s : ""; }
int64_t warm_now(const ccord_warm *w);
bool warm_state_path(const ccord_warm *w, char *out, size_t cap);
bool warm_history_path(const ccord_warm *w, ccord_id ch, char *out, size_t cap);
bool warm_parse_header_line(const char *text, size_t n, unsigned *version, char hex[65], size_t *hdr_len, size_t *body_len);
bool warm_verify_body_sha256(const char *body, size_t blen, const char hex[65]);
ccord_json_limits warm_limits(void);

// Snapshot JSON writer & loader (warm_cache_codec.c)
ccord_status warm_build_state(const ccord_warm *w, const ccord_store *s, bool reduced, ccord_buf *out);
