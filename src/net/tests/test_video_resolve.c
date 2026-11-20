// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdio.h>
#include <string.h>

#include <ccord/net/video_resolve.h>

#include "ccord_test.h"

static void test_url_safety(void)
{
  CT_CHECK(ccord_video_resolve_url_safe("https://www.youtube.com/watch?v=dQw4w9WgXcQ", 2049u));
  CT_CHECK(ccord_video_resolve_url_safe("https://x.com/user/status/1234567890", 2049u));
  CT_CHECK(ccord_video_resolve_url_safe("https://rr1---sn-abc.googlevideo.com:443/videoplayback?expire=1&sig=abc", 4096u));

  // Long signed stream URL (>2048 bytes, <4096 bytes) must pass the 4096-byte stream cap.
  char long_url[3200];
  int n = snprintf(long_url, sizeof long_url, "https://rr1---sn-test.googlevideo.com/videoplayback?id=");
  for (; n < 2800; n++) long_url[n] = (char)('a' + (n % 26));
  long_url[n] = '\0';
  CT_CHECK(ccord_video_resolve_url_safe(long_url, CCORD_VIDEO_RESOLVE_STREAM_URL_MAX));
  CT_CHECK(!ccord_video_resolve_url_safe(long_url, CCORD_VIDEO_RESOLVE_PAGE_URL_MAX));

  const char *bad[] = {
    "",
    "http://www.youtube.com/watch?v=dQw4w9WgXcQ",
    "file:///etc/passwd",
    "https://127.0.0.1/video.mp4",
    "https://203.0.113.5/video.mp4",
    "https://[::1]/video.mp4",
    "https://localhost/video.mp4",
    "https://box.local/video.mp4",
    "https://user:pass@example.com/video.mp4",
    "https://example.com:8443/video.mp4",
    "https://example.com/video.mp4\r\nX-Inject: 1",
    "https://example.com/with space.mp4",
    "https://example.com/",
    "https://nodot/video.mp4",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CT_CHECK(!ccord_video_resolve_url_safe(bad[i], CCORD_VIDEO_RESOLVE_STREAM_URL_MAX));
  }
  CT_CHECK(!ccord_video_resolve_url_safe(nullptr, CCORD_VIDEO_RESOLVE_STREAM_URL_MAX));
}

static void test_twitter_url_detection(void)
{
  char id[32];
  CT_CHECK(ccord_video_url_is_twitter("https://x.com/NASA/status/1845873219705946509", id, sizeof id));
  CT_CHECK(strcmp(id, "1845873219705946509") == 0);

  CT_CHECK(ccord_video_url_is_twitter("https://twitter.com/user/status/9876543210/video/1?s=20", id, sizeof id));
  CT_CHECK(strcmp(id, "9876543210") == 0);

  CT_CHECK(ccord_video_url_is_twitter("https://mobile.x.com/i/web/status/42424242", id, sizeof id));
  CT_CHECK(strcmp(id, "42424242") == 0);

  CT_CHECK(ccord_video_url_is_twitter("https://twitter.com/i/videos/tweet/555111222", id, sizeof id));
  CT_CHECK(strcmp(id, "555111222") == 0);

  CT_CHECK(ccord_video_url_is_twitter("https://fxtwitter.com/someone/status/123456789", id, sizeof id));
  CT_CHECK(strcmp(id, "123456789") == 0);

  CT_CHECK(ccord_video_url_is_twitter("https://fixupx.com/someone/status/777888999", id, sizeof id));
  CT_CHECK(strcmp(id, "777888999") == 0);

  CT_CHECK(!ccord_video_url_is_twitter("https://x.com/NASA", id, sizeof id));
  CT_CHECK(!ccord_video_url_is_twitter("https://x.com/NASA/status/notanumber", id, sizeof id));
  CT_CHECK(!ccord_video_url_is_twitter("https://evilx.com/NASA/status/12345", id, sizeof id));
  CT_CHECK(!ccord_video_url_is_twitter("http://x.com/NASA/status/12345", id, sizeof id));
}

static void test_youtube_url_detection(void)
{
  char id[32];
  CT_CHECK(ccord_video_url_is_youtube("https://www.youtube.com/watch?v=jNQXAC9IVRw", id, sizeof id));
  CT_CHECK(strcmp(id, "jNQXAC9IVRw") == 0);

  CT_CHECK(ccord_video_url_is_youtube("https://music.youtube.com/watch?list=PL123&v=aqz-KE-bpKQ&t=12", id, sizeof id));
  CT_CHECK(strcmp(id, "aqz-KE-bpKQ") == 0);

  CT_CHECK(ccord_video_url_is_youtube("https://youtu.be/dQw4w9WgXcQ?si=abcdef", id, sizeof id));
  CT_CHECK(strcmp(id, "dQw4w9WgXcQ") == 0);

  CT_CHECK(ccord_video_url_is_youtube("https://www.youtube.com/embed/dQw4w9WgXcQ?autoplay=1", id, sizeof id));
  CT_CHECK(strcmp(id, "dQw4w9WgXcQ") == 0);

  CT_CHECK(ccord_video_url_is_youtube("https://www.youtube.com/shorts/AbCdEfGhI_1", id, sizeof id));
  CT_CHECK(strcmp(id, "AbCdEfGhI_1") == 0);

  CT_CHECK(ccord_video_url_is_youtube("https://www.youtube-nocookie.com/embed/1234567890A", id, sizeof id));
  CT_CHECK(strcmp(id, "1234567890A") == 0);

  CT_CHECK(!ccord_video_url_is_youtube("https://www.youtube.com/watch?list=PL123", id, sizeof id));
  CT_CHECK(!ccord_video_url_is_youtube("https://notyoutube.com/watch?v=jNQXAC9IVRw", id, sizeof id));
  CT_CHECK(!ccord_video_url_is_youtube("https://youtu.be/bad!id", id, sizeof id));
}

static void test_syndication_token_and_parser(void)
{
  char tok[32];
  CT_CHECK(ccord_video_twitter_syndication_token("1845873219705946509", tok, sizeof tok));
  CT_CHECK(strlen(tok) >= 4u);
  CT_CHECK(strchr(tok, '0') == nullptr && strchr(tok, '.') == nullptr);
  CT_CHECK(!ccord_video_twitter_syndication_token("abc", tok, sizeof tok));

  const char *syn_json =
      "{\"mediaDetails\":[{\"type\":\"video\",\"video_info\":{\"duration_millis\":15200,\"variants\":["
      "{\"content_type\":\"application/x-mpegURL\",\"url\":\"https://video.twimg.com/ext_tw_video/1/pl.m3u8\"},"
      "{\"bitrate\":832000,\"content_type\":\"video/mp4\",\"url\":\"https://video.twimg.com/ext_tw_video/1/480.mp4\"},"
      "{\"bitrate\":2176000,\"content_type\":\"video/mp4\",\"url\":\"https://video.twimg.com/ext_tw_video/1/720.mp4?tag=12\"},"
      "{\"bitrate\":10368000,\"content_type\":\"video/mp4\",\"url\":\"https://video.twimg.com/ext_tw_video/1/1080.mp4\"},"
      "{\"bitrate\":2500000,\"content_type\":\"video/mp4\",\"url\":\"http://video.twimg.com/insecure.mp4\"}"
      "]}}]}";
  ccord_video_resolved out = {0};
  CT_CHECK(ccord_video_parse_twitter_syndication(syn_json, strlen(syn_json), &out));
  CT_CHECK(strcmp(out.video_url, "https://video.twimg.com/ext_tw_video/1/720.mp4?tag=12") == 0);
  CT_CHECK(out.audio_url[0] == '\0');
  CT_CHECK(out.duration_ms == 15200u);

  // Quoted-tweet video fallback + `video.variants` shape.
  const char *quoted_json =
      "{\"mediaDetails\":[],\"quoted_tweet\":{\"video\":{\"durationMs\":7500,\"variants\":["
      "{\"type\":\"video/mp4\",\"src\":\"https://video.twimg.com/tweet_video/quoted.mp4\"}"
      "]}}}";
  memset(&out, 0, sizeof out);
  CT_CHECK(ccord_video_parse_twitter_syndication(quoted_json, strlen(quoted_json), &out));
  CT_CHECK(strcmp(out.video_url, "https://video.twimg.com/tweet_video/quoted.mp4") == 0);
  CT_CHECK(out.duration_ms == 7500u);

  // Reject tweet with only HLS / unsafe URLs.
  const char *no_mp4 =
      "{\"mediaDetails\":[{\"video_info\":{\"variants\":["
      "{\"content_type\":\"application/x-mpegURL\",\"url\":\"https://video.twimg.com/pl.m3u8\"},"
      "{\"content_type\":\"video/mp4\",\"url\":\"https://127.0.0.1/bad.mp4\"}"
      "]}}]}";
  memset(&out, 0, sizeof out);
  CT_CHECK(!ccord_video_parse_twitter_syndication(no_mp4, strlen(no_mp4), &out));
}

static void test_fxtwitter_parser(void)
{
  const char *fx_json =
      "{\"code\":200,\"tweet\":{\"media\":{\"videos\":[{\"duration\":24.5,"
      "\"url\":\"https://video.twimg.com/fallback.mp4\","
      "\"formats\":["
      "{\"container\":\"m3u8\",\"url\":\"https://video.twimg.com/a.m3u8\"},"
      "{\"container\":\"mp4\",\"bitrate\":950000,\"url\":\"https://video.twimg.com/med.mp4\"},"
      "{\"container\":\"mp4\",\"bitrate\":2176000,\"url\":\"https://video.twimg.com/hd.mp4\"}"
      "]}]}}}";
  ccord_video_resolved out = {0};
  CT_CHECK(ccord_video_parse_fxtwitter(fx_json, strlen(fx_json), &out));
  CT_CHECK(strcmp(out.video_url, "https://video.twimg.com/hd.mp4") == 0);
  CT_CHECK(out.duration_ms == 24500u);

  // Quote fallback without formats array.
  const char *fx_quote =
      "{\"code\":200,\"tweet\":{\"quote\":{\"media\":{\"videos\":[{\"duration\":5,"
      "\"url\":\"https://video.twimg.com/q.mp4\"}]}}}}";
  memset(&out, 0, sizeof out);
  CT_CHECK(ccord_video_parse_fxtwitter(fx_quote, strlen(fx_quote), &out));
  CT_CHECK(strcmp(out.video_url, "https://video.twimg.com/q.mp4") == 0);
  CT_CHECK(out.duration_ms == 5000u);
}

static void test_ytdlp_output_parser(void)
{
  // Dual-stream DASH output (duration + video URL + audio URL).
  const char *dash =
      "635.25\n"
      "https://rr1---sn-demo.googlevideo.com/videoplayback?itag=136&sig=aaa\n"
      "https://rr1---sn-demo.googlevideo.com/videoplayback?itag=140&sig=bbb\n";
  ccord_video_resolved out = {0};
  CT_CHECK(ccord_video_parse_ytdlp_output(dash, strlen(dash), &out));
  CT_CHECK(out.duration_ms == 635250u);
  CT_CHECK(strcmp(out.video_url, "https://rr1---sn-demo.googlevideo.com/videoplayback?itag=136&sig=aaa") == 0);
  CT_CHECK(strcmp(out.audio_url, "https://rr1---sn-demo.googlevideo.com/videoplayback?itag=140&sig=bbb") == 0);

  // Single muxed stream with NA duration and CRLF endings.
  const char *single = "NA\r\nhttps://video.twimg.com/ext_tw_video/42/720.mp4\r\n";
  memset(&out, 0, sizeof out);
  CT_CHECK(ccord_video_parse_ytdlp_output(single, strlen(single), &out));
  CT_CHECK(out.duration_ms == 0u);
  CT_CHECK(strcmp(out.video_url, "https://video.twimg.com/ext_tw_video/42/720.mp4") == 0);
  CT_CHECK(out.audio_url[0] == '\0');

  // Disallowed URLs (http, file, localhost, IP literal) must be rejected.
  const char *bad =
      "12\n"
      "http://rr1.googlevideo.com/videoplayback?itag=18\n"
      "file:///tmp/x.mp4\n"
      "https://127.0.0.1/video.mp4\n"
      "https://localhost/video.mp4\n";
  memset(&out, 0, sizeof out);
  CT_CHECK(!ccord_video_parse_ytdlp_output(bad, strlen(bad), &out));
}

static void test_resolver_lifecycle_and_gate(void)
{
  ccord_video_resolver *r = ccord_video_resolver_create();
  if (!r) return;   // offline build
  ccord_error err;
  CT_CHECK(ccord_video_resolver_submit(r, 1u, "http://www.youtube.com/watch?v=jNQXAC9IVRw", &err) ==
           CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_video_resolver_submit(r, 2u, "https://127.0.0.1/watch?v=1", &err) == CCORD_ERR_PERMISSION);
  ccord_video_resolved out;
  CT_CHECK(!ccord_video_resolver_poll(r, &out));
  ccord_video_resolver_destroy(r);
}

int main(void)
{
  test_url_safety();
  test_twitter_url_detection();
  test_youtube_url_detection();
  test_syndication_token_and_parser();
  test_fxtwitter_parser();
  test_ytdlp_output_parser();
  test_resolver_lifecycle_and_gate();
  return CT_RESULT("video_resolve");
}
