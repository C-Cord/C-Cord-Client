// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Chunked (HTTP Range) playback source for a clicked chat video: the session fetches byte ranges through the
// transport's fetch_media_range and serves them to a ccord_av_source reader.
// The UI owns the stream: it opens it, hands the three byte-source callbacks to ccord_av_stream_open_source, and
// closes it only after the render stream is closed.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/session/session.h>

typedef struct ccord_media_stream ccord_media_stream;

// Constants. CCORD_STREAM_RANGE_MAX_BYTES (1 MiB) lives in net/transport.h.
#define CCORD_STREAM_BLOCK_BYTES        (256u * 1024u)
#define CCORD_STREAM_RANGE_BLOCKS       4u                                   // one request = up to 4 blocks = 1 MiB
#define CCORD_STREAM_CACHE_BLOCKS       64u
#define CCORD_STREAM_RAM_MAX            (CCORD_STREAM_CACHE_BLOCKS * CCORD_STREAM_BLOCK_BYTES)   // 16 MiB
#define CCORD_STREAM_READAHEAD_BLOCKS   8u                                   // fetch up to 8 blocks past the read block
#define CCORD_STREAM_MAX_ATTEMPTS       3u                                   // per range (backoff 250 ms, 1 s)
#define CCORD_STREAM_READ_TIMEOUT_MS    30000u                               // a blocked read_at fails with -CCORD_ERR_TIMEOUT

typedef struct ccord_media_stream_stats {
  int64_t  total;                 // -1 unknown
  bool     ranged;                // a 206 was seen
  bool     no_range;              // the server answered 200 to the probe
  ccord_status error;             // CCORD_OK, or the failure that ended the stream
  char     error_text[128];       // no URL, no query
  uint64_t bytes_fetched;         // sum of accepted bodies
  uint32_t requests, retries, evictions;
  uint32_t resident_blocks, peak_resident_blocks;   // <= CCORD_STREAM_CACHE_BLOCKS
  uint8_t  last_redirects;
  uint32_t log_count;             // ring of the last 16 completed ranges, oldest first
  int64_t  log_start[16];
  uint32_t log_len[16];
} ccord_media_stream_stats;

// UI thread. Starts a ranged stream for url. Fails without side effects (CCORD_ERR_UNSUPPORTED) when the transport has
// no fetch_media_range, or on out of memory. The session cannot check the URL gate itself (ccord_live_user_video_url_allowed
// lives in the live-only discord_rest.c, which ccord_session does not link in offline builds): the transport's
// fetch_media_range refuses a blocked URL with CCORD_ERR_PERMISSION when the next ccord_session_pump issues the first
// range (bytes 0..1 MiB-1); the stream then latches that error (stats.error == CCORD_ERR_PERMISSION).
[[nodiscard]] ccord_media_stream *ccord_session_open_media_stream(ccord_session *s, const char *url, ccord_error *err);
// UI thread. Cancels (wakes a blocked read_at) and frees. Safe on nullptr. ccord_session_destroy closes what is left.
// Must be called after ccord_av_stream_close for the stream that reads from it.
void ccord_session_close_media_stream(ccord_session *s, ccord_media_stream *ms);

// Byte-source callbacks handed to ccord_av_stream_open_source by the UI (the UI links both session and render; the
// session does not depend on render, session/CMakeLists.txt:4):
int     ccord_media_stream_read_at(void *ms, int64_t off, uint8_t *buf, int n);   // demux thread; blocks (100 ms slices)
int64_t ccord_media_stream_size(void *ms);                                        // any thread; -1 unknown
void    ccord_media_stream_cancel(void *ms);                                      // any thread; idempotent
// UI thread:
void ccord_media_stream_stats_get(const ccord_media_stream *ms, ccord_media_stream_stats *out);
