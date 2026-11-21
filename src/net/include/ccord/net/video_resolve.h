// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Third-party embed video resolver (YouTube, X/Twitter, Vimeo, Reddit, Twitch, etc.) -> direct HTTPS stream URL(s).
// Runs asynchronously on a single lazily-started worker thread so clicking Play never blocks the UI thread:
//   - X/Twitter: resolves natively via Twitter's public syndication endpoint (zero external tools), falling back to
//     yt-dlp (if installed) and api.fxtwitter.com.
//   - YouTube and other video pages: resolves via yt-dlp (direct exec without a shell, strict flags, hard timeout,
//     cancellable), returning either a single muxed HTTPS MP4/WebM URL or a DASH (video + audio) URL pair.
// Every input and output URL is validated (https://, public DNS host, port 443, no IP literals or private hosts).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

#define CCORD_VIDEO_RESOLVE_PAGE_URL_MAX 2049u
#define CCORD_VIDEO_RESOLVE_STREAM_URL_MAX 4096u
#define CCORD_VIDEO_RESOLVE_ERR_MAX 192u

typedef struct ccord_video_resolved {
  uint64_t req_id;
  ccord_status status;
  bool missing_helper;                                   // true when external yt-dlp helper is not installed
  uint32_t duration_ms;                                  // 0 when unknown
  char page_url[CCORD_VIDEO_RESOLVE_PAGE_URL_MAX];       // canonical URL requested
  char video_url[CCORD_VIDEO_RESOLVE_STREAM_URL_MAX];    // direct HTTPS video URL (muxed, or video-only when audio_url is set)
  char audio_url[CCORD_VIDEO_RESOLVE_STREAM_URL_MAX];    // optional direct HTTPS audio URL ("" when video_url is muxed)
  char error[CCORD_VIDEO_RESOLVE_ERR_MAX];               // host-safe error summary (no query strings / tokens)
} ccord_video_resolved;

typedef struct ccord_video_resolver ccord_video_resolver;

// ---- Pure URL classification & response parsers (deterministic, offline-testable) ----

// True when `url` is a clean https:// URL with a public DNS host (no IP literal, localhost, userinfo, non-443 port,
// whitespace or control bytes) within `max_len` bytes.
bool ccord_video_resolve_url_safe(const char *url, size_t max_len);

// True when `url` is an X/Twitter (or fxtwitter/vxtwitter/fixupx/xcancel) status/video URL; extracts numeric tweet id.
bool ccord_video_url_is_twitter(const char *url, char *tweet_id_out, size_t id_cap);

// True when `url` is a YouTube watch/embed/shorts/live/youtu.be URL; extracts the video id.
bool ccord_video_url_is_youtube(const char *url, char *video_id_out, size_t id_cap);

// Computes Twitter's public syndication query token for a numeric tweet id.
bool ccord_video_twitter_syndication_token(const char *tweet_id, char *out, size_t cap);

// Parses cdn.syndication.twimg.com/tweet-result JSON into `out` (video_url + optional duration_ms).
bool ccord_video_parse_twitter_syndication(const char *json, size_t len, ccord_video_resolved *out);

// Parses api.fxtwitter.com JSON into `out` (video_url + optional duration_ms).
bool ccord_video_parse_fxtwitter(const char *json, size_t len, ccord_video_resolved *out);

// Parses stdout from `yt-dlp --print "%(duration)s" --print "%(urls)s"` into `out` (video_url + optional audio_url).
bool ccord_video_parse_ytdlp_output(const char *text, size_t len, ccord_video_resolved *out);

// ---- Async resolver lifecycle (live builds; offline builds return CCORD_ERR_UNSUPPORTED) ----

[[nodiscard]] ccord_video_resolver *ccord_video_resolver_create(void);
void ccord_video_resolver_destroy(ccord_video_resolver *r);

// Queues an async resolution for `url` (cancels any superseded in-flight resolve). Result arrives via poll().
[[nodiscard]] ccord_status ccord_video_resolver_submit(ccord_video_resolver *r, uint64_t req_id, const char *url,
                                                       ccord_error *err);

// Pops the next completed resolution (non-blocking). Returns false when none is ready.
bool ccord_video_resolver_poll(ccord_video_resolver *r, ccord_video_resolved *out);
// True while a resolution job is queued, running, or waiting to be popped via poll().
bool ccord_video_resolver_busy(const ccord_video_resolver *r);
