// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "fake_discord.h"  // first: defines _POSIX_C_SOURCE / _DEFAULT_SOURCE

#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/model/rich.h>
#include <ccord/net/admin.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/rtp_h264.h>
#include <ccord/net/voice_camera.h>
#include <ccord/net/voice_crypto.h>

#include <sodium.h>

#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ccord_test.h"

static bool make_temp(char *out, size_t cap)
{
  const char *base = getenv("TMPDIR");
  if (!base || base[0] == '\0') base = ".";
  int n = snprintf(out, cap, "%s/ccord-live-XXXXXX", base);
  if (n < 0 || (size_t)n >= cap) return false;
  return mkdtemp(out) != nullptr;
}

static void test_token_env(void)
{
  ccord_error err;
  char out[64];
  (void)setenv(CCORD_LIVE_TOKEN_ENV, "REDACTED_TEST_TOKEN", 1);
  CT_CHECK(ccord_live_load_token("/nonexistent/ccord", out, sizeof out, &err) == CCORD_OK);
  CT_CHECK(strcmp(out, "REDACTED_TEST_TOKEN") == 0);
  ccord_live_wipe(out, sizeof out);
  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);
}

static void test_token_file(void)
{
  char dir[512];
  if (!make_temp(dir, sizeof dir)) {
    fprintf(stderr, "test_live_transport: no TMPDIR, skipping token file test\n");
    return;
  }
  const char *p1 = "AAAAAAAAAAAAAAAAAAAAAAAA";
  const char *p2 = "BBBBBB";
  const char *p3 = "CCCCCCCCCCCCCCCCCCCCCCCCCCCC";
  char token[160];
  int n = snprintf(token, sizeof token, "%s.%s.%s", p1, p2, p3);
  CT_CHECK(n > 0 && (size_t)n < sizeof token);

  char path[640];
  n = snprintf(path, sizeof path, "%s/token", dir);
  CT_CHECK(n > 0 && (size_t)n < sizeof path);
  FILE *f = fopen(path, "wb");
  CT_CHECK(f != nullptr);
  if (f) {
    CT_CHECK(fwrite(token, 1, strlen(token), f) == strlen(token));
    fclose(f);
  }

  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);
  ccord_error err;
  char out[CCORD_LIVE_TOKEN_MAX + 1];
  CT_CHECK(ccord_live_load_token(path, out, sizeof out, &err) == CCORD_OK);
  CT_CHECK(strcmp(out, token) == 0);
  ccord_live_wipe(out, sizeof out);

  CT_CHECK(unlink(path) == 0);
  CT_CHECK(ccord_live_load_token(path, out, sizeof out, &err) == CCORD_ERR_NOT_FOUND);
  ccord_live_wipe(out, sizeof out);
  CT_CHECK(rmdir(dir) == 0);
  ccord_live_wipe(token, sizeof token);
}

static void test_payment_routes_blocked(void)
{
  const char *bad[] = {"/store/skus", "/billing/subscription", "/payments/checkout", "/subscriptions/1",
                       "/guilds/7/store", "/users/@me/gifts"};
  ccord_rest_config cfg = {
    .base_url = "https://discord.com/api/v9",
    .token = "REDACTED_TEST_TOKEN",
    .timeout_ms = 1000,
  };
  ccord_buf resp;
  ccord_buf_init(&resp, 4096);
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CT_CHECK(!ccord_live_rest_path_allowed(bad[i]));
    ccord_error err;
    long status = -1;
    ccord_status st = ccord_live_rest_request(&cfg, CCORD_REST_GET, bad[i], nullptr, &resp, &status, &err);
    CT_CHECK(st == CCORD_ERR_PERMISSION);
    CT_CHECK(err.code == CCORD_ERR_PERMISSION);
    CT_CHECK(status == -1);
  }
  const char *good[] = {"/channels/123/messages", "/users/@me/channels", "/guilds/7/channels",
                        "/channels/1/messages/2/reactions/%F0%9F%91%8D/@me"};
  for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
    CT_CHECK(ccord_live_rest_path_allowed(good[i]));
  }
  CT_CHECK(!ccord_live_rest_path_allowed("channels/1/messages"));

  // CDN media URL allowlist tests
  CT_CHECK(ccord_live_media_url_allowed("https://cdn.discordapp.com/avatars/1001/a_hash.png?size=64"));
  CT_CHECK(ccord_live_media_url_allowed("https://media.discordapp.net/attachments/10/20/pic.png"));
  CT_CHECK(ccord_live_media_url_allowed("https://media.discordapp.com/icons/77/icon.png"));
  CT_CHECK(ccord_live_media_url_allowed("https://media.tenor.com/abc123XYZ/tenor.gif"));
  CT_CHECK(ccord_live_media_url_allowed("https://c.tenor.com/abc123XYZ/tenor.gif"));
  // Embed media goes through Discord's external-image proxy; the GIF picker's previews come from its provider.
  CT_CHECK(ccord_live_media_url_allowed("https://images-ext-1.discordapp.net/external/abc/https/example.com/a.jpg"));
  CT_CHECK(ccord_live_media_url_allowed("https://images-ext-2.discordapp.net/external/abc/https/example.com/a.mp4"));
  CT_CHECK(ccord_live_media_url_allowed("https://static.klipy.com/ii/abc/de/f1/cat.webp"));
  CT_CHECK(ccord_live_media_url_allowed("https://media1.tenor.com/m/abc123XYZ/cat.gif"));
  // Third-party GIF providers (Giphy, Imgur, legacy tenor.co): exact hosts only.
  static const char *const gif_hosts[] = {"media.giphy.com", "media0.giphy.com", "media1.giphy.com",
                                          "media2.giphy.com", "media3.giphy.com", "media4.giphy.com",
                                          "i.giphy.com", "i.imgur.com", "media.tenor.co"};
  for (size_t i = 0; i < sizeof gif_hosts / sizeof gif_hosts[0]; i++) {
    char u[128];
    (void)snprintf(u, sizeof u, "https://%s/media/abc/cat.mp4", gif_hosts[i]);
    CT_CHECK(ccord_live_media_url_allowed(u));
  }
  CT_CHECK(ccord_live_media_url_allowed("HTTPS://I.GIPHY.COM:443/media/abc/cat.gif"));
  CT_CHECK(!ccord_live_media_url_allowed("https://media.giphy.com.evil.com/media/abc/cat.gif"));
  CT_CHECK(!ccord_live_media_url_allowed("https://evilgiphy.com/media/abc/cat.gif"));
  CT_CHECK(!ccord_live_media_url_allowed("https://giphy.com/media/abc/cat.gif"));
  CT_CHECK(!ccord_live_media_url_allowed("https://i.imgur.com.evil.com/abc.mp4"));
  CT_CHECK(!ccord_live_media_url_allowed("https://media5.giphy.com/media/abc/cat.gif"));
  CT_CHECK(!ccord_live_media_url_allowed("https://imgur.com/abc.mp4"));
  CT_CHECK(!ccord_live_media_url_allowed("http://i.giphy.com/media/abc/cat.gif"));
  CT_CHECK(!ccord_live_media_url_allowed("https://images-ext-1.discordapp.net.evil.com/external/a.jpg"));
  CT_CHECK(!ccord_live_media_url_allowed("https://static.klipy.com.evil.com/cat.webp"));
  CT_CHECK(!ccord_live_media_url_allowed("http://cdn.discordapp.com/avatars/1001/a_hash.png"));
  CT_CHECK(!ccord_live_media_url_allowed("https://evil.example.com/avatars/1001/a_hash.png"));
  CT_CHECK(!ccord_live_media_url_allowed("https://cdn.discordapp.com.evil.com/avatars/1001/a_hash.png"));
  CT_CHECK(!ccord_live_media_url_allowed("https://cdn.discordapp.com/billing/invoice.png"));
  CT_CHECK(!ccord_live_media_url_allowed("/channels/123/messages"));
  ccord_buf_free(&resp);
}

// The gate for a video the user clicked: any https DNS host, nothing local, nothing with a port or userinfo.
static void test_user_video_url_allowed(void)
{
  // Every URL the strict table accepts is accepted here too.
  const char *strict_ok[] = {
    "https://cdn.discordapp.com/avatars/1001/a_hash.png?size=64",
    "https://media.discordapp.net/attachments/10/20/pic.png",
    "https://media.discordapp.com/icons/77/icon.png",
    "https://images-ext-1.discordapp.net/external/abc/https/example.com/a.jpg",
    "https://images-ext-2.discordapp.net/external/abc/https/example.com/a.mp4",
    "https://static.klipy.com/ii/abc/de/f1/cat.webp",
    "https://media.tenor.com/abc123XYZ/tenor.gif",
    "https://media1.tenor.com/m/abc123XYZ/cat.gif",
    "https://c.tenor.com/abc123XYZ/tenor.gif",
  };
  for (size_t i = 0; i < sizeof strict_ok / sizeof strict_ok[0]; i++) {
    CT_CHECK(ccord_live_media_url_allowed(strict_ok[i]));
    CT_CHECK(ccord_live_user_video_url_allowed(strict_ok[i]));
  }
  const char *user_ok[] = {
    "https://videos.example.org/a.mp4",
    "https://videos.example.org/clip/a.webm?token=1&sig=2",
    "HTTPS://Videos.Example.Org/a.mov",
    "https://videos.example.org:443/a.mp4",
    "https://cdn-1.example.co.uk/path/to/a.mp4#t=3",
    "https://localhost.example.com/a.mp4",
  };
  for (size_t i = 0; i < sizeof user_ok / sizeof user_ok[0]; i++) {
    CT_CHECK(ccord_live_user_video_url_allowed(user_ok[i]));
    if (i == 0) CT_CHECK(!ccord_live_media_url_allowed(user_ok[i]));   // the automatic gate stays strict
  }
  const char *refused[] = {
    "http://videos.example.org/a.mp4",
    "ftp://videos.example.org/a.mp4",
    "//videos.example.org/a.mp4",
    "videos.example.org/a.mp4",
    "https://a@videos.example.org/a.mp4",
    "https://user:pw@videos.example.org/a.mp4",
    "https://203.0.113.5/a.mp4",
    "https://127.0.0.1/a.mp4",
    "https://2130706433/a.mp4",
    "https://127.1/a.mp4",
    "https://[::1]/a.mp4",
    "https://[::1]:443/a.mp4",
    "https://localhost/a.mp4",
    "https://LOCALHOST/a.mp4",
    "https://foo.localhost/a.mp4",
    "https://x.local/a.mp4",
    "https://nodot/a.mp4",
    "https://videos.example.org:8443/a.mp4",
    "https://videos.example.org:/a.mp4",
    "https://videos.example.org:4430/a.mp4",
    "https://videos.example.org",
    "https://videos.example.org/",
    "https://videos.example.org/?a.mp4",
    "https://videos.example.org?x/a.mp4",
    "https://videos..example.org/a.mp4",
    "https://.example.org/a.mp4",
    "https://videos.example.org%2f@evil.com/a.mp4",
    "https://videos.example.org/billing/a.mp4",
    "",
  };
  for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
    CT_CHECK(!ccord_live_user_video_url_allowed(refused[i]));
  }
  CT_CHECK(!ccord_live_user_video_url_allowed(nullptr));
}

static void test_rest_url_and_headers(void)
{
  ccord_error err;
  ccord_buf url;
  ccord_buf_init(&url, 512);
  CT_CHECK(ccord_live_rest_build_url("https://discord.com/api/v9/", "/channels/1/messages?limit=50", &url, &err) ==
           CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&url), "https://discord.com/api/v9/channels/1/messages?limit=50") == 0);
  ccord_buf_clear(&url);
  CT_CHECK(ccord_live_rest_build_url(nullptr, "/users/@me/channels", &url, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&url), "https://discord.com/api/v9/users/@me/channels") == 0);
  ccord_buf_free(&url);

  const char *super = ccord_live_super_properties();
  CT_CHECK(super != nullptr);
  CT_CHECK(strlen(super) > 32);
  CT_CHECK(super[0] == 'e' && super[1] == 'y'); // base64 of '{'
  CT_CHECK(strstr(super, "REDACTED") == nullptr);
}

static void test_rest_authorization_header(void)
{
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return;
  int opt = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = 0;
  if (bind(lfd, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(lfd, 1) != 0) {
    close(lfd);
    return;
  }
  socklen_t slen = sizeof sin;
  if (getsockname(lfd, (struct sockaddr *)&sin, &slen) != 0) {
    close(lfd);
    return;
  }
  int port = ntohs(sin.sin_port);

  pid_t pid = fork();
  if (pid == 0) {
    int cfd = accept(lfd, nullptr, nullptr);
    close(lfd);
    if (cfd < 0) _exit(1);
    char buf[4096] = {0};
    ssize_t n = read(cfd, buf, sizeof buf - 1);
    int ok = 0;
    if (n > 0) {
      bool has_raw_auth = (strstr(buf, "Authorization: REDACTED_TEST_TOKEN\r\n") != nullptr);
      bool has_no_bearer = (strstr(buf, "Bearer") == nullptr);
      char ua_hdr[512];
      snprintf(ua_hdr, sizeof ua_hdr, "User-Agent: %s\r\n", ccord_live_user_agent());
      bool has_ua = (strstr(buf, ua_hdr) != nullptr);
      bool has_json = (strstr(buf, "Content-Type: application/json\r\n") != nullptr);
      bool has_super = (strstr(buf, "X-Super-Properties: ") != nullptr);
      if (has_raw_auth && has_no_bearer && has_ua && has_json && has_super) {
        ok = 1;
      }
    }
    const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
    (void)write(cfd, resp, sizeof resp - 1);
    close(cfd);
    _exit(ok ? 0 : 2);
  }

  char base_url[64];
  (void)snprintf(base_url, sizeof base_url, "http://127.0.0.1:%d", port);
  ccord_rest_config cfg = {
    .base_url = base_url,
    .token = "REDACTED_TEST_TOKEN",
    .timeout_ms = 3000,
  };
  ccord_buf resp;
  ccord_buf_init(&resp, 256);
  long status = 0;
  ccord_error err;
  ccord_status st = ccord_live_rest_request(&cfg, CCORD_REST_GET, "/users/@me/channels", nullptr, &resp, &status, &err);
  close(lfd);
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(status == 200);
  ccord_buf_free(&resp);

  int wstatus = 0;
  pid_t waited = waitpid(pid, &wstatus, 0);
  CT_CHECK(waited == pid);
  CT_CHECK(WIFEXITED(wstatus));
  CT_CHECK(WEXITSTATUS(wstatus) == 0);
}

static void test_gateway_ready_large_nodes(void)
{
  // LIVE_JSON_MAX_NODES >= 2,500,000 (was 200,000).
  // A payload with > 200,000 nodes previously failed node limit checks.
  ccord_buf buf;
  ccord_buf_init(&buf, 0);
  bool ok = (ccord_buf_append_str(&buf, "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"guilds\":[],\"private_channels\":[],\"extra\":[") == CCORD_OK);
  for (size_t i = 0; i < 205000 && ok; i++) {
    if (ccord_buf_append_str(&buf, i > 0 ? ",1" : "1") != CCORD_OK) ok = false;
  }
  if (ok && ccord_buf_append_str(&buf, "]}}") != CCORD_OK) ok = false;
  CT_CHECK(ok);
  ccord_live_event ev;
  ccord_error err;
  ccord_status st = ccord_live_gateway_decode(ccord_buf_cstr(&buf), buf.len, &ev, &err);
  CT_CHECK(st == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.self_id == 42);
  ccord_live_event_free(&ev);
  ccord_buf_free(&buf);
}

static void test_gateway_message(void)
{
  const char *json =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"s\":7,\"d\":{\"id\":\"100000000000000001\","
    "\"channel_id\":\"100000000000000002\",\"author\":{\"id\":\"100000000000000003\"},"
    "\"content\":\"hello live\",\"timestamp\":\"2026-11-08T00:00:00.500000+00:00\","
    "\"reactions\":[{\"emoji\":{\"name\":\"\xf0\x9f\x91\x8d\"},\"count\":2,\"me\":true}]}}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.op == 0);
  CT_CHECK(ev.seq == 7);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE);
  CT_CHECK(ev.ev.message_count == 1);
  CT_CHECK(ev.messages[0].id == 100000000000000001ull);
  CT_CHECK(ev.messages[0].channel_id == 100000000000000002ull);
  CT_CHECK(ev.messages[0].author_id == 100000000000000003ull);
  CT_CHECK(strcmp(ev.messages[0].content, "hello live") == 0);
  CT_CHECK(ev.messages[0].timestamp_ms == 1794096000500ll);
  CT_CHECK(ev.messages[0].reaction_count == 2 && ev.messages[0].self_reacted);
  CT_CHECK(ev.messages[0].reaction_kind_count == 1);
  CT_CHECK(ev.messages[0].reactions && strcmp(ev.messages[0].reactions[0].emoji, "\xf0\x9f\x91\x8d") == 0);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);
}

static void test_gateway_controls_and_state(void)
{
  ccord_live_event ev;
  ccord_error err;

  const char *hello = "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}";
  CT_CHECK(ccord_live_gateway_decode(hello, strlen(hello), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.hello);
  CT_CHECK(ev.heartbeat_interval_ms == 45000);
  ccord_live_event_free(&ev);

  const char *typing =
    "{\"op\":0,\"t\":\"TYPING_START\",\"d\":{\"channel_id\":\"10\",\"user_id\":\"42\",\"timestamp\":1700000000}}";
  CT_CHECK(ccord_live_gateway_decode(typing, strlen(typing), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_TYPING_START);
  CT_CHECK(ev.ev.channel_id == 10);
  CT_CHECK(ev.ev.user_id == 42);
  CT_CHECK(ev.ev.timestamp_ms == 1700000000000ll + 10000);
  ccord_live_event_free(&ev);

  const char *presence =
    "{\"op\":0,\"t\":\"PRESENCE_UPDATE\",\"d\":{\"user\":{\"id\":\"42\"},\"status\":\"idle\","
    "\"activities\":[{\"type\":4,\"state\":\"building\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(presence, strlen(presence), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PRESENCE_UPDATE);
  CT_CHECK(ev.ev.user_id == 42);
  CT_CHECK(ev.ev.presence == CCORD_PRESENCE_IDLE);
  CT_CHECK(strcmp(ev.ev.status_text, "building") == 0);
  ccord_live_event_free(&ev);

  const char *upsert =
    "{\"op\":0,\"t\":\"CHANNEL_CREATE\",\"d\":{\"id\":\"55\",\"guild_id\":\"77\",\"type\":0,"
    "\"name\":\"general\",\"topic\":\"chat\"}}";
  CT_CHECK(ccord_live_gateway_decode(upsert, strlen(upsert), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_UPSERT);
  CT_CHECK(ev.ev.channel != nullptr);
  CT_CHECK(ev.ev.channel->id == 55);
  CT_CHECK(ev.ev.channel->guild_id == 77);
  CT_CHECK(strcmp(ev.ev.channel->name, "general") == 0);
  ccord_live_event_free(&ev);

  const char *del =
    "{\"op\":0,\"t\":\"MESSAGE_DELETE\",\"d\":{\"id\":\"9\",\"channel_id\":\"10\",\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(del, strlen(del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_DELETE);
  CT_CHECK(ev.ev.message_id == 9);
  CT_CHECK(ev.ev.channel_id == 10);
  ccord_live_event_free(&ev);

  const char *cdelete = "{\"op\":0,\"t\":\"CHANNEL_DELETE\",\"d\":{\"id\":\"10\",\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(cdelete, strlen(cdelete), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_DELETE);
  CT_CHECK(ev.ev.channel_id == 10);
  ccord_live_event_free(&ev);

  const char *react =
    "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{\"channel_id\":\"10\",\"message_id\":\"9\","
    "\"user_id\":\"42\",\"count\":2,\"emoji\":{\"name\":\"\xf0\x9f\x91\x8d\"}}}";
  CT_CHECK(ccord_live_gateway_decode(react, strlen(react), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_UPDATE);
  CT_CHECK(ev.ev.reaction_count == 2 && ev.ev.reaction_delta == 0);
  CT_CHECK(ev.ev.reaction_emoji != nullptr && strcmp(ev.ev.reaction_emoji, "\xf0\x9f\x91\x8d") == 0);
  ccord_live_event_free(&ev);

  const char *react_delta =
    "{\"op\":0,\"t\":\"MESSAGE_REACTION_REMOVE\",\"d\":{\"channel_id\":\"10\",\"message_id\":\"9\","
    "\"user_id\":\"42\",\"emoji\":{\"name\":\"\xf0\x9f\x91\x8d\"}}}";
  CT_CHECK(ccord_live_gateway_decode(react_delta, strlen(react_delta), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_UPDATE);
  CT_CHECK(ev.ev.reaction_delta == -1);
  ccord_live_event_free(&ev);

  const char *react_custom =
    "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{\"channel_id\":\"10\",\"message_id\":\"9\","
    "\"user_id\":\"42\",\"emoji\":{\"id\":\"555666\",\"name\":\"blob\",\"animated\":true}}}";
  CT_CHECK(ccord_live_gateway_decode(react_custom, strlen(react_custom), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_UPDATE && ev.ev.reaction_delta == 1);
  CT_CHECK(ev.ev.reaction_emoji && strcmp(ev.ev.reaction_emoji, "blob:555666") == 0);
  ccord_live_event_free(&ev);

  const char *react_rem_all =
    "{\"op\":0,\"t\":\"MESSAGE_REACTION_REMOVE_ALL\",\"d\":{\"channel_id\":\"10\",\"message_id\":\"9\"}}";
  CT_CHECK(ccord_live_gateway_decode(react_rem_all, strlen(react_rem_all), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_UPDATE && ev.ev.reaction_count == 0 && ev.ev.reaction_emoji == nullptr);
  ccord_live_event_free(&ev);

  const char *react_rem_emj =
    "{\"op\":0,\"t\":\"MESSAGE_REACTION_REMOVE_EMOJI\",\"d\":{\"channel_id\":\"10\",\"message_id\":\"9\","
    "\"emoji\":{\"id\":\"555666\",\"name\":\"blob\"}}}";
  CT_CHECK(ccord_live_gateway_decode(react_rem_emj, strlen(react_rem_emj), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_UPDATE && ev.ev.reaction_count == 0 && ev.ev.reaction_delta == 0);
  CT_CHECK(ev.ev.reaction_emoji && strcmp(ev.ev.reaction_emoji, "blob:555666") == 0);
  ccord_live_event_free(&ev);

  const char *bulk_del =
    "{\"op\":0,\"t\":\"MESSAGE_DELETE_BULK\",\"d\":{\"channel_id\":\"10\",\"guild_id\":\"77\","
    "\"ids\":[\"101\",\"102\",\"103\"]}}";
  CT_CHECK(ccord_live_gateway_decode(bulk_del, strlen(bulk_del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_DELETE_BULK);
  CT_CHECK(ev.ev.message_id_count == 3 && ev.ev.message_ids[0] == 101 && ev.ev.message_ids[2] == 103);
  ccord_live_event_free(&ev);

  const char *pins_upd =
    "{\"op\":0,\"t\":\"CHANNEL_PINS_UPDATE\",\"d\":{\"channel_id\":\"10\",\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(pins_upd, strlen(pins_upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_PINS_UPDATE && ev.ev.channel_id == 10);
  ccord_live_event_free(&ev);

  const char *msg_ack =
    "{\"op\":0,\"t\":\"MESSAGE_ACK\",\"d\":{\"channel_id\":\"10\",\"message_id\":\"99\",\"mention_count\":0}}";
  CT_CHECK(ccord_live_gateway_decode(msg_ack, strlen(msg_ack), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_ACK && ev.ev.channel_id == 10 && ev.ev.message_id == 99);
  ccord_live_event_free(&ev);

  const char *rel_add =
    "{\"op\":0,\"t\":\"RELATIONSHIP_ADD\",\"d\":{\"id\":\"43\",\"type\":1}}";
  CT_CHECK(ccord_live_gateway_decode(rel_add, strlen(rel_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_RELATIONSHIP_UPDATE && ev.ev.user_id == 43 &&
           ev.ev.relationship_kind == CCORD_RELATIONSHIP_FRIEND);
  ccord_live_event_free(&ev);

  const char *rel_rem =
    "{\"op\":0,\"t\":\"RELATIONSHIP_REMOVE\",\"d\":{\"id\":\"43\"}}";
  CT_CHECK(ccord_live_gateway_decode(rel_rem, strlen(rel_rem), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_RELATIONSHIP_UPDATE && ev.ev.user_id == 43 &&
           ev.ev.relationship_kind == CCORD_RELATIONSHIP_NONE);
  ccord_live_event_free(&ev);

  ccord_gateway *gw = ccord_gateway_create(nullptr, "test_token", &err);
  CT_CHECK(gw != nullptr);
  if (gw) {
    CT_CHECK(ccord_gateway_state(gw) == CCORD_CONN_DISCONNECTED);
    ccord_gateway_destroy(gw);
  }
}

static void test_gateway_ready_snapshot(void)
{
  const char *json =
    "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\",\"global_name\":\"Me\","
    "\"bio\":\"C23 dev\",\"pronouns\":\"they/them\"},"
    "\"guilds\":[{\"id\":\"77\",\"name\":\"HQ\",\"channels\":["
    "{\"id\":\"10\",\"type\":4,\"name\":\"TEXT\"},"
    "{\"id\":\"11\",\"type\":0,\"name\":\"general\",\"parent_id\":\"10\",\"topic\":\"chat\",\"last_message_id\":\"500\"}],"
    "\"roles\":[{\"id\":\"1\",\"name\":\"Admins\",\"color\":255,\"position\":0},"
    "{\"id\":\"2\",\"name\":\"Members\",\"color\":0,\"position\":5}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[\"1\"]},"
    "{\"user\":{\"id\":\"43\",\"username\":\"Ada\"},\"roles\":[\"2\"]}],"
    "\"presences\":[{\"user\":{\"id\":\"43\"},\"status\":\"idle\"}]}],"
    "\"read_state\":[{\"id\":\"11\",\"last_message_id\":\"490\",\"mention_count\":3}],"
    "\"relationships\":[{\"id\":\"43\",\"type\":1}],"
    "\"private_channels\":[{\"id\":\"90\",\"type\":1,\"recipients\":[{\"username\":\"Ada\"}]}]}}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.self_id == 42);
  CT_CHECK(ev.snapshot.guild_count == 1);
  CT_CHECK(ev.snapshot.channel_count == 3);
  CT_CHECK(ev.snapshot.user_count == 2);
  CT_CHECK(ev.snapshot.member_count == 2);
  CT_CHECK(ev.snapshot.relationship_count == 1);
  CT_CHECK(ev.snapshot.relationships[0].user_id == 43 &&
           ev.snapshot.relationships[0].kind == CCORD_RELATIONSHIP_FRIEND);
  CT_CHECK(ccord_snapshot_validate(&ev.snapshot, &err) == CCORD_OK);
  bool saw_dm = false, saw_text = false, saw_ada = false;
  for (size_t i = 0; i < ev.snapshot.member_count; i++) {
    const ccord_member *m = &ev.snapshot.members[i];
    if (m->user_id == 43) {
      saw_ada = true;
      CT_CHECK(strcmp(m->role_name, "Members") == 0);
    }
  }
  for (size_t i = 0; i < ev.snapshot.user_count; i++) {
    if (ev.snapshot.users[i].id == 42) {
      CT_CHECK(strcmp(ev.snapshot.users[i].bio, "C23 dev") == 0);
      CT_CHECK(strcmp(ev.snapshot.users[i].pronouns, "they/them") == 0);
    }
    if (ev.snapshot.users[i].id == 43) CT_CHECK(ev.snapshot.users[i].presence == CCORD_PRESENCE_IDLE);
  }
  for (size_t i = 0; i < ev.snapshot.channel_count; i++) {
    const ccord_channel *c = &ev.snapshot.channels[i];
    if (c->guild_id == CCORD_DM_GUILD_ID) {
      saw_dm = true;
      CT_CHECK(strcmp(c->name, "Ada") == 0);
    }
    if (c->id == 11) {
      saw_text = true;
      CT_CHECK(c->parent_id == 10);
      CT_CHECK(strcmp(c->topic, "chat") == 0);
      CT_CHECK(c->unread_count == 3 && c->mention_count == 3);
      CT_CHECK(c->last_read_id == 490);   // the read position is what the account has acknowledged
    } else {
      CT_CHECK(c->last_read_id == CCORD_ID_NONE);   // no read state, no position
    }
  }
  CT_CHECK(saw_dm);
  CT_CHECK(saw_text);
  CT_CHECK(saw_ada);
  ccord_live_event_free(&ev);
}

static void test_history_json(void)
{
  const char *json =
    "[{\"id\":\"2\",\"channel_id\":\"10\",\"author\":{\"id\":\"42\"},\"content\":\"b\","
    "\"timestamp\":\"2026-11-08T00:00:01Z\"},"
    "{\"id\":\"1\",\"channel_id\":\"10\",\"author\":{\"id\":\"42\"},\"content\":\"a\","
    "\"timestamp\":\"2026-11-08T00:00:00Z\"}]";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_json_history(json, strlen(json), 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_HISTORY);
  CT_CHECK(ev.ev.channel_id == 10);
  CT_CHECK(ev.message_count == 2);
  CT_CHECK(ev.messages[0].id == 2);
  CT_CHECK(strcmp(ev.messages[1].content, "a") == 0);
  ccord_live_event_free(&ev);
}

static void test_gateway_payload_builders(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 4096);

  CT_CHECK(ccord_gateway_build_identify("REDACTED_TEST_TOKEN", &b, &err) == CCORD_OK);
  const char *json = ccord_buf_cstr(&b);
  char cap[64];
  (void)snprintf(cap, sizeof cap, "\"capabilities\":%u", (unsigned)CCORD_LIVE_CAPABILITIES);
  CT_CHECK(strstr(json, "\"op\":2") != nullptr);
  CT_CHECK(strstr(json, cap) != nullptr);
  CT_CHECK(strstr(json, "\"compress\":false") != nullptr);
  CT_CHECK(strstr(json, "REDACTED_TEST_TOKEN") != nullptr);
  ccord_live_wipe(b.data, b.cap);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_gateway_build_resume("REDACTED_TEST_TOKEN", "sess-abc", 42, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":6") != nullptr);
  CT_CHECK(strstr(json, "\"session_id\":\"sess-abc\"") != nullptr);
  CT_CHECK(strstr(json, "\"seq\":42") != nullptr);
  ccord_live_wipe(b.data, b.cap);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_gateway_build_lazy_request(77, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":14") != nullptr);
  CT_CHECK(strstr(json, "\"guild_id\":\"77\"") != nullptr);
  CT_CHECK(strstr(json, "\"members\":[]") != nullptr);
  CT_CHECK(strstr(json, "\"activities\":true") != nullptr);
  ccord_live_wipe(b.data, b.cap);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_lazy_request(CCORD_DM_GUILD_ID, &b, &err) == CCORD_ERR_INVALID);
  ccord_buf_free(&b);

  ccord_gateway *gw = ccord_gateway_create(nullptr, "test_token", &err);
  CT_CHECK(gw != nullptr);
  if (gw) {
    CT_CHECK(ccord_gateway_select_guild(gw, 77, &err) == CCORD_OK);
    CT_CHECK(ccord_gateway_select_guild(gw, CCORD_DM_GUILD_ID, &err) == CCORD_OK);
    ccord_gateway_destroy(gw);
  }
}

static void test_gateway_session_lifecycle(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *ready =
    "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"session_id\":\"abc123\",\"resume_gateway_url\":\"wss://gateway-us-east1-b.discord.gg\","
    "\"guilds\":[],\"private_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.session_id != nullptr && strcmp(ev.session_id, "abc123") == 0);
  CT_CHECK(ev.resume_gateway_url != nullptr &&
           strcmp(ev.resume_gateway_url, "wss://gateway-us-east1-b.discord.gg") == 0);
  ccord_live_event_free(&ev);

  const char *reconnect = "{\"op\":7}";
  CT_CHECK(ccord_live_gateway_decode(reconnect, strlen(reconnect), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.reconnect);
  CT_CHECK(!ev.invalid_session);
  ccord_live_event_free(&ev);

  const char *invalid = "{\"op\":9,\"d\":true}";
  CT_CHECK(ccord_live_gateway_decode(invalid, strlen(invalid), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.invalid_session);
  CT_CHECK(ev.resumable);
  ccord_live_event_free(&ev);

  const char *expired = "{\"op\":9,\"d\":false}";
  CT_CHECK(ccord_live_gateway_decode(expired, strlen(expired), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.invalid_session);
  CT_CHECK(!ev.resumable);
  ccord_live_event_free(&ev);

  const char *resumed = "{\"op\":0,\"t\":\"RESUMED\",\"s\":8,\"d\":{}}";
  CT_CHECK(ccord_live_gateway_decode(resumed, strlen(resumed), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.resumed);
  ccord_live_event_free(&ev);
}

static void test_gateway_guild_events(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *create =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"s\":9,\"d\":{\"id\":\"77\",\"name\":\"HQ\","
    "\"channels\":[{\"id\":\"10\",\"type\":4,\"name\":\"TEXT\"},"
    "{\"id\":\"11\",\"type\":0,\"name\":\"general\",\"parent_id\":\"10\"}],"
    "\"roles\":[{\"id\":\"1\",\"name\":\"Admins\",\"color\":255,\"position\":0}],"
    "\"members\":[{\"user\":{\"id\":\"43\",\"username\":\"Ada\"},\"roles\":[\"1\"]}],"
    "\"presences\":[{\"user\":{\"id\":\"43\"},\"status\":\"online\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(create, strlen(create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT);
  CT_CHECK(ev.ev.guild_id == 77);
  CT_CHECK(ev.guild_count == 1);
  CT_CHECK(ev.guilds != nullptr && strcmp(ev.guilds[0].name, "HQ") == 0);
  CT_CHECK(ev.snap_channel_count == 2);
  CT_CHECK(ev.member_count == 1);
  CT_CHECK(ev.user_count == 1);
  CT_CHECK(ev.users[0].presence == CCORD_PRESENCE_ONLINE);
  ccord_live_event_free(&ev);

  const char *update =
    "{\"op\":0,\"t\":\"GUILD_UPDATE\",\"d\":{\"id\":\"77\",\"name\":\"HQ2\",\"channels\":[],"
    "\"roles\":[],\"members\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(update, strlen(update), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT);
  CT_CHECK(ev.guild_count == 1 && strcmp(ev.guilds[0].name, "HQ2") == 0);
  ccord_live_event_free(&ev);

  const char *remove = "{\"op\":0,\"t\":\"GUILD_DELETE\",\"d\":{\"id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(remove, strlen(remove), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_DELETE);
  CT_CHECK(ev.ev.guild_id == 77);
  ccord_live_event_free(&ev);

  const char *unavailable = "{\"op\":0,\"t\":\"GUILD_DELETE\",\"d\":{\"id\":\"77\",\"unavailable\":true}}";
  CT_CHECK(ccord_live_gateway_decode(unavailable, strlen(unavailable), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_NONE);
  CT_CHECK(ev.ev.guild_id == 77);
  ccord_live_event_free(&ev);

  const char *chunk =
    "{\"op\":0,\"t\":\"GUILD_MEMBERS_CHUNK\",\"d\":{\"guild_id\":\"77\","
    "\"members\":[{\"user\":{\"id\":\"44\",\"username\":\"Bob\"}}],"
    "\"presences\":[{\"user\":{\"id\":\"44\"},\"status\":\"idle\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(chunk, strlen(chunk), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_MEMBERS);
  CT_CHECK(ev.ev.guild_id == 77);
  CT_CHECK(ev.member_count == 1);
  CT_CHECK(ev.members[0].user_id == 44);
  CT_CHECK(ev.user_count == 1);
  CT_CHECK(ev.users[0].presence == CCORD_PRESENCE_IDLE);
  ccord_live_event_free(&ev);
}

static void test_message_reply_attachments_embeds(void)
{
  const char *json =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\","
    "\"author\":{\"id\":\"300\"},\"content\":\"look at this\","
    "\"referenced_message\":{\"id\":\"99\",\"content\":\"original text\","
    "\"author\":{\"id\":\"400\",\"username\":\"Ada\"}},"
    "\"attachments\":[{\"filename\":\"cat.png\",\"content_type\":\"image/png\",\"size\":2048}],"
    "\"embeds\":[{\"title\":\"Patch notes\",\"description\":\"v1 shipped\"}]}}";
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE);
  CT_CHECK(ev.message_count == 1);
  // The text is only the text now: the reply, files and embeds travel as structure.
  CT_CHECK(strcmp(ev.messages[0].content, "look at this") == 0);
  const ccord_message_rich *rr = ev.messages[0].rich;
  CT_CHECK(rr != nullptr);
  if (rr) {
    CT_CHECK(rr->ref_preview.present && strcmp(rr->ref_preview.author_name, "Ada") == 0 &&
             strcmp(rr->ref_preview.content, "original text") == 0);
    CT_CHECK(rr->reference.kind == CCORD_REF_REPLY);   // referenced_message alone makes it a reply
    CT_CHECK(rr->attachment_count == 1 && strcmp(rr->attachments[0].filename, "cat.png") == 0 &&
             strcmp(rr->attachments[0].content_type, "image/png") == 0 && rr->attachments[0].size == 2048);
    CT_CHECK(rr->embed_count == 1 && strcmp(rr->embeds[0].title, "Patch notes") == 0 &&
             strcmp(rr->embeds[0].description, "v1 shipped") == 0);
  }
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // The REST mapper shares the decoding, so history/send echoes carry the same structure.
  const char *rest =
    "{\"id\":\"101\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},\"content\":\"\","
    "\"attachments\":[{\"url\":\"https://cdn.discordapp.com/x/report.pdf\"}],"
    "\"embeds\":[{\"url\":\"https://example.test/post\"}]}";
  CT_CHECK(ccord_live_json_message(rest, strlen(rest), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].content[0] == '\0');
  rr = ev.messages[0].rich;
  CT_CHECK(rr && rr->attachment_count == 1 && strcmp(rr->attachments[0].url, "https://cdn.discordapp.com/x/report.pdf") == 0 &&
           rr->attachments[0].filename[0] == '\0');
  CT_CHECK(rr && rr->embed_count == 1 && strcmp(rr->embeds[0].url, "https://example.test/post") == 0);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);
}

static void test_rich_message_decode(void)
{
  ccord_live_event ev;
  ccord_error err;

  // A plain text message carries no rich block at all.
  const char *plain = "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\","
                      "\"author\":{\"id\":\"300\"},\"content\":\"hi\",\"type\":0,\"flags\":0,\"attachments\":[],\"embeds\":[],"
                      "\"mentions\":[],\"mention_roles\":[],\"mention_everyone\":false}}";
  CT_CHECK(ccord_live_gateway_decode(plain, strlen(plain), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].rich == nullptr);
  ccord_live_event_free(&ev);

  // A message with everything.
  const char *full =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"guild_id\":\"50\","
    "\"author\":{\"id\":\"300\",\"username\":\"bob\"},\"content\":\"look\",\"type\":19,\"flags\":32,"
    "\"mention_everyone\":true,\"mentions\":[{\"id\":\"11\",\"username\":\"a\"},{\"id\":\"12\",\"username\":\"b\"}],"
    "\"mention_roles\":[\"21\"],"
    "\"attachments\":[{\"id\":\"5\",\"filename\":\"SPOILER_cat.png\",\"description\":\"a cat\",\"content_type\":\"image/png\","
    "\"url\":\"https://cdn.example/cat.png\",\"proxy_url\":\"https://media.example/cat.png\",\"size\":2048,\"width\":640,"
    "\"height\":480,\"flags\":4},"
    "{\"id\":\"6\",\"filename\":\"voice.ogg\",\"content_type\":\"audio/ogg\",\"url\":\"https://cdn.example/v.ogg\","
    "\"size\":10,\"duration_secs\":2.5}],"
    "\"embeds\":[{\"type\":\"rich\",\"title\":\"Patch notes\",\"description\":\"v1 shipped\",\"url\":\"https://e.example/p\","
    "\"color\":3368601,\"timestamp\":\"2024-01-02T03:04:05.000000+00:00\",\"provider\":{\"name\":\"Example\"},"
    "\"author\":{\"name\":\"Ann\",\"url\":\"https://e.example/a\",\"icon_url\":\"https://e.example/a.png\"},"
    "\"footer\":{\"text\":\"foot\",\"icon_url\":\"https://e.example/f.png\"},"
    "\"image\":{\"url\":\"https://e.example/i.png\",\"width\":10,\"height\":20},"
    "\"thumbnail\":{\"url\":\"https://e.example/t.png\"},"
    "\"fields\":[{\"name\":\"f1\",\"value\":\"v1\",\"inline\":true},{\"name\":\"f2\",\"value\":\"v2\"}]},"
    "{\"type\":\"gifv\",\"url\":\"https://tenor.example/x\",\"video\":{\"url\":\"https://t.example/x.mp4\",\"width\":300,\"height\":200}}],"
    "\"sticker_items\":[{\"id\":\"9\",\"name\":\"wave\",\"format_type\":3}],"
    "\"message_reference\":{\"type\":0,\"message_id\":\"99\",\"channel_id\":\"200\",\"guild_id\":\"50\"},"
    "\"referenced_message\":{\"id\":\"99\",\"content\":\"original text\",\"attachments\":[{\"id\":\"1\"}],"
    "\"author\":{\"id\":\"400\",\"username\":\"ada\",\"global_name\":\"Ada L\"}},"
    "\"poll\":{\"question\":{\"text\":\"Pizza?\"},\"answers\":[{\"answer_id\":1,\"poll_media\":{\"text\":\"Yes\","
    "\"emoji\":{\"name\":\"x\"}}},{\"answer_id\":2,\"poll_media\":{\"text\":\"No\"}}],\"expiry\":\"2024-01-03T00:00:00.000000+00:00\","
    "\"allow_multiselect\":true,\"results\":{\"is_finalized\":false,\"answer_counts\":[{\"id\":1,\"count\":3,\"me_voted\":true},"
    "{\"id\":2,\"count\":1,\"me_voted\":false}]}},"
    "\"thread\":{\"id\":\"31\",\"name\":\"side thread\",\"message_count\":4}}}";
  CT_CHECK(ccord_live_gateway_decode(full, strlen(full), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1);
  const ccord_message_rich *r = ev.messages[0].rich;
  CT_CHECK(r != nullptr);
  if (r) {
    CT_CHECK(r->present == CCORD_RICH_ALL);
    CT_CHECK(r->type == 19 && r->flags == 32);
    CT_CHECK(r->mention_everyone && r->mention_user_count == 2 && r->mention_users[0] == 11 && r->mention_users[1] == 12);
    CT_CHECK(r->mention_role_count == 1 && r->mention_roles[0] == 21);
    CT_CHECK(r->attachment_count == 2);
    if (r->attachment_count == 2) {
      const ccord_attachment *a0 = &r->attachments[0];
      CT_CHECK(a0->id == 5 && strcmp(a0->filename, "SPOILER_cat.png") == 0 && a0->spoiler);
      CT_CHECK(strcmp(a0->description, "a cat") == 0 && strcmp(a0->content_type, "image/png") == 0);
      CT_CHECK(strcmp(a0->url, "https://cdn.example/cat.png") == 0 && strcmp(a0->proxy_url, "https://media.example/cat.png") == 0);
      CT_CHECK(a0->size == 2048 && a0->width == 640 && a0->height == 480 && a0->flags == 4 && a0->duration_ms == 0);
      const ccord_attachment *a1 = &r->attachments[1];
      CT_CHECK(!a1->spoiler && a1->duration_ms == 2500 && ccord_attachment_is_audio(a1));
    }
    CT_CHECK(r->embed_count == 2);
    if (r->embed_count == 2) {
      const ccord_embed *e = &r->embeds[0];
      CT_CHECK(e->kind == CCORD_EMBED_RICH && strcmp(e->title, "Patch notes") == 0 && strcmp(e->description, "v1 shipped") == 0);
      CT_CHECK(strcmp(e->url, "https://e.example/p") == 0 && e->has_color && e->color == 3368601u);
      CT_CHECK(e->timestamp_ms == 1704164645000LL);
      CT_CHECK(strcmp(e->provider, "Example") == 0 && strcmp(e->author_name, "Ann") == 0);
      CT_CHECK(strcmp(e->author_url, "https://e.example/a") == 0 && strcmp(e->author_icon, "https://e.example/a.png") == 0);
      CT_CHECK(strcmp(e->footer_text, "foot") == 0 && strcmp(e->footer_icon, "https://e.example/f.png") == 0);
      CT_CHECK(strcmp(e->image.url, "https://e.example/i.png") == 0 && e->image.width == 10 && e->image.height == 20);
      CT_CHECK(strcmp(e->thumbnail.url, "https://e.example/t.png") == 0 && e->video.url[0] == '\0');
      CT_CHECK(e->field_count == 2 && e->fields[0].inline_ && !e->fields[1].inline_ && strcmp(e->fields[1].value, "v2") == 0);
      const ccord_embed *g = &r->embeds[1];
      CT_CHECK(g->kind == CCORD_EMBED_GIFV && !g->has_color && g->video.width == 300 && g->video.height == 200);
    }
    CT_CHECK(r->sticker_count == 1 && r->stickers[0].id == 9 && r->stickers[0].format == 3 &&
             strcmp(r->stickers[0].name, "wave") == 0);
    CT_CHECK(r->reference.kind == CCORD_REF_REPLY && r->reference.message_id == 99 && r->reference.channel_id == 200 &&
             r->reference.guild_id == 50);
    CT_CHECK(r->ref_preview.present && !r->ref_preview.deleted && r->ref_preview.author_id == 400);
    CT_CHECK(strcmp(r->ref_preview.author_name, "Ada L") == 0 && strcmp(r->ref_preview.content, "original text") == 0);
    CT_CHECK(r->ref_preview.has_attachments);
    CT_CHECK(r->poll.present && strcmp(r->poll.question, "Pizza?") == 0 && r->poll.answer_count == 2 && r->poll.multiselect);
    CT_CHECK(!r->poll.finalized && r->poll.expiry_ms == 1704240000000LL);
    if (r->poll.answer_count == 2) {
      CT_CHECK(r->poll.answers[0].id == 1 && strcmp(r->poll.answers[0].text, "Yes") == 0 &&
               strcmp(r->poll.answers[0].emoji, "x") == 0 && r->poll.answers[0].votes == 3 && r->poll.answers[0].me_voted);
      CT_CHECK(r->poll.answers[1].id == 2 && strcmp(r->poll.answers[1].text, "No") == 0 && r->poll.answers[1].votes == 1 &&
               !r->poll.answers[1].me_voted);
    }
    CT_CHECK(r->thread.id == 31 && strcmp(r->thread.name, "side thread") == 0 && r->thread.message_count == 4);
    CT_CHECK(!r->forward.present);
  }
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // A reply whose original is gone: Discord says referenced_message is null.
  const char *gone = "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"101\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},"
                     "\"content\":\"re\",\"type\":19,\"message_reference\":{\"message_id\":\"98\"},\"referenced_message\":null}}";
  CT_CHECK(ccord_live_gateway_decode(gone, strlen(gone), &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->reference.kind == CCORD_REF_REPLY && r->reference.message_id == 98 && !r->ref_preview.present &&
           r->ref_preview.deleted);
  ccord_live_event_free(&ev);

  // Discord left referenced_message out: the original is unknown, not deleted.
  const char *unknown = "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"102\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},"
                        "\"content\":\"re\",\"type\":19,\"message_reference\":{\"message_id\":\"97\"}}}";
  CT_CHECK(ccord_live_gateway_decode(unknown, strlen(unknown), &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->reference.kind == CCORD_REF_REPLY && !r->ref_preview.present && !r->ref_preview.deleted);
  ccord_live_event_free(&ev);

  // A forward carries a snapshot of the original.
  const char *fwd =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"103\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},\"content\":\"\","
    "\"flags\":16384,\"message_reference\":{\"type\":1,\"message_id\":\"5\",\"channel_id\":\"6\",\"guild_id\":\"7\"},"
    "\"message_snapshots\":[{\"message\":{\"content\":\"forwarded text\",\"timestamp\":\"2024-01-02T03:04:05.000000+00:00\","
    "\"attachments\":[{\"id\":\"8\",\"filename\":\"f.png\",\"url\":\"https://cdn.example/f.png\"}],"
    "\"embeds\":[{\"title\":\"fe\"}],\"sticker_items\":[{\"id\":\"9\",\"name\":\"s\",\"format_type\":1}]}}]}}";
  CT_CHECK(ccord_live_gateway_decode(fwd, strlen(fwd), &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r != nullptr);
  if (r) {
    CT_CHECK(r->reference.kind == CCORD_REF_FORWARD && r->reference.message_id == 5 && r->reference.channel_id == 6);
    CT_CHECK((r->flags & CCORD_MSGFLAG_HAS_SNAPSHOT) != 0);
    CT_CHECK(r->forward.present && strcmp(r->forward.content, "forwarded text") == 0 && r->forward.timestamp_ms == 1704164645000LL);
    CT_CHECK(r->forward.attachment_count == 1 && strcmp(r->forward.attachments[0].filename, "f.png") == 0);
    CT_CHECK(r->forward.embed_count == 1 && strcmp(r->forward.embeds[0].title, "fe") == 0);
    CT_CHECK(r->forward.sticker_count == 1 && r->forward.stickers[0].id == 9);
    CT_CHECK(!r->ref_preview.present);
  }
  ccord_live_event_free(&ev);

  // Strings are copied out of the parse buffer: they survive after decode and are cut on a character boundary.
  char big[CCORD_RICH_SHORT_MAX_BYTES * 2u + 8u];
  memset(big, 'a', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  big[CCORD_RICH_SHORT_MAX_BYTES - 1u] = '\xC3';       // a two-byte character that straddles the cut
  big[CCORD_RICH_SHORT_MAX_BYTES] = '\xA9';
  char cap_json[2048];
  snprintf(cap_json, sizeof cap_json,
           "{\"id\":\"110\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},\"content\":\"x\","
           "\"attachments\":[{\"id\":\"1\",\"filename\":\"%s\",\"url\":\"https://cdn.example/a\"}]}", big);
  CT_CHECK(ccord_live_json_message(cap_json, strlen(cap_json), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->attachment_count == 1);
  if (r && r->attachment_count == 1) {
    size_t fl = strlen(r->attachments[0].filename);
    CT_CHECK(fl == CCORD_RICH_SHORT_MAX_BYTES - 1u);   // the half character is dropped, not kept
    CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  }
  ccord_live_event_free(&ev);

  // Too many of everything: capped, the message still decodes and validates.
  char many[8192];
  size_t w = (size_t)snprintf(many, sizeof many,
    "{\"id\":\"111\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},\"content\":\"x\",\"attachments\":[");
  for (unsigned i = 0; i < 15u; i++) {
    w += (size_t)snprintf(many + w, sizeof many - w, "%s{\"id\":\"%u\",\"filename\":\"f%u\",\"url\":\"https://c.example/%u\"}",
                          i ? "," : "", i + 1u, i, i);
  }
  w += (size_t)snprintf(many + w, sizeof many - w, "],\"embeds\":[");
  for (unsigned i = 0; i < 14u; i++) {
    w += (size_t)snprintf(many + w, sizeof many - w, "%s{\"title\":\"e%u\"}", i ? "," : "", i);
  }
  w += (size_t)snprintf(many + w, sizeof many - w, "],\"mentions\":[");
  for (unsigned i = 0; i < 130u; i++) {
    w += (size_t)snprintf(many + w, sizeof many - w, "%s{\"id\":\"%u\"}", i ? "," : "", i + 1u);
  }
  w += (size_t)snprintf(many + w, sizeof many - w, "],\"sticker_items\":[{\"id\":\"1\"},{\"id\":\"2\"},{\"id\":\"3\"},{\"id\":\"4\"},{\"id\":\"5\"}]}");
  CT_CHECK(w < sizeof many);
  CT_CHECK(ccord_live_json_message(many, strlen(many), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->attachment_count == CCORD_RICH_MAX_ATTACHMENTS && r->embed_count == CCORD_RICH_MAX_EMBEDS);
  CT_CHECK(r && r->mention_user_count == CCORD_RICH_MAX_MENTIONS && r->sticker_count == CCORD_RICH_MAX_STICKERS);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // An over-long URL is dropped rather than cut (half a link is worse than none).
  char longurl[CCORD_RICH_URL_MAX_BYTES + 64u];
  memcpy(longurl, "https://e.example/", 18);
  memset(longurl + 18, 'u', sizeof longurl - 19);
  longurl[sizeof longurl - 1] = '\0';
  char url_json[CCORD_RICH_URL_MAX_BYTES + 512u];
  snprintf(url_json, sizeof url_json,
           "{\"id\":\"112\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},\"content\":\"x\","
           "\"embeds\":[{\"title\":\"t\",\"url\":\"%s\",\"thumbnail\":{\"url\":\"https://e.example/ok.png\"}}]}", longurl);
  CT_CHECK(ccord_live_json_message(url_json, strlen(url_json), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->embed_count == 1 && r->embeds[0].url[0] == '\0' &&
           strcmp(r->embeds[0].thumbnail.url, "https://e.example/ok.png") == 0);
  ccord_live_event_free(&ev);

  // A partial MESSAGE_UPDATE carries only the parts whose keys are there.
  const char *upd_embeds = "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\","
                           "\"embeds\":[{\"type\":\"link\",\"title\":\"Preview\",\"url\":\"https://e.example/l\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(upd_embeds, strlen(upd_embeds), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_UPDATE && ev.message_count == 1);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->present == CCORD_RICH_HAS_EMBEDS && r->embed_count == 1 && r->embeds[0].kind == CCORD_EMBED_LINK);
  CT_CHECK(r && r->text_absent && ev.messages[0].content[0] == '\0');   // the text must not be replaced by this
  ccord_live_event_free(&ev);

  // An empty list under the key means "now empty" (a suppressed preview), distinct from the key being absent.
  const char *upd_clear = "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"embeds\":[],\"flags\":4}}";
  CT_CHECK(ccord_live_gateway_decode(upd_clear, strlen(upd_clear), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_UPDATE && ev.message_count == 1);
  r = ev.messages[0].rich;
  CT_CHECK(r && (r->present & CCORD_RICH_HAS_EMBEDS) != 0 && r->embed_count == 0 && (r->present & CCORD_RICH_HAS_TYPE) != 0 &&
           r->flags == CCORD_MSGFLAG_SUPPRESS_EMBEDS);
  CT_CHECK(r && (r->present & CCORD_RICH_HAS_ATTACHMENTS) == 0 && (r->present & CCORD_RICH_HAS_POLL) == 0 && r->text_absent);
  ccord_live_event_free(&ev);

  // An update that names neither the text nor any part says nothing about the message: no event.
  const char *upd_nothing = "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"guild_id\":\"50\"}}";
  CT_CHECK(ccord_live_gateway_decode(upd_nothing, strlen(upd_nothing), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 0);
  ccord_live_event_free(&ev);

  // A whole message is never "text absent", and a forward is not a reply.
  CT_CHECK(ccord_live_gateway_decode(fwd, strlen(fwd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && !ev.messages[0].rich->text_absent && ev.messages[0].reply_to_id == CCORD_ID_NONE);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_gateway_decode(gone, strlen(gone), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].reply_to_id == 98);
  ccord_live_event_free(&ev);

  // An edit of the text alone carries no rich block at all.
  const char *upd_text = "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"content\":\"edited\"}}";
  CT_CHECK(ccord_live_gateway_decode(upd_text, strlen(upd_text), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].rich == nullptr);
  ccord_live_event_free(&ev);
}

// The finer points of the decoding: which parts a partial update names, the caps, defaults and odd input.
static void test_rich_decode_details(void)
{
  ccord_live_event ev;
  ccord_error err;
  const ccord_message_rich *r;

  // Each part of a partial update is "present" exactly when its key is there.
  static const struct { const char *frag; uint32_t bit; } parts[] = {
    {"\"flags\":4", CCORD_RICH_HAS_TYPE},
    {"\"type\":19", CCORD_RICH_HAS_TYPE},
    {"\"mention_everyone\":true", CCORD_RICH_HAS_MENTIONS},
    {"\"mentions\":[]", CCORD_RICH_HAS_MENTIONS},
    {"\"mention_roles\":[\"5\"]", CCORD_RICH_HAS_MENTIONS},
    {"\"attachments\":[]", CCORD_RICH_HAS_ATTACHMENTS},
    {"\"embeds\":[]", CCORD_RICH_HAS_EMBEDS},
    {"\"sticker_items\":[]", CCORD_RICH_HAS_STICKERS},
    {"\"stickers\":[]", CCORD_RICH_HAS_STICKERS},
    {"\"message_reference\":{\"message_id\":\"9\"}", CCORD_RICH_HAS_REFERENCE},
    {"\"referenced_message\":null", CCORD_RICH_HAS_REFERENCE},
    {"\"message_snapshots\":[]", CCORD_RICH_HAS_FORWARD},
    {"\"poll\":{\"question\":{\"text\":\"q\"}}", CCORD_RICH_HAS_POLL},
    {"\"thread\":{\"id\":\"7\",\"name\":\"t\"}", CCORD_RICH_HAS_THREAD},
  };
  for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
    char json[512];
    snprintf(json, sizeof json, "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",%s}}", parts[i].frag);
    CT_CHECK(ccord_live_gateway_decode(json, strlen(json), &ev, &err) == CCORD_OK);
    CT_CHECK(ev.message_count == 1);
    r = ev.message_count == 1 ? ev.messages[0].rich : nullptr;
    if (!r || r->present != parts[i].bit || !r->text_absent) fprintf(stderr, "part %zu (%s): present=%x\n", i, parts[i].frag, r ? r->present : 0u);
    CT_CHECK(r && r->present == parts[i].bit && r->text_absent);
    ccord_live_event_free(&ev);
  }

  // An update with the text and a part: the text is carried, so the store replaces it.
  const char *upd_both = "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"content\":\"new\",\"embeds\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(upd_both, strlen(upd_both), &ev, &err) == CCORD_OK);
  r = ev.message_count == 1 ? ev.messages[0].rich : nullptr;
  CT_CHECK(r && !r->text_absent && r->present == CCORD_RICH_HAS_EMBEDS && strcmp(ev.messages[0].content, "new") == 0);
  ccord_live_event_free(&ev);
  // An empty text counts as carried (the author removed it), a non-text "content" does not.
  const char *upd_empty = "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"content\":\"\",\"embeds\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(upd_empty, strlen(upd_empty), &ev, &err) == CCORD_OK);
  r = ev.message_count == 1 ? ev.messages[0].rich : nullptr;
  CT_CHECK(r && !r->text_absent);
  ccord_live_event_free(&ev);
  const char *upd_odd = "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"content\":5,\"embeds\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(upd_odd, strlen(upd_odd), &ev, &err) == CCORD_OK);
  r = ev.message_count == 1 ? ev.messages[0].rich : nullptr;
  CT_CHECK(r && r->text_absent);
  ccord_live_event_free(&ev);

  // Embed kinds by their type word; an unknown or missing one is not an error.
  static const struct { const char *type; ccord_embed_kind kind; } kinds[] = {
    {"rich", CCORD_EMBED_RICH}, {"image", CCORD_EMBED_IMAGE}, {"video", CCORD_EMBED_VIDEO}, {"gifv", CCORD_EMBED_GIFV},
    {"article", CCORD_EMBED_ARTICLE}, {"link", CCORD_EMBED_LINK}, {"poll_result", CCORD_EMBED_POLL_RESULT},
    {"auto_moderation_message", CCORD_EMBED_OTHER},
  };
  for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
    char json[512];
    snprintf(json, sizeof json, "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\",\"embeds\":[{\"type\":\"%s\",\"title\":\"t\"}]}", kinds[i].type);
    CT_CHECK(ccord_live_json_message(json, strlen(json), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
    r = ev.messages[0].rich;
    CT_CHECK(r && r->embed_count == 1 && r->embeds[0].kind == kinds[i].kind);
    ccord_live_event_free(&ev);
  }
  const char *no_type = "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\",\"embeds\":[{\"title\":\"t\"}]}";
  CT_CHECK(ccord_live_json_message(no_type, strlen(no_type), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->embeds[0].kind == CCORD_EMBED_RICH);
  ccord_live_event_free(&ev);

  // Odd values are dropped, not trusted.
  const char *odd =
    "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"x\",\"type\":70000,\"flags\":-5,"
    "\"attachments\":[{},{\"id\":\"4\",\"filename\":\"only-name\"},{\"id\":\"5\",\"url\":\"https://c.example/x\",\"size\":-1,\"width\":-3,\"duration_secs\":-2}],"
    "\"embeds\":[{\"title\":\"c\",\"color\":16777216,\"timestamp\":\"nonsense\"},{\"title\":\"d\",\"color\":-1},{\"title\":\"e\",\"color\":0}],"
    "\"sticker_items\":[{\"name\":\"no id\",\"format_type\":1},{\"id\":\"6\",\"name\":\"fmt\",\"format_type\":7},{\"id\":\"7\",\"name\":\"gif\",\"format_type\":4}]}";
  CT_CHECK(ccord_live_json_message(odd, strlen(odd), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r != nullptr);
  if (r) {
    CT_CHECK(r->type == 0 && r->flags == 0);
    CT_CHECK(r->attachment_count == 2);   // the empty object is skipped; a name alone or a link alone is kept
    if (r->attachment_count == 2) {
      CT_CHECK(strcmp(r->attachments[0].filename, "only-name") == 0 && r->attachments[0].url[0] == '\0');
      CT_CHECK(r->attachments[1].size == 0 && r->attachments[1].width == 0 && r->attachments[1].duration_ms == 0);
    }
    CT_CHECK(r->embed_count == 3);
    if (r->embed_count == 3) {
      CT_CHECK(!r->embeds[0].has_color && r->embeds[0].timestamp_ms == 0);   // a colour must fit in 24 bits
      CT_CHECK(!r->embeds[1].has_color);
      CT_CHECK(r->embeds[2].has_color && r->embeds[2].color == 0);             // black is a colour
    }
    CT_CHECK(r->sticker_count == 2);   // one without an id is dropped
    if (r->sticker_count == 2) {
      CT_CHECK(r->stickers[0].id == 6 && r->stickers[0].format == 0);          // formats are 1..4
      CT_CHECK(r->stickers[1].id == 7 && r->stickers[1].format == 4);
    }
  }
  ccord_live_event_free(&ev);

  // Caps on the inner lists: 30 embed fields, 12 poll answers, and a reply preview over its snippet size.
  char big[16384];
  size_t w = (size_t)snprintf(big, sizeof big, "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\",\"embeds\":[{\"title\":\"f\",\"fields\":[");
  for (unsigned i = 0; i < 30u; i++) {
    w += (size_t)snprintf(big + w, sizeof big - w, "%s{\"name\":\"n%u\",\"value\":\"v\"}", i ? "," : "", i);
  }
  w += (size_t)snprintf(big + w, sizeof big - w, ",{\"name\":\"\",\"value\":\"\"}]}],\"poll\":{\"question\":{\"text\":\"\"},\"answers\":[");
  for (unsigned i = 0; i < 12u; i++) {
    w += (size_t)snprintf(big + w, sizeof big - w, "%s{\"poll_media\":{\"text\":\"a%u\"}}", i ? "," : "", i);
  }
  w += (size_t)snprintf(big + w, sizeof big - w, "]},\"referenced_message\":{\"content\":\"");
  for (unsigned i = 0; i < 400u; i++) big[w++] = 'x';
  w += (size_t)snprintf(big + w, sizeof big - w, "\",\"author\":{\"id\":\"4\",\"username\":\"u\"}},\"message_reference\":{\"message_id\":\"9\"}}");
  CT_CHECK(w < sizeof big);
  CT_CHECK(ccord_live_json_message(big, strlen(big), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->embed_count == 1 && r->embeds[0].field_count == CCORD_RICH_MAX_EMBED_FIELDS);
  CT_CHECK(r && r->poll.present && r->poll.answer_count == CCORD_RICH_MAX_POLL_ANSWERS);
  CT_CHECK(r && strcmp(r->poll.question, "Poll") == 0);                        // no question text: a placeholder
  CT_CHECK(r && r->poll.answers[0].id == 1 && r->poll.answers[9].id == 10);     // no answer_id: its position
  CT_CHECK(r && strlen(r->ref_preview.content) == CCORD_RICH_SNIPPET_MAX_BYTES);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // Poll results, custom emoji, a closed poll; a text-less question; a thread without an id is no thread.
  const char *poll =
    "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\","
    "\"poll\":{\"question\":{\"text\":\"Best?\"},\"answers\":[{\"answer_id\":1,\"poll_media\":{\"text\":\"A\",\"emoji\":{\"id\":\"777\",\"name\":\"blob\"}}},"
    "{\"answer_id\":2,\"poll_media\":{\"text\":\"B\"}}],\"allow_multiselect\":false,"
    "\"results\":{\"is_finalized\":true,\"answer_counts\":[{\"id\":2,\"count\":5,\"me_voted\":true},{\"id\":99,\"count\":1}]}},"
    "\"thread\":{\"name\":\"no id\"}}";
  CT_CHECK(ccord_live_json_message(poll, strlen(poll), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->poll.present && r->poll.finalized && !r->poll.multiselect && r->poll.expiry_ms == 0);
  CT_CHECK(r && r->poll.answer_count == 2 && r->poll.answers[0].emoji_id == 777 && strcmp(r->poll.answers[0].emoji, "blob") == 0);
  CT_CHECK(r && r->poll.answers[0].votes == 0 && !r->poll.answers[0].me_voted);    // no count for it
  CT_CHECK(r && r->poll.answers[1].votes == 5 && r->poll.answers[1].me_voted);     // counts match answers by id
  CT_CHECK(r && r->thread.id == 0 && r->thread.name[0] == '\0');
  ccord_live_event_free(&ev);

  // The user name on a reply preview: the display name when there is one, else the user name, else nothing.
  const char *names =
    "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"x\",\"message_reference\":{\"message_id\":\"9\"},"
    "\"referenced_message\":{\"content\":\"y\",\"author\":{\"id\":\"4\",\"username\":\"plain\",\"global_name\":\"\"}}}";
  CT_CHECK(ccord_live_json_message(names, strlen(names), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && strcmp(r->ref_preview.author_name, "plain") == 0 && r->ref_preview.author_id == 4 && !r->ref_preview.has_attachments);
  ccord_live_event_free(&ev);
  const char *anon =
    "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"x\",\"message_reference\":{\"message_id\":\"9\"},"
    "\"referenced_message\":{\"content\":\"y\",\"attachments\":[]}}";
  CT_CHECK(ccord_live_json_message(anon, strlen(anon), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->ref_preview.present && r->ref_preview.author_name[0] == '\0' && r->ref_preview.author_id == 0 && !r->ref_preview.has_attachments);
  ccord_live_event_free(&ev);

  // A forward carries the snapshot even when its parts are empty; one without a "message" is no forward.
  const char *fwd_min = "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\",\"message_snapshots\":[{\"message\":{}}]}";
  CT_CHECK(ccord_live_json_message(fwd_min, strlen(fwd_min), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->forward.present && r->forward.content[0] == '\0' && r->forward.timestamp_ms == 0 && r->forward.attachment_count == 0);
  ccord_live_event_free(&ev);
  const char *fwd_none = "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"hi\",\"message_snapshots\":[{}]}";
  CT_CHECK(ccord_live_json_message(fwd_none, strlen(fwd_none), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich == nullptr);
  ccord_live_event_free(&ev);

  // A duration is seconds with a fraction; flags pass through.
  const char *dur = "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\",\"flags\":8192,"
                    "\"attachments\":[{\"id\":\"4\",\"filename\":\"voice-message.ogg\",\"url\":\"https://c.example/v.ogg\",\"flags\":4,\"duration_secs\":3.25}]}";
  CT_CHECK(ccord_live_json_message(dur, strlen(dur), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->attachment_count == 1 && r->attachments[0].duration_ms == 3250 && r->attachments[0].flags == 4);
  CT_CHECK(r && (r->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0);
  CT_CHECK(r && r->attachments[0].waveform_b64 != nullptr && r->attachments[0].waveform_b64[0] == '\0');
  ccord_live_event_free(&ev);

  // A voice message's waveform is base64 text; a non-base64 or over-long one is dropped whole, not truncated.
  const char *wf = "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\",\"flags\":8192,"
                   "\"attachments\":[{\"id\":\"4\",\"filename\":\"voice-message.ogg\",\"url\":\"https://c.example/v.ogg\","
                   "\"duration_secs\":3.25,\"waveform\":\"AAECAwQFBgcICQ==\"},"
                   "{\"id\":\"5\",\"filename\":\"bad.ogg\",\"url\":\"https://c.example/b.ogg\",\"waveform\":\"no spaces!\"},"
                   "{\"id\":\"6\",\"filename\":\"num.ogg\",\"url\":\"https://c.example/n.ogg\",\"waveform\":7}]}";
  CT_CHECK(ccord_live_json_message(wf, strlen(wf), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.messages[0].rich;
  CT_CHECK(r && r->attachment_count == 3);
  if (r && r->attachment_count == 3) {
    CT_CHECK(strcmp(r->attachments[0].waveform_b64, "AAECAwQFBgcICQ==") == 0 && r->attachments[0].duration_ms == 3250);
    CT_CHECK(r->attachments[1].waveform_b64 != nullptr && r->attachments[1].waveform_b64[0] == '\0');
    CT_CHECK(r->attachments[2].waveform_b64 != nullptr && r->attachments[2].waveform_b64[0] == '\0');
  }
  ccord_live_event_free(&ev);
  char wf_big[CCORD_RICH_WAVEFORM_MAX_BYTES + 2];
  memset(wf_big, 'B', sizeof wf_big - 1);
  wf_big[sizeof wf_big - 1] = '\0';
  char *wf_long = ccord_malloc(1024 + sizeof wf_big);
  CT_CHECK(wf_long != nullptr);
  if (wf_long) {
    (void)snprintf(wf_long, 1024 + sizeof wf_big,
                   "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"\","
                   "\"attachments\":[{\"id\":\"4\",\"filename\":\"v.ogg\",\"url\":\"https://c.example/v.ogg\","
                   "\"duration_secs\":1,\"waveform\":\"%s\"}]}", wf_big);
    CT_CHECK(ccord_live_json_message(wf_long, strlen(wf_long), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
    r = ev.messages[0].rich;
    CT_CHECK(r && r->attachment_count == 1 && r->attachments[0].waveform_b64[0] == '\0' &&
             r->attachments[0].duration_ms == 1000);
    ccord_live_event_free(&ev);
    ccord_free(wf_long);
  }
}

// A whole message (history / send echo) around `extra`, decoded; the caller frees `ev`.
static const ccord_message_rich *decode_whole(const char *extra, ccord_live_event *ev)
{
  char json[8192];
  int n = snprintf(json, sizeof json, "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":\"x\",%s}", extra);
  ccord_error err;
  *ev = (ccord_live_event){0};
  if (n < 0 || (size_t)n >= sizeof json || ccord_live_json_message(json, (size_t)n, CCORD_NET_HISTORY, ev, &err) != CCORD_OK) {
    CT_CHECK(false);
    return nullptr;
  }
  CT_CHECK(ev->message_count == 1);
  return ev->message_count == 1 ? ev->messages[0].rich : nullptr;
}

// What makes a message carry a rich block at all (one part is enough), and the finer fields of embeds, stickers and
// snapshots that the big round-trip test does not exercise.
static void test_rich_decode_edges(void)
{
  ccord_live_event ev;
  ccord_error err;
  const ccord_message_rich *r;

  // A whole message is never "text absent", even when its content is missing or not a string: only an update can mean
  // "the text did not change".
  const char *no_content = "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"embeds\":[{\"title\":\"t\"}]}";
  CT_CHECK(ccord_live_json_message(no_content, strlen(no_content), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.message_count == 1 ? ev.messages[0].rich : nullptr;
  CT_CHECK(r && !r->text_absent && r->present == CCORD_RICH_ALL && ev.messages[0].content[0] == '\0');
  ccord_live_event_free(&ev);
  const char *odd_content = "{\"id\":\"1\",\"channel_id\":\"2\",\"author\":{\"id\":\"3\"},\"content\":7,\"embeds\":[{\"title\":\"t\"}]}";
  CT_CHECK(ccord_live_json_message(odd_content, strlen(odd_content), CCORD_NET_HISTORY, &ev, &err) == CCORD_OK);
  r = ev.message_count == 1 ? ev.messages[0].rich : nullptr;
  CT_CHECK(r && !r->text_absent);
  ccord_live_event_free(&ev);

  // Each part alone is enough for a block (and nothing else makes one).
  r = decode_whole("\"mention_everyone\":true", &ev);
  CT_CHECK(r && r->mention_everyone && r->mention_user_count == 0);
  ccord_live_event_free(&ev);
  r = decode_whole("\"mention_everyone\":false,\"mentions\":[],\"mention_roles\":[]", &ev);
  CT_CHECK(r == nullptr);
  ccord_live_event_free(&ev);
  r = decode_whole("\"flags\":4", &ev);
  CT_CHECK(r && r->flags == CCORD_MSGFLAG_SUPPRESS_EMBEDS && r->type == 0);
  ccord_live_event_free(&ev);
  r = decode_whole("\"type\":7", &ev);
  CT_CHECK(r && r->type == 7 && r->flags == 0);
  ccord_live_event_free(&ev);
  r = decode_whole("\"thread\":{\"id\":\"31\"}", &ev);
  CT_CHECK(r && r->thread.id == 31 && r->thread.name[0] == '\0' && r->thread.message_count == 0);
  ccord_live_event_free(&ev);
  r = decode_whole("\"message_reference\":{\"message_id\":\"9\"}", &ev);
  CT_CHECK(r && r->reference.kind == CCORD_REF_REPLY && r->reference.message_id == 9 && !r->ref_preview.present);
  ccord_live_event_free(&ev);
  r = decode_whole("\"message_snapshots\":[{\"message\":{}}]", &ev);
  CT_CHECK(r && r->forward.present);
  ccord_live_event_free(&ev);
  r = decode_whole("\"poll\":{\"question\":{\"text\":\"q\"},\"answers\":[]}", &ev);
  CT_CHECK(r && r->poll.present);
  ccord_live_event_free(&ev);
  r = decode_whole("\"sticker_items\":[{\"id\":\"4\"}]", &ev);
  CT_CHECK(r && r->sticker_count == 1);
  ccord_live_event_free(&ev);

  // A reference that names no message is not a reply (a crossposted message points at a channel only); a bare
  // `referenced_message: null` is a reply whose original is gone.
  r = decode_whole("\"flags\":2,\"message_reference\":{\"channel_id\":\"6\",\"guild_id\":\"7\"}", &ev);
  CT_CHECK(r && r->reference.kind == CCORD_REF_NONE && r->reference.message_id == 0 && r->reference.channel_id == 6 &&
           r->reference.guild_id == 7);
  ccord_live_event_free(&ev);
  r = decode_whole("\"referenced_message\":null", &ev);
  CT_CHECK(r && r->reference.kind == CCORD_REF_REPLY && r->ref_preview.deleted && !r->ref_preview.present);
  ccord_live_event_free(&ev);
  r = decode_whole("\"referenced_message\":{\"content\":\"y\",\"author\":{\"id\":\"4\",\"username\":\"u\"}}", &ev);
  CT_CHECK(r && r->reference.kind == CCORD_REF_REPLY && r->ref_preview.present && r->reference.message_id == 0);
  ccord_live_event_free(&ev);
  r = decode_whole("\"message_reference\":{\"type\":1,\"channel_id\":\"6\"},\"message_snapshots\":[{\"message\":{}}]", &ev);
  CT_CHECK(r && r->reference.kind == CCORD_REF_FORWARD && r->reference.message_id == 0);
  ccord_live_event_free(&ev);

  // A forward: the first snapshot is the one shown, and its stickers may come under either key.
  r = decode_whole("\"message_snapshots\":[{\"message\":{\"content\":\"one\"}},{\"message\":{\"content\":\"two\"}}]", &ev);
  CT_CHECK(r && r->forward.present && strcmp(r->forward.content, "one") == 0);
  ccord_live_event_free(&ev);
  r = decode_whole("\"message_snapshots\":[{\"message\":{\"stickers\":[{\"id\":\"9\",\"name\":\"s\",\"format_type\":2}]}}]", &ev);
  CT_CHECK(r && r->forward.sticker_count == 1 && r->forward.stickers[0].id == 9 && r->forward.stickers[0].format == 2);
  ccord_live_event_free(&ev);
  r = decode_whole("\"message_snapshots\":[{\"message\":{\"sticker_items\":[{\"id\":\"8\",\"name\":\"t\"}],"
                   "\"stickers\":[{\"id\":\"9\",\"name\":\"s\"}]}}]", &ev);
  CT_CHECK(r && r->forward.sticker_count == 1 && r->forward.stickers[0].id == 8);   // sticker_items wins
  ccord_live_event_free(&ev);

  // The spoiler mark is exactly "SPOILER_" at the start of the file name.
  r = decode_whole("\"attachments\":[{\"filename\":\"SPOILERcat.png\",\"url\":\"https://c.example/1\"},"
                   "{\"filename\":\"my_SPOILER_cat.png\",\"url\":\"https://c.example/2\"},"
                   "{\"filename\":\"SPOILER_cat.png\",\"url\":\"https://c.example/3\"}]", &ev);
  CT_CHECK(r && r->attachment_count == 3 && !r->attachments[0].spoiler && !r->attachments[1].spoiler && r->attachments[2].spoiler);
  ccord_live_event_free(&ev);

  // Embed media keeps both the link and the proxied link; an embed's own parts have their own caps.
  r = decode_whole("\"embeds\":[{\"title\":\"t\",\"image\":{\"url\":\"https://e.example/i\",\"proxy_url\":\"https://p.example/i\"},"
                   "\"thumbnail\":{\"proxy_url\":\"https://p.example/t\"},\"video\":{\"url\":\"https://e.example/v\",\"proxy_url\":\"https://p.example/v\"}}]", &ev);
  CT_CHECK(r && r->embed_count == 1);
  if (r && r->embed_count == 1) {
    const ccord_embed *e = &r->embeds[0];
    CT_CHECK(strcmp(e->image.proxy_url, "https://p.example/i") == 0 && strcmp(e->image.url, "https://e.example/i") == 0);
    CT_CHECK(strcmp(e->thumbnail.proxy_url, "https://p.example/t") == 0 && e->thumbnail.url[0] == '\0');
    CT_CHECK(strcmp(e->video.proxy_url, "https://p.example/v") == 0 && strcmp(e->video.url, "https://e.example/v") == 0);
  }
  ccord_live_event_free(&ev);

  char longtext[2300];
  memset(longtext, 'v', sizeof longtext - 1);
  longtext[sizeof longtext - 1] = '\0';
  char frag[4096];
  snprintf(frag, sizeof frag,
           "\"embeds\":[{\"title\":\"%.*s\",\"fields\":[{\"name\":\"\",\"value\":\"\"},{\"name\":\"only name\"},"
           "{\"value\":\"only value\",\"inline\":true},{\"name\":\"big\",\"value\":\"%s\"},{\"name\":\"in\",\"value\":\"v\",\"inline\":false}]}]",
           600, longtext, longtext);
  r = decode_whole(frag, &ev);
  CT_CHECK(r && r->embed_count == 1);
  if (r && r->embed_count == 1) {
    const ccord_embed *e = &r->embeds[0];
    CT_CHECK(strlen(e->title) == CCORD_RICH_SHORT_MAX_BYTES * 2u);
    CT_CHECK(e->field_count == 4);   // the empty one is dropped; a name alone or a value alone is kept
    if (e->field_count == 4) {
      CT_CHECK(strcmp(e->fields[0].name, "only name") == 0 && e->fields[0].value[0] == '\0' && !e->fields[0].inline_);
      CT_CHECK(e->fields[1].name[0] == '\0' && strcmp(e->fields[1].value, "only value") == 0 && e->fields[1].inline_);
      CT_CHECK(strlen(e->fields[2].value) == CCORD_RICH_FIELD_VALUE_MAX_BYTES);
      CT_CHECK(!e->fields[3].inline_);
    }
  }
  CT_CHECK(r && ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // A sticker format outside 1..4 (including a negative one) is "unknown", not a wrapped number.
  r = decode_whole("\"sticker_items\":[{\"id\":\"1\",\"format_type\":-1},{\"id\":\"2\",\"format_type\":0},{\"id\":\"3\",\"format_type\":1}]", &ev);
  CT_CHECK(r && r->sticker_count == 3 && r->stickers[0].format == 0 && r->stickers[1].format == 0 && r->stickers[2].format == 1);
  ccord_live_event_free(&ev);
}

static void test_live_transport_create(void)
{
  char dir[512];
  if (!make_temp(dir, sizeof dir)) {
    fprintf(stderr, "test_live_transport: no TMPDIR, skipping create test\n");
    return;
  }
  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);
  ccord_error err;
  ccord_live_config missing = {.token_file = "/nonexistent/ccord_token", .disable_gateway = true};
  ccord_transport *t = ccord_live_transport_create(&missing, &err);
  CT_CHECK(t == nullptr);
  CT_CHECK(err.code == CCORD_ERR_NOT_FOUND);

  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .disable_gateway = true,
  };
  t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (t) {
    CT_CHECK(strcmp(t->vt->name, "live") == 0);
    CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
    ccord_net_event ev;
    CT_CHECK(ccord_transport_poll(t, &ev));
    CT_CHECK(ev.type == CCORD_NET_STATE);
    CT_CHECK(ev.state == CCORD_CONN_CONNECTED);
    ccord_transport_destroy(t);
  }
  CT_CHECK(rmdir(dir) == 0);
}


// live_fetch_media_range uses the clicked-video gate: https, a DNS host, port 443. Everything else is refused before
// anything is queued (so nothing here touches the network).
static void test_fetch_media_range_gate(void)
{
  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);
  ccord_error err;
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN", .disable_gateway = true};
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(t->vt->fetch_media_range != nullptr);
  const char *refused[] = {
      "http://videos.example.org/a.mp4",
      "https://203.0.113.5/a.mp4",
      "https://127.0.0.1/a.mp4",
      "https://[::1]/a.mp4",
      "https://localhost/a.mp4",
      "https://videos.example.org:8443/a.mp4",
      "https://videos.example.org/a b.mp4",
      "ftp://videos.example.org/a.mp4",
  };
  for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
    ccord_media_range_req req = {.id = 1, .url = refused[i], .start = 0, .len = 1024};
    CT_CHECK(ccord_transport_fetch_media_range(t, &req, &err) == CCORD_ERR_PERMISSION);
  }
  ccord_media_range_req bad = {.id = 2, .url = "https://videos.example.org/a.mp4", .start = -1, .len = 1024};
  CT_CHECK(ccord_transport_fetch_media_range(t, &bad, &err) == CCORD_ERR_INVALID);
  bad.start = 0;
  bad.len = 0;
  CT_CHECK(ccord_transport_fetch_media_range(t, &bad, &err) == CCORD_ERR_INVALID);
  bad.len = CCORD_STREAM_RANGE_MAX_BYTES + 1u;
  CT_CHECK(ccord_transport_fetch_media_range(t, &bad, &err) == CCORD_ERR_INVALID);
  ccord_media_range_req empty = {.id = 3, .url = "", .start = 0, .len = 1024};
  CT_CHECK(ccord_transport_fetch_media_range(t, &empty, &err) == CCORD_ERR_INVALID);
  ccord_transport_destroy(t);
}

static void test_gateway_voice_and_stream(void)
{
  ccord_live_event ev;
  ccord_error err;

  const char *ready =
    "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"guilds\":[{\"id\":\"77\",\"name\":\"HQ\",\"channels\":[{\"id\":\"11\",\"type\":2,\"name\":\"General\"}],"
    "\"roles\":[],\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"}},"
    "{\"user\":{\"id\":\"43\",\"username\":\"Ada\"}}]}],\"private_channels\":[],"
    "\"voice_states\":[{\"guild_id\":\"77\",\"channel_id\":\"11\",\"user_id\":\"43\","
    "\"self_mute\":true,\"self_deaf\":false,\"self_video\":true,\"self_stream\":false}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.voice_state_count == 1);
  CT_CHECK(ev.snapshot.voice_states[0].guild_id == 77);
  CT_CHECK(ev.snapshot.voice_states[0].channel_id == 11);
  CT_CHECK(ev.snapshot.voice_states[0].user_id == 43);
  CT_CHECK(ev.snapshot.voice_states[0].self_mute && ev.snapshot.voice_states[0].self_video);
  CT_CHECK(ccord_snapshot_validate(&ev.snapshot, &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  const char *vsu =
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\","
    "\"user_id\":\"43\",\"self_mute\":true,\"self_deaf\":false,\"self_video\":false,\"self_stream\":true}}";
  CT_CHECK(ccord_live_gateway_decode(vsu, strlen(vsu), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATE_UPDATE);
  CT_CHECK(ev.ev.voice_state != nullptr && ev.ev.voice_state->self_stream && ev.ev.voice_state->self_mute);
  CT_CHECK(ev.ev.guild_id == 77 && ev.ev.channel_id == 11 && ev.ev.user_id == 43);
  ccord_live_event_free(&ev);

  const char *left =
    "{\"op\":0,\"t\":\"VOICE_STATE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":null,\"user_id\":\"43\"}}";
  CT_CHECK(ccord_live_gateway_decode(left, strlen(left), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.voice_state != nullptr && ev.ev.voice_state->channel_id == CCORD_ID_NONE);
  ccord_live_event_free(&ev);

  const char *server =
    "{\"op\":0,\"t\":\"VOICE_SERVER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"channel_id\":\"11\","
    "\"token\":\"REDACTED_TEST_TOKEN\",\"endpoint\":\"voice.example\"}}";
  CT_CHECK(ccord_live_gateway_decode(server, strlen(server), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_SERVER_UPDATE);
  CT_CHECK(ev.ev.voice_server != nullptr && ev.ev.voice_server->guild_id == 77);
  CT_CHECK(strcmp(ev.ev.voice_server->endpoint, "voice.example") == 0);
  ccord_live_event_free(&ev);

  const char *reply =
    "{\"id\":\"101\",\"channel_id\":\"200\",\"author\":{\"id\":\"300\"},\"content\":\"hello\","
    "\"message_reference\":{\"message_id\":\"99\"},\"pinned\":true}";
  CT_CHECK(ccord_live_json_message(reply, strlen(reply), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].reply_to_id == 99);
  CT_CHECK(ev.messages[0].pinned);
  ccord_live_event_free(&ev);

  const char *st_create =
    "{\"op\":0,\"t\":\"STREAM_CREATE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"rtc_server_id\":\"999\",\"paused\":false}}";
  CT_CHECK(ccord_live_gateway_decode(st_create, strlen(st_create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STREAM_SIGNAL && ev.ev.stream_signal != nullptr);
  CT_CHECK(ev.ev.stream_signal->kind == CCORD_STREAM_SIG_CREATE);
  CT_CHECK(ev.ev.stream_signal->guild_id == 77 && ev.ev.stream_signal->channel_id == 11 && ev.ev.stream_signal->user_id == 43);
  CT_CHECK(!ev.ev.stream_signal->paused);
  ccord_live_event_free(&ev);

  const char *st_srv =
    "{\"op\":0,\"t\":\"STREAM_SERVER_UPDATE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"token\":\"st_tok\",\"endpoint\":\"stream.discord.media:443\"}}";
  CT_CHECK(ccord_live_gateway_decode(st_srv, strlen(st_srv), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STREAM_SIGNAL && ev.ev.stream_signal != nullptr);
  CT_CHECK(ev.ev.stream_signal->kind == CCORD_STREAM_SIG_SERVER_UPDATE);
  CT_CHECK(strcmp(ev.ev.stream_signal->token, "st_tok") == 0);
  CT_CHECK(strcmp(ev.ev.stream_signal->endpoint, "stream.discord.media:443") == 0);
  ccord_live_event_free(&ev);

  const char *st_del =
    "{\"op\":0,\"t\":\"STREAM_DELETE\",\"d\":{\"stream_key\":\"guild:77:11:43\",\"reason\":\"stream_full\"}}";
  CT_CHECK(ccord_live_gateway_decode(st_del, strlen(st_del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STREAM_SIGNAL && ev.ev.stream_signal != nullptr);
  CT_CHECK(ev.ev.stream_signal->kind == CCORD_STREAM_SIG_DELETE);
  CT_CHECK(strcmp(ev.ev.stream_signal->reason, "stream_full") == 0);
  ccord_live_event_free(&ev);
}

static void test_voice_stream_payload_builders(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 4096);

  CT_CHECK(ccord_gateway_build_voice_state(77, 11, true, true, false, true, &b, &err) == CCORD_OK);
  const char *json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":4") != nullptr);
  CT_CHECK(strstr(json, "\"guild_id\":\"77\"") != nullptr);
  CT_CHECK(strstr(json, "\"channel_id\":\"11\"") != nullptr);
  CT_CHECK(strstr(json, "\"self_mute\":true") != nullptr);
  CT_CHECK(strstr(json, "\"self_stream\":true") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_gateway_build_voice_state(77, CCORD_ID_NONE, false, false, false, false, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"channel_id\":null") != nullptr);
  ccord_buf_clear(&b);

  ccord_stream_config cfg = {.encoder = CCORD_STREAM_ENCODER_NVENC, .codec = CCORD_STREAM_CODEC_AV1,
                             .framework = CCORD_STREAM_FRAMEWORK_DMABUF,
                             .resolution = CCORD_STREAM_RES_1440P, .fps = CCORD_STREAM_FPS_60};
  CT_CHECK(ccord_gateway_build_stream_create(77, 11, &cfg, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":18") != nullptr);
  CT_CHECK(strstr(json, "\"encoder\":\"nvenc\"") != nullptr);
  CT_CHECK(strstr(json, "\"codec\":\"AV1\"") != nullptr);
  CT_CHECK(strstr(json, "\"framework\":\"dmabuf\"") != nullptr);
  CT_CHECK(strstr(json, "\"resolution\":\"1440p\"") != nullptr);
  CT_CHECK(strstr(json, "\"max_width\":2560") != nullptr);
  CT_CHECK(strstr(json, "\"max_height\":1440") != nullptr);
  CT_CHECK(strstr(json, "\"max_fps\":60") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_gateway_build_stream_delete(77, 11, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":19") != nullptr);
  CT_CHECK(strstr(json, "\"channel_id\":\"11\"") != nullptr);
  ccord_buf_clear(&b);

  char skey[96] = {0};
  ccord_stream_key_format(77, 11, 43, skey);
  CT_CHECK(strcmp(skey, "guild:77:11:43") == 0);
  ccord_id gid = 0, cid = 0, uid = 0;
  CT_CHECK(ccord_stream_key_parse(skey, &gid, &cid, &uid));
  CT_CHECK(gid == 77 && cid == 11 && uid == 43);

  CT_CHECK(ccord_gateway_build_stream_watch(skey, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":20") != nullptr);
  CT_CHECK(strstr(json, "\"stream_key\":\"guild:77:11:43\"") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_gateway_build_stream_set_paused(skey, true, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":22") != nullptr);
  CT_CHECK(strstr(json, "\"paused\":true") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_gateway_build_stream_delete_key(skey, &b, &err) == CCORD_OK);
  json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":19") != nullptr);
  CT_CHECK(strstr(json, "\"stream_key\":\"guild:77:11:43\"") != nullptr);
  ccord_buf_free(&b);
}

static void test_gateway_presence_builder(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 512);
  CT_CHECK(ccord_gateway_build_presence(CCORD_PRESENCE_IDLE, "building", &b, &err) == CCORD_OK);
  const char *json = ccord_buf_cstr(&b);
  CT_CHECK(strstr(json, "\"op\":3") != nullptr);
  CT_CHECK(strstr(json, "\"status\":\"idle\"") != nullptr);
  CT_CHECK(strstr(json, "\"state\":\"building\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_presence(CCORD_PRESENCE_DND, nullptr, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"status\":\"dnd\"") != nullptr);
  ccord_buf_clear(&b);
  // Invisible: the gateway has no "offline" status to *send*, it is "invisible".
  CT_CHECK(ccord_gateway_build_presence(CCORD_PRESENCE_OFFLINE, nullptr, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"status\":\"invisible\"") != nullptr);
  ccord_buf_free(&b);
}

static bool wait_for_event(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out)
{
  for (int i = 0; i < 300; i++) {
    if (ccord_transport_poll(t, out)) {
      if (out->type == want) return true;
      if (out->type == CCORD_NET_ERROR) return false;
    }
    usleep(10000);
  }
  return false;
}

static void test_rest_queue_rate_limit_and_pins(void)
{
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return;
  int opt = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sin.sin_port = 0;
  if (bind(lfd, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(lfd, 4) != 0) {
    close(lfd);
    return;
  }
  socklen_t slen = sizeof sin;
  if (getsockname(lfd, (struct sockaddr *)&sin, &slen) != 0) {
    close(lfd);
    return;
  }
  int port = ntohs(sin.sin_port);

  const char *pin_json =
    "[{\"id\":\"2\",\"channel_id\":\"10\",\"author\":{\"id\":\"42\"},\"content\":\"pin\","
    "\"timestamp\":\"2026-11-08T00:00:00Z\",\"pinned\":true}]";
  pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    for (int i = 0; i < 3; i++) {
      int cfd = accept(lfd, nullptr, nullptr);
      if (cfd < 0) _exit(1);
      char buf[4096] = {0};
      (void)read(cfd, buf, sizeof buf - 1);
      char resp[1024];
      int n = 0;
      if (i == 0) {
        n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 20\r\nConnection: close\r\n\r\n"
                     "{\"retry_after\":0.05}");
      } else if (i == 1) {
        n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n[]");
      } else {
        n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     strlen(pin_json), pin_json);
      }
      if (n > 0) (void)write(cfd, resp, (size_t)n);
      close(cfd);
    }
    _exit(0);
  }

  char base_url[64];
  (void)snprintf(base_url, sizeof base_url, "http://127.0.0.1:%d", port);
  ccord_error err;
  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = base_url,
    .disable_gateway = true,
    .request_timeout_ms = 3000,
  };
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (t) {
    CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
    ccord_net_event ev;
    CT_CHECK(wait_for_event(t, CCORD_NET_STATE, &ev));
    CT_CHECK(ev.state == CCORD_CONN_CONNECTED);
    CT_CHECK(ccord_transport_request_history(t, 10, 50, &err) == CCORD_OK);
    CT_CHECK(wait_for_event(t, CCORD_NET_HISTORY, &ev));
    CT_CHECK(ev.channel_id == 10);
    CT_CHECK(ccord_transport_request_pins(t, 10, 50, &err) == CCORD_OK);
    CT_CHECK(wait_for_event(t, CCORD_NET_PIN_LIST, &ev));
    CT_CHECK(ev.channel_id == 10 && ev.message_count == 1);
    CT_CHECK(ev.messages && ev.messages[0].pinned);
    ccord_transport_destroy(t);
  }
  close(lfd);
  int wstatus = 0;
  pid_t waited = waitpid(pid, &wstatus, 0);
  CT_CHECK(waited == pid);
  CT_CHECK(WIFEXITED(wstatus));
  CT_CHECK(WEXITSTATUS(wstatus) == 0);
}

static void test_ready_reserves_dm_slot(void)
{
  ccord_buf b;
  ccord_buf_init(&b, 16384);
  bool ok = ccord_buf_append_str(&b, "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
                                     "\"guilds\":[") == CCORD_OK;
  for (size_t i = 0; ok && i < CCORD_MAX_GUILDS; i++) {
    if (ccord_buf_appendf(&b, "%s{\"id\":\"%llu\",\"name\":\"G\",\"channels\":[],\"members\":[]}",
                          i ? "," : "", (unsigned long long)(10000 + i)) != CCORD_OK) {
      ok = false;
    }
  }
  if (ok) {
    ok = ccord_buf_append_str(&b, "],\"private_channels\":[{\"id\":\"900\",\"type\":1,"
                                  "\"recipients\":[{\"username\":\"Ada\"}]}]}}") == CCORD_OK;
  }
  CT_CHECK(ok);
  ccord_live_event ev;
  ccord_error err;
  CT_CHECK(ccord_live_gateway_decode(ccord_buf_cstr(&b), b.len, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.guild_count == CCORD_MAX_REAL_GUILDS);
  bool saw_dm = false;
  for (size_t i = 0; i < ev.snapshot.channel_count; i++) {
    if (ev.snapshot.channels[i].guild_id == CCORD_DM_GUILD_ID) saw_dm = true;
  }
  CT_CHECK(saw_dm);
  ccord_live_event_free(&ev);
  ccord_buf_free(&b);
}

static void test_token_save_load_delete(void)
{
  char dir[512];
  if (!make_temp(dir, sizeof dir)) return;
  const char *old_xdg = getenv("XDG_CONFIG_HOME");
  char saved_xdg[512] = {0};
  if (old_xdg) (void)snprintf(saved_xdg, sizeof saved_xdg, "%s", old_xdg);
  (void)setenv("XDG_CONFIG_HOME", dir, 1);
  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);

  ccord_error err;
  char tpath[1024];
  CT_CHECK(ccord_live_token_path(tpath, sizeof tpath, &err) == CCORD_OK);
  CT_CHECK(strstr(tpath, "/ccord/token") != nullptr);

  CT_CHECK(ccord_live_save_token(nullptr, "  \"MY_STANDALONE_TOKEN_123\" \n", &err) == CCORD_OK);
  struct stat st;
  CT_CHECK(stat(tpath, &st) == 0);
  CT_CHECK((st.st_mode & 0777) == 0600);

  char loaded[256];
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "MY_STANDALONE_TOKEN_123") == 0);
  ccord_live_wipe(loaded, sizeof loaded);

  CT_CHECK(ccord_live_delete_token(nullptr, &err) == CCORD_OK);
  CT_CHECK(stat(tpath, &st) != 0);
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_ERR_NOT_FOUND);

  char subdir[600];
  (void)snprintf(subdir, sizeof subdir, "%s/ccord", dir);
  (void)rmdir(subdir);
  (void)rmdir(dir);
  if (saved_xdg[0]) (void)setenv("XDG_CONFIG_HOME", saved_xdg, 1);
  else (void)unsetenv("XDG_CONFIG_HOME");
}

static bool path_mode_0600(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && (st.st_mode & 0777) == 0600;
}

static void test_multi_account_keyring(void)
{
  char dir[512];
  if (!make_temp(dir, sizeof dir)) return;
  const char *old_xdg = getenv("XDG_CONFIG_HOME");
  char saved_xdg[512] = {0};
  if (old_xdg) (void)snprintf(saved_xdg, sizeof saved_xdg, "%s", old_xdg);
  (void)setenv("XDG_CONFIG_HOME", dir, 1);
  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);

  ccord_error err = {0};
  char token_path[1024], accounts_path[1024], slot_path[1024];
  CT_CHECK(ccord_live_token_path(token_path, sizeof token_path, &err) == CCORD_OK);
  (void)snprintf(accounts_path, sizeof accounts_path, "%s/ccord/accounts.ini", dir);

  // A legacy single token is migrated into a "legacy" account without losing the token.
  CT_CHECK(ccord_live_save_token(nullptr, "tok_acct_legacy_0", &err) == CCORD_OK);
  ccord_saved_account accounts[CCORD_MAX_SAVED_ACCOUNTS];
  size_t count = 0;
  int active = -2;
  CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
  CT_CHECK(count == 1 && active == 0);
  CT_CHECK(strcmp(accounts[0].slot_id, "legacy") == 0);
  CT_CHECK(accounts[0].active);
  CT_CHECK(path_mode_0600(token_path));
  CT_CHECK(path_mode_0600(accounts_path));
  (void)snprintf(slot_path, sizeof slot_path, "%s/ccord/tokens/legacy", dir);
  CT_CHECK(path_mode_0600(slot_path));
  char loaded[256];
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_legacy_0") == 0);
  ccord_live_wipe(loaded, sizeof loaded);

  // Two more accounts, each in its own 0600 token file.
  ccord_saved_account alpha = {0};
  alpha.user_id = 111;
  (void)snprintf(alpha.username, sizeof alpha.username, "alpha");
  (void)snprintf(alpha.display_name, sizeof alpha.display_name, "Alpha");
  (void)snprintf(alpha.avatar_hash, sizeof alpha.avatar_hash, "av_alpha");
  (void)snprintf(alpha.slot_id, sizeof alpha.slot_id, "alpha");
  CT_CHECK(ccord_live_account_upsert(&alpha, "tok_acct_alpha_1", true, &err) == CCORD_OK);
  (void)snprintf(slot_path, sizeof slot_path, "%s/ccord/tokens/alpha", dir);
  CT_CHECK(path_mode_0600(slot_path));
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_alpha_1") == 0);
  ccord_live_wipe(loaded, sizeof loaded);

  ccord_saved_account beta = {0};
  beta.user_id = 222;
  (void)snprintf(beta.username, sizeof beta.username, "beta");
  (void)snprintf(beta.display_name, sizeof beta.display_name, "Beta");
  (void)snprintf(beta.avatar_hash, sizeof beta.avatar_hash, "av_beta");
  (void)snprintf(beta.slot_id, sizeof beta.slot_id, "beta");
  CT_CHECK(ccord_live_account_upsert(&beta, "tok_acct_beta_2", false, &err) == CCORD_OK);

  CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
  CT_CHECK(count == 3 && active == 1);
  CT_CHECK(accounts[1].user_id == 111 && strcmp(accounts[1].display_name, "Alpha") == 0 &&
           strcmp(accounts[1].avatar_hash, "av_alpha") == 0 && accounts[1].active);
  CT_CHECK(accounts[2].user_id == 222 && strcmp(accounts[2].slot_id, "beta") == 0 && !accounts[2].active);

  // Updating the active account rewrites its token and the legacy mirror; no duplicate row.
  (void)snprintf(alpha.display_name, sizeof alpha.display_name, "Alpha Two");
  CT_CHECK(ccord_live_account_upsert(&alpha, "tok_acct_alpha_1b", false, &err) == CCORD_OK);
  CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
  CT_CHECK(count == 3 && active == 1 && strcmp(accounts[1].display_name, "Alpha Two") == 0);
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_alpha_1b") == 0);
  ccord_live_wipe(loaded, sizeof loaded);

  // Peeking reads a non-active slot's token and changes nothing on disk.
  {
    const char *files[] = {"ccord/token", "ccord/accounts.ini", "ccord/tokens/legacy", "ccord/tokens/alpha",
                           "ccord/tokens/beta"};
    struct stat before[5], after[5];
    char fb[5][1100], content[5][512];
    for (int i = 0; i < 5; i++) {
      (void)snprintf(fb[i], sizeof fb[i], "%s/%s", dir, files[i]);
      CT_CHECK(stat(fb[i], &before[i]) == 0);
      content[i][0] = '\0';
      FILE *f = fopen(fb[i], "rb");
      if (f) {
        size_t n = fread(content[i], 1, sizeof content[i] - 1, f);
        content[i][n] = '\0';
        (void)fclose(f);
      }
    }
    CT_CHECK(ccord_live_account_peek_token("beta", loaded, sizeof loaded, &err) == CCORD_OK);
    CT_CHECK(strcmp(loaded, "tok_acct_beta_2") == 0);
    ccord_live_wipe(loaded, sizeof loaded);
    CT_CHECK(ccord_live_account_peek_token("222", loaded, sizeof loaded, &err) == CCORD_OK);
    CT_CHECK(strcmp(loaded, "tok_acct_beta_2") == 0);
    ccord_live_wipe(loaded, sizeof loaded);
    CT_CHECK(ccord_live_account_peek_token("missing", loaded, sizeof loaded, &err) == CCORD_ERR_NOT_FOUND);
    CT_CHECK(loaded[0] == '\0');
    CT_CHECK(ccord_live_account_peek_token(nullptr, loaded, sizeof loaded, &err) == CCORD_ERR_NOT_FOUND);
    for (int i = 0; i < 5; i++) {
      CT_CHECK(stat(fb[i], &after[i]) == 0);
      CT_CHECK(before[i].st_mtim.tv_sec == after[i].st_mtim.tv_sec &&
               before[i].st_mtim.tv_nsec == after[i].st_mtim.tv_nsec && before[i].st_size == after[i].st_size);
      char now[512] = "";
      FILE *f = fopen(fb[i], "rb");
      if (f) {
        size_t n = fread(now, 1, sizeof now - 1, f);
        now[n] = '\0';
        (void)fclose(f);
      }
      CT_CHECK(strcmp(now, content[i]) == 0);
    }
    CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
    CT_CHECK(active == 1);
  }

  // Switching by slot or decimal user id returns the token and syncs the legacy file.
  CT_CHECK(ccord_live_account_switch("beta", loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_beta_2") == 0);
  ccord_live_wipe(loaded, sizeof loaded);
  CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
  CT_CHECK(active == 2 && accounts[2].active && !accounts[1].active);
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_beta_2") == 0);
  ccord_live_wipe(loaded, sizeof loaded);

  CT_CHECK(ccord_live_account_switch("111", loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_alpha_1b") == 0);
  ccord_live_wipe(loaded, sizeof loaded);
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_alpha_1b") == 0);
  ccord_live_wipe(loaded, sizeof loaded);

  // Removing the active account selects the next one and removes its token file.
  CT_CHECK(ccord_live_account_remove("alpha", &err) == CCORD_OK);
  CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
  CT_CHECK(count == 2 && active == 1 && strcmp(accounts[1].slot_id, "beta") == 0 && accounts[1].active);
  (void)snprintf(slot_path, sizeof slot_path, "%s/ccord/tokens/alpha", dir);
  CT_CHECK(access(slot_path, F_OK) != 0);
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_OK);
  CT_CHECK(strcmp(loaded, "tok_acct_beta_2") == 0);
  ccord_live_wipe(loaded, sizeof loaded);

  // Removing down to the migrated legacy account and then everything leaves no token behind.
  CT_CHECK(ccord_live_account_remove("222", &err) == CCORD_OK);
  CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
  CT_CHECK(count == 1 && active == 0 && strcmp(accounts[0].slot_id, "legacy") == 0);
  CT_CHECK(ccord_live_account_remove("legacy", &err) == CCORD_OK);
  CT_CHECK(ccord_live_accounts_list(accounts, CCORD_MAX_SAVED_ACCOUNTS, &count, &active, &err) == CCORD_OK);
  CT_CHECK(count == 0 && active == CCORD_NO_ACTIVE_ACCOUNT);
  CT_CHECK(access(token_path, F_OK) != 0);
  CT_CHECK(ccord_live_load_token(nullptr, loaded, sizeof loaded, &err) == CCORD_ERR_NOT_FOUND);
  ccord_live_wipe(loaded, sizeof loaded);

  // Unknown accounts are reported, not silently created.
  CT_CHECK(ccord_live_account_switch("missing", loaded, sizeof loaded, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_live_account_remove("missing", &err) == CCORD_ERR_NOT_FOUND);

  (void)unlink(accounts_path);
  (void)snprintf(slot_path, sizeof slot_path, "%s/ccord/tokens", dir);
  (void)rmdir(slot_path);
  (void)snprintf(slot_path, sizeof slot_path, "%s/ccord", dir);
  (void)rmdir(slot_path);
  (void)rmdir(dir);
  if (saved_xdg[0]) (void)setenv("XDG_CONFIG_HOME", saved_xdg, 1);
  else (void)unsetenv("XDG_CONFIG_HOME");
}

typedef struct voice_capture {
  char frames[8][1024];
  size_t count;
} voice_capture;

static ccord_status capture_send(void *ud, const char *data, size_t len, ccord_error *err)
{
  voice_capture *c = ud;
  ccord_error_clear(err);
  if (c->count >= 8 || len >= sizeof c->frames[0]) return ccord_error_set(err, CCORD_ERR_RANGE, "capture full");
  memcpy(c->frames[c->count], data, len);
  c->frames[c->count][len] = '\0';
  c->count++;
  return CCORD_OK;
}

static uint64_t voice_test_now(void *ud)
{
  return *(uint64_t *)ud;
}

static void test_voice_gateway_builders_and_decode(void)
{
  ccord_error err;
  char url[256];
  CT_CHECK(ccord_voice_build_url("c-dfw09-1.discord.media:443", url, sizeof url, &err) == CCORD_OK);
  CT_CHECK(strcmp(url, "wss://c-dfw09-1.discord.media:443/?v=8") == 0);

  uint8_t ip_pkt[CCORD_VOICE_IP_DISCOVERY_SIZE];
  CT_CHECK(ccord_voice_ip_discovery_build(0x11223344u, ip_pkt) == CCORD_VOICE_IP_DISCOVERY_SIZE);
  CT_CHECK(ip_pkt[0] == 0x00 && ip_pkt[1] == 0x01 && ip_pkt[2] == 0x00 && ip_pkt[3] == 0x46);
  // Turn into a 0x0002 response packet with IP "198.51.100.42" and port 54321
  ip_pkt[1] = 0x02;
  memcpy(ip_pkt + 8, "198.51.100.42", 14);
  ip_pkt[72] = (uint8_t)(54321u >> 8);
  ip_pkt[73] = (uint8_t)(54321u & 0xFFu);
  uint32_t parsed_ssrc = 0;
  char parsed_ip[64] = {0};
  uint16_t parsed_port = 0;
  CT_CHECK(ccord_voice_ip_discovery_parse(ip_pkt, sizeof ip_pkt, &parsed_ssrc, parsed_ip, &parsed_port, &err) ==
           CCORD_OK);
  CT_CHECK(parsed_ssrc == 0x11223344u && strcmp(parsed_ip, "198.51.100.42") == 0 && parsed_port == 54321u);

  ccord_buf b;
  ccord_buf_init(&b, 1024);
  CT_CHECK(ccord_voice_build_identify(77, 42, "sess-1", "REDACTED_TEST_TOKEN", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"op\":0") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"server_id\":\"77\"") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_voice_build_select_protocol("203.0.113.7", 51234, "xsalsa20_poly1305", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"op\":1") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"port\":51234") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_voice_build_heartbeat(&b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"op\":3") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_voice_build_speaking(1234, 1, 0, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"op\":5") != nullptr);
  ccord_buf_clear(&b);

  CT_CHECK(ccord_voice_build_video(1234, 1235, 1236, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"op\":12") != nullptr);
  ccord_buf_free(&b);

  ccord_voice_event ev;
  const char *hello = "{\"op\":8,\"d\":{\"heartbeat_interval\":41250}}";
  CT_CHECK(ccord_voice_gateway_decode(hello, strlen(hello), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.hello && ev.heartbeat_interval_ms == 41250);
  const char *ready =
    "{\"op\":2,\"d\":{\"ssrc\":1234,\"ip\":\"203.0.113.9\",\"port\":50000,"
    "\"modes\":[\"xsalsa20_poly1305\",\"xsalsa20_poly1305_suffix\"]}}";
  CT_CHECK(ccord_voice_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ready && ev.ready_info.ssrc == 1234 && ev.ready_info.port == 50000);
}

static void test_voice_gateway_state_machine(void)
{
  int srv_fd = socket(AF_INET, SOCK_DGRAM, 0);
  CT_CHECK(srv_fd >= 0);
  if (srv_fd < 0) return;
  struct sockaddr_in srv_addr = {0};
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  srv_addr.sin_port = 0;
  CT_CHECK(bind(srv_fd, (struct sockaddr *)&srv_addr, sizeof srv_addr) == 0);
  socklen_t slen = sizeof srv_addr;
  CT_CHECK(getsockname(srv_fd, (struct sockaddr *)&srv_addr, &slen) == 0);
  uint16_t srv_port = ntohs(srv_addr.sin_port);

  voice_capture cap;
  memset(&cap, 0, sizeof cap);
  uint64_t now = 1000;
  ccord_error err;
  ccord_voice_gateway_config cfg = {
    .server_id = 77,
    .user_id = 42,
    .session_id = "voice-session",
    .token = "REDACTED_TEST_TOKEN",
    .endpoint = "voice.example",
    .connect_timeout_ms = 100,
    .now_ms = voice_test_now,
    .clock_ud = &now,
    .send = capture_send,
    .send_ud = &cap,
  };
  ccord_voice_gateway *vg = ccord_voice_gateway_create(&cfg, &err);
  CT_CHECK(vg != nullptr);
  if (!vg) {
    close(srv_fd);
    return;
  }
  CT_CHECK(ccord_voice_gateway_start(vg, &err) == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_CONNECTING);

  ccord_voice_event ev;
  const char *hello = "{\"op\":8,\"d\":{\"heartbeat_interval\":41250}}";
  CT_CHECK(ccord_voice_gateway_handle(vg, hello, strlen(hello), &ev, &err) == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_HELLO);
  CT_CHECK(cap.count == 1 && strstr(cap.frames[0], "\"op\":0") != nullptr);
  CT_CHECK(strstr(cap.frames[0], "\"max_dave_protocol_version\":") != nullptr);

  char ready[256];
  (void)snprintf(ready, sizeof ready,
                 "{\"op\":2,\"s\":7,\"d\":{\"ssrc\":1234,\"ip\":\"127.0.0.1\",\"port\":%u,"
                 "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\",\"aead_aes256_gcm_rtpsize\"]}}",
                 (unsigned)srv_port);
  CT_CHECK(ccord_voice_gateway_handle(vg, ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_READY);
  CT_CHECK(ccord_voice_gateway_ssrc(vg) == 1234);
  // SELECT_PROTOCOL is deferred until the 74-byte 0x0002 IP Discovery response arrives!
  CT_CHECK(cap.count == 1);

  struct sockaddr_in cli_addr = {0};
  socklen_t clen = sizeof cli_addr;
  uint8_t disc_req[CCORD_VOICE_IP_DISCOVERY_SIZE];
  ssize_t rn = recvfrom(srv_fd, disc_req, sizeof disc_req, 0, (struct sockaddr *)&cli_addr, &clen);
  CT_CHECK(rn == (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE);
  CT_CHECK(disc_req[0] == 0x00 && disc_req[1] == 0x01);
  // Reply with 0x0002 external IP "198.51.100.77" port 61234
  disc_req[1] = 0x02;
  memset(disc_req + 8, 0, 64);
  memcpy(disc_req + 8, "198.51.100.77", 13);
  disc_req[72] = (uint8_t)(61234u >> 8);
  disc_req[73] = (uint8_t)(61234u & 0xFFu);
  (void)sendto(srv_fd, disc_req, sizeof disc_req, 0, (const struct sockaddr *)&cli_addr, clen);

  ccord_voice_event polled;
  (void)ccord_voice_gateway_poll(vg, &polled);
  CT_CHECK(cap.count == 2 && strstr(cap.frames[1], "\"op\":1") != nullptr);
  CT_CHECK(strstr(cap.frames[1], "\"address\":\"198.51.100.77\"") != nullptr);
  CT_CHECK(strstr(cap.frames[1], "\"port\":61234") != nullptr);
  CT_CHECK(strstr(cap.frames[1], "\"mode\":\"aead_aes256_gcm_rtpsize\"") != nullptr);

  const char *desc =
    "{\"op\":4,\"s\":8,\"d\":{\"mode\":\"aead_aes256_gcm_rtpsize\",\"dave_protocol_version\":0,\"secret_key\":"
    "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32]}}";
  CT_CHECK(ccord_voice_gateway_handle(vg, desc, strlen(desc), &ev, &err) == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_state(vg) == CCORD_VOICE_ESTABLISHED);

  int16_t tx_pcm[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
  for (size_t i = 0; i < sizeof tx_pcm / sizeof tx_pcm[0]; i++) tx_pcm[i] = (int16_t)((int)(i % 200) - 100);
  CT_CHECK(ccord_voice_gateway_push_pcm(vg, tx_pcm, CCORD_VOICE_FRAME_SAMPLES, &err) == CCORD_OK);
  // Pushing 20ms of PCM triggers Op 5 SPEAKING and sends an encrypted RTP+Opus UDP packet!
  CT_CHECK(cap.count == 3 && strstr(cap.frames[2], "\"op\":5") != nullptr);
  CT_CHECK(ccord_voice_gateway_self_speaking(vg));

  uint8_t udp_pkt[1500];
  ssize_t un = recvfrom(srv_fd, udp_pkt, sizeof udp_pkt, 0, nullptr, nullptr);
  CT_CHECK(un > (ssize_t)CCORD_VOICE_RTP_HEADER_SIZE + 16 + 4);
  // Echo the encrypted RTP+Opus packet back to the voice gateway UDP socket for playback pull_pcm!
  (void)sendto(srv_fd, udp_pkt, (size_t)un, 0, (const struct sockaddr *)&cli_addr, clen);

  int16_t rx_pcm[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
  size_t pulled = ccord_voice_gateway_pull_pcm(vg, rx_pcm, CCORD_VOICE_FRAME_SAMPLES);
  CT_CHECK(pulled == CCORD_VOICE_FRAME_SAMPLES);

  // Test H.264 Annex-B video frame RTP packetize + AEAD encrypt -> UDP loopback -> depacketize + poll_video
  const uint8_t h264_idr[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAA,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0x11, 0x22,
  };
  CT_CHECK(ccord_voice_gateway_send_video_op(vg, 1235u, 1236u, &err) == CCORD_OK);
  CT_CHECK(ccord_voice_gateway_send_h264(vg, h264_idr, sizeof h264_idr, 90000u, &err) == CCORD_OK);
  for (;;) {
    ssize_t vn = recvfrom(srv_fd, udp_pkt, sizeof udp_pkt, MSG_DONTWAIT, nullptr, nullptr);
    if (vn <= 0) break;
    (void)sendto(srv_fd, udp_pkt, (size_t)vn, 0, (const struct sockaddr *)&cli_addr, clen);
  }
  const uint8_t *rx_h264 = nullptr;
  size_t rx_h264_len = 0;
  bool rx_keyframe = false;
  CT_CHECK(ccord_voice_gateway_poll_video(vg, &rx_h264, &rx_h264_len, &rx_keyframe));
  CT_CHECK(rx_keyframe && rx_h264_len == sizeof h264_idr);
  CT_CHECK(memcmp(rx_h264, h264_idr, sizeof h264_idr) == 0);

  ccord_voice_gateway_destroy(vg);
  close(srv_fd);
}

// ---- per-user camera receive (voice_camera.h): loopback fake SFU ------------------------------------------------
typedef struct cam_rig {
  voice_capture cap;
  uint64_t now;
  int srv_fd;
  struct sockaddr_in cli;
  socklen_t clen;
  ccord_voice_gateway *vg;
  uint32_t counter;
  uint32_t seq_ssrc[16];
  uint16_t seq_next[16];
  uint8_t key[CCORD_VOICE_KEY_BYTES];
} cam_rig;

static bool cam_rig_open(cam_rig *r, bool camera_rx)
{
  memset(r, 0, sizeof *r);
  r->now = 1000;
  r->clen = sizeof r->cli;
  for (size_t i = 0; i < sizeof r->key; i++) r->key[i] = (uint8_t)(i + 1u);
  r->srv_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (r->srv_fd < 0) return false;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t sl = sizeof a;
  if (bind(r->srv_fd, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(r->srv_fd, (struct sockaddr *)&a, &sl) != 0) {
    return false;
  }
  ccord_error err;
  ccord_voice_gateway_config cfg = {
    .server_id = 77,
    .user_id = 42,
    .session_id = "voice-session",
    .token = "REDACTED_TEST_TOKEN",
    .endpoint = "voice.example",
    .connect_timeout_ms = 100,
    .now_ms = voice_test_now,
    .clock_ud = &r->now,
    .send = capture_send,
    .send_ud = &r->cap,
  };
  r->vg = ccord_voice_gateway_create(&cfg, &err);
  if (!r->vg) return false;
  if (camera_rx) ccord_voice_gateway_set_camera_rx(r->vg, true);
  ccord_voice_event ev;
  const char *hello = "{\"op\":8,\"d\":{\"heartbeat_interval\":41250}}";
  char ready[256];
  (void)snprintf(ready, sizeof ready,
                 "{\"op\":2,\"s\":7,\"d\":{\"ssrc\":1234,\"ip\":\"127.0.0.1\",\"port\":%u,"
                 "\"modes\":[\"aead_xchacha20_poly1305_rtpsize\"]}}",
                 (unsigned)ntohs(a.sin_port));
  const char *desc =
    "{\"op\":4,\"s\":8,\"d\":{\"mode\":\"aead_xchacha20_poly1305_rtpsize\",\"dave_protocol_version\":0,\"secret_key\":"
    "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32]}}";
  bool ok = ccord_voice_gateway_start(r->vg, &err) == CCORD_OK &&
            ccord_voice_gateway_handle(r->vg, hello, strlen(hello), &ev, &err) == CCORD_OK &&
            ccord_voice_gateway_handle(r->vg, ready, strlen(ready), &ev, &err) == CCORD_OK;
  uint8_t disc[CCORD_VOICE_IP_DISCOVERY_SIZE];
  ok = ok && recvfrom(r->srv_fd, disc, sizeof disc, 0, (struct sockaddr *)&r->cli, &r->clen) ==
               (ssize_t)CCORD_VOICE_IP_DISCOVERY_SIZE;
  if (!ok) return false;
  disc[1] = 0x02;
  memset(disc + 8, 0, 64);
  memcpy(disc + 8, "198.51.100.77", 13);
  disc[72] = (uint8_t)(61234u >> 8);
  disc[73] = (uint8_t)(61234u & 0xFFu);
  (void)sendto(r->srv_fd, disc, sizeof disc, 0, (const struct sockaddr *)&r->cli, r->clen);
  (void)ccord_voice_gateway_poll(r->vg, &ev);
  return ccord_voice_gateway_handle(r->vg, desc, strlen(desc), &ev, &err) == CCORD_OK &&
         ccord_voice_gateway_state(r->vg) == CCORD_VOICE_ESTABLISHED;
}

static void cam_rig_close(cam_rig *r)
{
  ccord_voice_gateway_destroy(r->vg);
  if (r->srv_fd >= 0) close(r->srv_fd);
}

static void cam_rig_op12(cam_rig *r, unsigned user, uint32_t video_ssrc)
{
  char js[192];
  (void)snprintf(js, sizeof js,
                 "{\"op\":12,\"d\":{\"user_id\":\"%u\",\"audio_ssrc\":%u,\"video_ssrc\":%u,\"rtx_ssrc\":%u}}", user,
                 user + 1000u, video_ssrc, video_ssrc ? video_ssrc + 1000u : 0u);
  ccord_voice_event ev;
  ccord_error err;
  CT_CHECK(ccord_voice_gateway_handle(r->vg, js, strlen(js), &ev, &err) == CCORD_OK);
}

static void cam_rig_op13(cam_rig *r, unsigned user)
{
  char js[96];
  (void)snprintf(js, sizeof js, "{\"op\":13,\"d\":{\"user_id\":\"%u\"}}", user);
  ccord_voice_event ev;
  ccord_error err;
  CT_CHECK(ccord_voice_gateway_handle(r->vg, js, strlen(js), &ev, &err) == CCORD_OK);
}

static void cam_rig_send_rtp(cam_rig *r, uint32_t ssrc, uint32_t ts, bool marker, const uint8_t *payload, size_t len)
{
  size_t slot = 0;
  while (slot < 16 && r->seq_ssrc[slot] != 0 && r->seq_ssrc[slot] != ssrc) slot++;
  if (slot == 16) return;
  if (r->seq_ssrc[slot] == 0) {
    r->seq_ssrc[slot] = ssrc;
    r->seq_next[slot] = 100;
  }
  uint8_t hdr[CCORD_RTP_FIXED_HEADER];
  CT_CHECK(ccord_rtp_write_header(hdr, sizeof hdr, CCORD_RTP_H264_PT, marker, r->seq_next[slot]++, ts, ssrc) ==
           sizeof hdr);
  uint8_t pkt[2048];
  size_t n = 0;
  ccord_error err;
  CT_CHECK(ccord_voice_seal(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, r->key, ++r->counter, hdr, sizeof hdr, payload, len,
                            pkt, sizeof pkt, &n, &err) == CCORD_OK);
  (void)sendto(r->srv_fd, pkt, n, 0, (const struct sockaddr *)&r->cli, r->clen);
}

typedef struct cam_pkts {
  uint8_t data[8][1300];
  size_t len[8];
  bool marker[8];
  size_t n;
} cam_pkts;

static bool cam_collect(void *ud, const uint8_t *payload, size_t len, bool marker)
{
  cam_pkts *p = ud;
  if (p->n >= 8 || len > sizeof p->data[0]) return false;
  memcpy(p->data[p->n], payload, len);
  p->len[p->n] = len;
  p->marker[p->n] = marker;
  p->n++;
  return true;
}

// SPS + PPS + one slice NAL of `nal_len` bytes filled with `fill` (no 00 00 01 inside, so it survives start-code
// scanning). key=true: IDR slice (type 5); else a P slice (type 1).
static size_t cam_make_au(uint8_t *out, uint8_t fill, size_t nal_len, bool key)
{
  static const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1E, 0xAA};
  static const uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80};
  size_t n = 0;
  if (key) {
    memcpy(out, sps, sizeof sps);
    n += sizeof sps;
    memcpy(out + n, pps, sizeof pps);
    n += sizeof pps;
  }
  static const uint8_t sc[] = {0, 0, 0, 1};
  memcpy(out + n, sc, sizeof sc);
  n += sizeof sc;
  out[n++] = key ? 0x65 : 0x41;
  memset(out + n, fill, nal_len);
  return n + nal_len;
}

static size_t cam_packetize(cam_pkts *p, const uint8_t *au, size_t len)
{
  memset(p, 0, sizeof *p);
  ccord_error err;
  CT_CHECK(ccord_rtp_h264_packetize(au, len, 1100u, cam_collect, p, nullptr, &err) == CCORD_OK);
  return p->n;
}

static void cam_send_au(cam_rig *r, uint32_t ssrc, uint32_t ts, const uint8_t *au, size_t len)
{
  cam_pkts p;
  (void)cam_packetize(&p, au, len);
  for (size_t i = 0; i < p.n; i++) cam_rig_send_rtp(r, ssrc, ts, p.marker[i], p.data[i], p.len[i]);
}

// Opens every PLI the fake SFU received; ssrcs[] gets the media SSRC of each one, in order.
static size_t cam_rig_drain_pli(cam_rig *r, uint32_t *ssrcs, size_t max)
{
  size_t found = 0;
  for (;;) {
    uint8_t pkt[256];
    ssize_t n = recvfrom(r->srv_fd, pkt, sizeof pkt, MSG_DONTWAIT, nullptr, nullptr);
    if (n <= 0) break;
    if (n < 8 + 4 + 16 + 4 || pkt[0] != 0x81 || pkt[1] != 206 || pkt[2] != 0 || pkt[3] != 2) {
      CT_CHECK(false);  // the gateway sends nothing else here
      continue;
    }
    size_t sealed = (size_t)n - 8u - CCORD_VOICE_NONCE_SUFFIX_BYTES;
    const uint8_t *sfx = pkt + n - CCORD_VOICE_NONCE_SUFFIX_BYTES;
    uint32_t counter = ((uint32_t)sfx[0] << 24) | ((uint32_t)sfx[1] << 16) | ((uint32_t)sfx[2] << 8) | sfx[3];
    uint8_t nonce[24];
    (void)ccord_voice_nonce(CCORD_VOICE_MODE_XCHACHA20_RTPSIZE, counter, nonce);
    uint8_t plain[16];
    unsigned long long mlen = 0;
    int rc = sealed <= sizeof plain + CCORD_VOICE_TAG_BYTES
               ? crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &mlen, nullptr, pkt + 8, sealed, pkt, 8, nonce,
                                                            r->key)
               : -1;
    CT_CHECK(rc == 0 && mlen == 4);
    uint32_t sender = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
    CT_CHECK(sender == 1234u);  // our own audio SSRC
    if (rc == 0 && found < max) {
      ssrcs[found++] = ((uint32_t)plain[0] << 24) | ((uint32_t)plain[1] << 16) | ((uint32_t)plain[2] << 8) | plain[3];
    }
  }
  return found;
}

static size_t cam_count_ssrc(const uint32_t *v, size_t n, uint32_t ssrc)
{
  size_t c = 0;
  for (size_t i = 0; i < n; i++) c += v[i] == ssrc;
  return c;
}

static bool cam_poll(cam_rig *r, unsigned user, uint8_t *buf, size_t cap, size_t *len, bool *key)
{
  return ccord_voice_gateway_poll_camera(r->vg, user, buf, cap, len, nullptr, key);
}

static size_t cam_op15_count(const cam_rig *r)
{
  size_t n = 0;
  for (size_t i = 0; i < r->cap.count; i++) {
    if (strcmp(r->cap.frames[i], "{\"op\":15,\"d\":{\"any\":100}}") == 0) n++;
  }
  return n;
}

static void test_gateway_multi_camera(void)
{
  static cam_rig r;
  CT_CHECK(cam_rig_open(&r, true));
  if (!r.vg) return;
  ccord_voice_event ev;
  CT_CHECK(cam_op15_count(&r) == 1);  // sink wants, sent once at SESSION_DESCRIPTION

  // Five senders announce a camera: four are admitted, the fifth waits.
  for (unsigned i = 0; i < 5; i++) cam_rig_op12(&r, 2001u + i, 5001u + i);
  ccord_voice_cam_info info[CCORD_VOICE_CAM_MAX];
  CT_CHECK(ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX) == 5);
  for (unsigned i = 0; i < 5; i++) {
    CT_CHECK(info[i].user_id == 2001u + i && info[i].video_ssrc == 5001u + i);
    CT_CHECK(info[i].decoding == (i < CCORD_VOICE_CAM_DECODE_MAX));
    CT_CHECK(info[i].generation != 0);
  }
  uint32_t pli[32];
  size_t npli = cam_rig_drain_pli(&r, pli, 32);
  CT_CHECK(npli == 4);  // one keyframe request per admitted sender, none for the waiter
  for (uint32_t s = 5001; s <= 5004; s++) CT_CHECK(cam_count_ssrc(pli, npli, s) == 1);

  // Two senders' FU-A fragments interleaved packet by packet: each user must get its own intact access unit. A
  // single shared depacketizer would see a sequence gap / interleaved fragments and deliver neither.
  uint8_t au_a[4096], au_b[4096];
  size_t la = cam_make_au(au_a, 0x11, 3000, true), lb = cam_make_au(au_b, 0x22, 3000, true);
  cam_pkts pa, pb;
  CT_CHECK(cam_packetize(&pa, au_a, la) >= 3 && cam_packetize(&pb, au_b, lb) >= 3);
  for (size_t i = 0; i < pa.n || i < pb.n; i++) {
    if (i < pa.n) cam_rig_send_rtp(&r, 5001, 90000, pa.marker[i], pa.data[i], pa.len[i]);
    if (i < pb.n) cam_rig_send_rtp(&r, 5002, 90000, pb.marker[i], pb.data[i], pb.len[i]);
  }
  static uint8_t out[CCORD_VOICE_CAM_AU_MAX];
  size_t out_len = 0;
  bool key = false;
  CT_CHECK(cam_poll(&r, 2001, out, sizeof out, &out_len, &key));
  CT_CHECK(key && out_len == la && memcmp(out, au_a, la) == 0);
  CT_CHECK(cam_poll(&r, 2002, out, sizeof out, &out_len, &key));
  CT_CHECK(key && out_len == lb && memcmp(out, au_b, lb) == 0);
  CT_CHECK(!cam_poll(&r, 2001, out, sizeof out, &out_len, &key));
  ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX);
  CT_CHECK(!info[0].need_keyframe && !info[1].need_keyframe && info[2].need_keyframe);

  // The waiting fifth sender and RTX packets produce nothing.
  uint8_t idr[64];
  size_t li = cam_make_au(idr, 0x33, 20, true);
  cam_send_au(&r, 5005, 91000, idr, li);
  cam_send_au(&r, 6001, 91000, idr, li);  // RTX SSRC of sender 1
  CT_CHECK(!cam_poll(&r, 2005, out, sizeof out, &out_len, &key));
  CT_CHECK(!cam_poll(&r, 2001, out, sizeof out, &out_len, &key));

  // An admitted sender leaves: the waiter is promoted (fresh generation, keyframe request) and now yields frames.
  uint32_t gen5 = info[4].generation;
  cam_rig_op13(&r, 2003);
  CT_CHECK(ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX) == 4);
  CT_CHECK(info[3].user_id == 2005 && info[3].decoding && info[3].generation != gen5);
  for (size_t i = 0; i < 4; i++) CT_CHECK(info[i].decoding);
  npli = cam_rig_drain_pli(&r, pli, 32);
  CT_CHECK(npli == 1 && pli[0] == 5005u);
  cam_send_au(&r, 5005, 92000, idr, li);
  CT_CHECK(cam_poll(&r, 2005, out, sizeof out, &out_len, &key));
  CT_CHECK(key && out_len == li && memcmp(out, idr, li) == 0);
  CT_CHECK(!cam_poll(&r, 2003, out, sizeof out, &out_len, &key));  // gone

  // Ring overflow: nobody polls for 5 access units -> flush, wait for the next IDR, ask for one (once a second).
  r.now += 2000;
  for (uint32_t i = 0; i < 5; i++) cam_send_au(&r, 5001, 93000 + i, idr, li);
  CT_CHECK(!cam_poll(&r, 2001, out, sizeof out, &out_len, &key));
  ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX);
  CT_CHECK(info[0].user_id == 2001 && info[0].need_keyframe);
  npli = cam_rig_drain_pli(&r, pli, 32);
  CT_CHECK(cam_count_ssrc(pli, npli, 5001) == 1);
  // Other senders that never got an IDR (sender 4) keep their own once-a-second requests; sender 1 stays quiet
  // while it is inside its 1 s window, however often the gateway is polled.
  for (int i = 0; i < 3; i++) (void)ccord_voice_gateway_poll(r.vg, &ev);
  npli = cam_rig_drain_pli(&r, pli, 32);
  CT_CHECK(cam_count_ssrc(pli, npli, 5001) == 0);
  r.now += 1001;
  (void)ccord_voice_gateway_poll(r.vg, &ev);
  (void)ccord_voice_gateway_poll(r.vg, &ev);
  npli = cam_rig_drain_pli(&r, pli, 32);
  CT_CHECK(cam_count_ssrc(pli, npli, 5001) == 1);  // and again after it, still exactly one
  uint8_t pslice[64];
  size_t lp = cam_make_au(pslice, 0x44, 20, false);
  cam_send_au(&r, 5001, 94000, pslice, lp);
  CT_CHECK(!cam_poll(&r, 2001, out, sizeof out, &out_len, &key));  // gated: its reference is missing
  cam_send_au(&r, 5001, 95000, idr, li);
  cam_send_au(&r, 5001, 96000, pslice, lp);
  CT_CHECK(cam_poll(&r, 2001, out, sizeof out, &out_len, &key) && key && out_len == li);
  CT_CHECK(cam_poll(&r, 2001, out, sizeof out, &out_len, &key) && !key && out_len == lp);
  ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX);
  CT_CHECK(!info[0].need_keyframe);

  // Camera off (OP 12 video_ssrc 0) frees the slot; the OP 15 text went out exactly once overall.
  cam_rig_op12(&r, 2002, 0);
  CT_CHECK(ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX) == 3);
  CT_CHECK(cam_op15_count(&r) == 1);
  cam_rig_close(&r);
}

static void test_gateway_camera_idle_demotion(void)
{
  static cam_rig r;
  CT_CHECK(cam_rig_open(&r, true));
  if (!r.vg) return;
  ccord_voice_event ev;
  for (unsigned i = 0; i < 5; i++) cam_rig_op12(&r, 2001u + i, 5001u + i);
  uint8_t idr[64];
  size_t li = cam_make_au(idr, 0x55, 20, true);
  uint8_t out[256];
  size_t out_len = 0;
  bool key = false;
  r.now += 3000;
  for (uint32_t i = 0; i < 3; i++) {
    cam_send_au(&r, 5001 + i, 90000, idr, li);
    CT_CHECK(cam_poll(&r, 2001 + i, out, sizeof out, &out_len, &key));
  }
  // Sender 4 never sent anything. After 5 s of silence its slot goes to the waiting sender 5, not back to itself.
  r.now += 2500;
  (void)ccord_voice_gateway_poll(r.vg, &ev);
  ccord_voice_cam_info info[CCORD_VOICE_CAM_MAX];
  CT_CHECK(ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX) == 5);
  CT_CHECK(info[3].user_id == 2004 && !info[3].decoding);
  CT_CHECK(info[4].user_id == 2005 && info[4].decoding);
  for (size_t i = 0; i < 3; i++) CT_CHECK(info[i].decoding);

  // A sixth sender arrives and waits. When a sender leaves, that waiter goes ahead of the idle-demoted one.
  cam_rig_op12(&r, 2006, 5006);
  cam_rig_op13(&r, 2001);
  CT_CHECK(ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX) == 5);
  for (size_t i = 0; i < 5; i++) {
    if (info[i].user_id == 2006) CT_CHECK(info[i].decoding);
    if (info[i].user_id == 2004) CT_CHECK(!info[i].decoding);
  }
  // With no other waiter left the demoted sender does get its turn.
  cam_rig_op13(&r, 2002);
  ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX);
  for (size_t i = 0; i < 4; i++) CT_CHECK(info[i].decoding);
  cam_rig_close(&r);
}

static void test_gateway_camera_legacy_path(void)
{
  static cam_rig r;
  CT_CHECK(cam_rig_open(&r, false));
  if (!r.vg) return;
  cam_rig_op12(&r, 2001, 5001);
  ccord_voice_cam_info info[CCORD_VOICE_CAM_MAX];
  CT_CHECK(ccord_voice_gateway_cameras(r.vg, info, CCORD_VOICE_CAM_MAX) == 0);  // per-user table is off
  CT_CHECK(cam_op15_count(&r) == 0);
  // The single-slot path still reassembles an access unit for poll_video.
  uint8_t idr[64];
  size_t li = cam_make_au(idr, 0x66, 20, true);
  cam_send_au(&r, 5001, 90000, idr, li);
  const uint8_t *rx = nullptr;
  size_t rx_len = 0;
  bool key = false;
  CT_CHECK(ccord_voice_gateway_poll_video(r.vg, &rx, &rx_len, &key));
  CT_CHECK(key && rx_len == li && memcmp(rx, idr, li) == 0);
  cam_rig_close(&r);
}

static void test_voice_media_framing(void)
{
  ccord_error err;
  ccord_voice_codec *codec = ccord_voice_codec_create(&err);
  CT_CHECK(codec != nullptr);
  if (!codec) return;
  int16_t pcm[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
  for (size_t i = 0; i < sizeof pcm / sizeof pcm[0]; i++) pcm[i] = (int16_t)((int)(i % 400) - 200);
  uint8_t opus[512];
  size_t opus_len = 0;
  CT_CHECK(ccord_voice_encode(codec, pcm, CCORD_VOICE_FRAME_SAMPLES, opus, sizeof opus, &opus_len, &err) == CCORD_OK);
  CT_CHECK(opus_len > 0);

  uint8_t rtp[CCORD_VOICE_RTP_HEADER_SIZE];
  CT_CHECK(ccord_voice_rtp_header(rtp, sizeof rtp, 7, 960, 1234) == CCORD_VOICE_RTP_HEADER_SIZE);
  CT_CHECK(rtp[0] == 0x80 && rtp[2] == 0 && rtp[3] == 7);

  uint8_t key[CCORD_VOICE_MAX_SECRET];
  for (size_t i = 0; i < sizeof key; i++) key[i] = (uint8_t)(i + 1);
  uint8_t packet[640];
  size_t packet_len = 0;
  CT_CHECK(ccord_voice_encrypt(key, rtp, sizeof rtp, opus, opus_len, packet, sizeof packet, &packet_len, &err) ==
           CCORD_OK);
  CT_CHECK(packet_len == sizeof rtp + opus_len + 16);

  uint8_t plain[512];
  size_t plain_len = 0;
  CT_CHECK(ccord_voice_decrypt(key, packet, packet_len, sizeof rtp, plain, sizeof plain, &plain_len, &err) == CCORD_OK);
  CT_CHECK(plain_len == opus_len && memcmp(plain, opus, opus_len) == 0);

  int16_t decoded[CCORD_VOICE_FRAME_SAMPLES * CCORD_VOICE_CHANNELS];
  size_t decoded_samples = 0;
  CT_CHECK(ccord_voice_decode(codec, plain, plain_len, decoded, CCORD_VOICE_FRAME_SAMPLES, &decoded_samples, &err) ==
           CCORD_OK);
  CT_CHECK(decoded_samples == CCORD_VOICE_FRAME_SAMPLES);
  ccord_voice_codec_destroy(codec);
}

static void test_json_search_upload_and_dm(void)
{
  ccord_error err;
  ccord_live_event ev;
  const char *search_json =
    "{\"messages\":[[{\"id\":\"501\",\"channel_id\":\"22\",\"author\":{\"id\":\"42\"},"
    "\"content\":\"needle in haystack\",\"timestamp\":\"2026-11-08T00:00:00Z\"}]]}";
  CT_CHECK(ccord_live_json_search(search_json, strlen(search_json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SEARCH_RESULTS && ev.message_count == 1);
  CT_CHECK(ev.messages[0].id == 501 && strcmp(ev.messages[0].content, "needle in haystack") == 0);
  ccord_live_event_free(&ev);

  const char *slot_json =
    "{\"attachments\":[{\"id\":0,\"upload_url\":\"https://upload.example/put\",\"upload_filename\":\"up_0_pic.png\"}]}";
  char uurl[256], ufn[256];
  CT_CHECK(ccord_live_json_upload_slot(slot_json, strlen(slot_json), uurl, sizeof uurl, ufn, sizeof ufn, &err) ==
           CCORD_OK);
  CT_CHECK(strcmp(uurl, "https://upload.example/put") == 0);
  CT_CHECK(strcmp(ufn, "up_0_pic.png") == 0);

  const char *dm_json =
    "{\"id\":\"9001\",\"type\":1,\"recipients\":[{\"id\":\"43\",\"username\":\"Ada\",\"global_name\":\"Ada L\",\"avatar\":\"avhash\"}]}";
  CT_CHECK(ccord_live_json_channel(dm_json, strlen(dm_json), CCORD_DM_GUILD_ID, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_UPSERT && ev.ev.channel != nullptr);
  CT_CHECK(ev.ev.channel->guild_id == CCORD_DM_GUILD_ID && strcmp(ev.ev.channel->name, "Ada L") == 0);
  CT_CHECK(ev.ev.channel->user_id == 43 && ev.ev.channel->recipient_id == 43);
  CT_CHECK(strstr(ev.ev.channel->icon_url, "avatars/43/avhash.png") != nullptr);
  ccord_live_event_free(&ev);

  char tok[128], ticket[128];
  const char *auth_ok = "{\"token\":\"REDACTED_TEST_TOKEN\"}";
  CT_CHECK(ccord_live_json_auth_response(auth_ok, strlen(auth_ok), 200, tok, sizeof tok, ticket, sizeof ticket, &err) ==
           CCORD_OK);
  CT_CHECK(strcmp(tok, "REDACTED_TEST_TOKEN") == 0 && ticket[0] == '\0');
  ccord_live_wipe(tok, sizeof tok);

  const char *auth_mfa = "{\"token\":null,\"mfa\":true,\"ticket\":\"mfa_ticket_xyz\"}";
  CT_CHECK(ccord_live_json_auth_response(auth_mfa, strlen(auth_mfa), 200, tok, sizeof tok, ticket, sizeof ticket, &err) ==
           CCORD_OK);
  CT_CHECK(tok[0] == '\0' && strcmp(ticket, "mfa_ticket_xyz") == 0);

  const char *auth_err = "{\"message\":\"Login or password is invalid.\"}";
  CT_CHECK(ccord_live_json_auth_response(auth_err, strlen(auth_err), 400, tok, sizeof tok, ticket, sizeof ticket, &err) ==
           CCORD_ERR_PERMISSION);
}

static void test_ready_v2_properties_permissions_sorting_and_dms(void)
{
  ccord_live_event ev;
  ccord_error err;
  // READY v2 payload:
  // - Guild 100 uses guild.properties for name, icon, owner_id, roles.
  // - Self user 42 has role 200 (no ADMINISTRATOR). @everyone (id=100) has VIEW_CHANNEL (1024).
  // - Uncategorized channels: voice pos 0 (id=19), text pos 2 (id=18), text pos 1 (id=17).
  // - Category 12 (pos 2) and Category 10 (pos 1):
  //   Under Category 10: voice pos 0 (id=15), text pos 1 (id=14), announcement(5) pos 0 (id=13),
  //   hidden text channel (id=16, @everyone deny 1024), role-allowed text channel (id=20, @everyone deny 1024, role 200 allow 1024).
  //   Under Category 12: forum(15) pos 0 (id=21), stage(13) pos 0 (id=22).
  // - private_channels uses recipient_ids resolved against d.users[] (preferring global_name) and sorted by last_message_id desc.
  const char *ready =
    "{\"op\":0,\"t\":\"READY\",\"d\":{"
    "\"user\":{\"id\":\"42\",\"username\":\"selfuser\",\"global_name\":\"Self Display\"},"
    "\"users\":["
    "{\"id\":\"501\",\"username\":\"alice_u\",\"global_name\":\"Alice G\",\"avatar\":\"alice_av\"},"
    "{\"id\":\"502\",\"username\":\"bob_u\",\"avatar\":\"bob_av\"}"
    "],"
    "\"guilds\":[{\"id\":\"100\","
    "\"properties\":{\"name\":\"Nested V2 Guild\",\"icon\":\"g_icon_hash\",\"owner_id\":\"999\","
    "\"roles\":[{\"id\":\"100\",\"name\":\"@everyone\",\"permissions\":\"1024\",\"position\":0},"
    "{\"id\":\"200\",\"name\":\"Trusted\",\"permissions\":\"0\",\"color\":16711680,\"position\":3}]},"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},\"roles\":[\"200\"]}],"
    "\"channels\":["
    "{\"id\":\"12\",\"type\":4,\"name\":\"Cat Two\",\"position\":2},"
    "{\"id\":\"10\",\"type\":4,\"name\":\"Cat One\",\"position\":1},"
    "{\"id\":\"19\",\"type\":2,\"name\":\"Uncat Voice\",\"position\":0},"
    "{\"id\":\"18\",\"type\":0,\"name\":\"uncat-b\",\"position\":2},"
    "{\"id\":\"17\",\"type\":0,\"name\":\"uncat-a\",\"position\":1},"
    "{\"id\":\"15\",\"type\":2,\"name\":\"Cat1 Voice\",\"parent_id\":\"10\",\"position\":0},"
    "{\"id\":\"14\",\"type\":0,\"name\":\"cat1-general\",\"parent_id\":\"10\",\"position\":1},"
    "{\"id\":\"13\",\"type\":5,\"name\":\"cat1-news\",\"parent_id\":\"10\",\"position\":0},"
    "{\"id\":\"16\",\"type\":0,\"name\":\"secret-staff\",\"parent_id\":\"10\",\"position\":0,"
    "\"permission_overwrites\":[{\"id\":\"100\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
    "{\"id\":\"20\",\"type\":0,\"name\":\"trusted-lounge\",\"parent_id\":\"10\",\"position\":2,"
    "\"permission_overwrites\":[{\"id\":\"100\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"},"
    "{\"id\":\"200\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]},"
    "{\"id\":\"21\",\"type\":15,\"name\":\"cat2-forum\",\"parent_id\":\"12\",\"position\":0},"
    "{\"id\":\"22\",\"type\":13,\"name\":\"cat2-stage\",\"parent_id\":\"12\",\"position\":0}"
    "]}],"
    "\"presences\":[{\"user\":{\"id\":\"501\"},\"status\":\"dnd\"}],"
    "\"private_channels\":["
    "{\"id\":\"801\",\"type\":1,\"recipient_ids\":[\"502\"],\"last_message_id\":\"1000\"},"
    "{\"id\":\"802\",\"type\":1,\"recipient_ids\":[\"501\"],\"last_message_id\":\"2500\"}"
    "]}}";
  CT_CHECK(ccord_live_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ccord_snapshot_validate(&ev.snapshot, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.guild_count == 1);
  CT_CHECK(strcmp(ev.snapshot.guilds[0].name, "Nested V2 Guild") == 0);
  CT_CHECK(strstr(ev.snapshot.guilds[0].icon_url, "icons/100/g_icon_hash.png") != nullptr);
  CT_CHECK(ev.snapshot.member_count == 1);
  CT_CHECK(strcmp(ev.snapshot.members[0].role_name, "Trusted") == 0);

  // Verify hidden channel 16 is filtered out and guild channels are in exact display order:
  // Uncategorized: 17 (text pos 1), 18 (text pos 2), 19 (voice pos 0)
  // Cat One (10, pos 1): 13 (text pos 0), 14 (text pos 1), 20 (text pos 2), 15 (voice pos 0)
  // Cat Two (12, pos 2): 21 (forum->text pos 0), 22 (stage->voice pos 0)
  const ccord_id expected_guild_order[] = {17, 18, 19, 10, 13, 14, 20, 15, 12, 21, 22};
  size_t g_idx = 0;
  size_t dm_idx = 0;
  const ccord_channel *dm_chans[2] = {nullptr, nullptr};
  for (size_t i = 0; i < ev.snapshot.channel_count; i++) {
    const ccord_channel *c = &ev.snapshot.channels[i];
    CT_CHECK(c->id != 16);
    if (c->guild_id == 100) {
      CT_CHECK(g_idx < sizeof(expected_guild_order) / sizeof(expected_guild_order[0]));
      if (g_idx < sizeof(expected_guild_order) / sizeof(expected_guild_order[0])) {
        CT_CHECK(c->id == expected_guild_order[g_idx]);
      }
      g_idx++;
    } else if (c->guild_id == CCORD_DM_GUILD_ID) {
      if (dm_idx < 2) dm_chans[dm_idx] = c;
      dm_idx++;
    }
  }
  CT_CHECK(g_idx == sizeof(expected_guild_order) / sizeof(expected_guild_order[0]));
  CT_CHECK(dm_idx == 2);
  if (dm_idx == 2) {
    // 802 has last_message_id 2500 > 801's 1000, so 802 comes first.
    CT_CHECK(dm_chans[0]->id == 802);
    CT_CHECK(strcmp(dm_chans[0]->name, "Alice G") == 0);
    CT_CHECK(dm_chans[0]->user_id == 501);
    CT_CHECK(strstr(dm_chans[0]->icon_url, "avatars/501/alice_av.png") != nullptr);
    CT_CHECK(dm_chans[1]->id == 801);
    CT_CHECK(strcmp(dm_chans[1]->name, "bob_u") == 0);
    CT_CHECK(dm_chans[1]->user_id == 502);
    CT_CHECK(strstr(dm_chans[1]->icon_url, "avatars/502/bob_av.png") != nullptr);
  }
  // Permission data rides on the same snapshot: the role table, our own role ids (the member list is lazy) and each
  // channel's overwrites, so the permission engine has something exact to work from.
  CT_CHECK(ev.ev.role_count == 2 && ev.ev.roles[0].guild_id == 100);
  CT_CHECK(ev.ev.roles[1].id == 200 && strcmp(ev.ev.roles[1].name, "Trusted") == 0 && ev.ev.roles[1].color == 16711680u &&
           ev.ev.roles[1].position == 3 && ev.ev.roles[1].permissions == 0 && !ev.ev.roles[1].managed);
  CT_CHECK(ev.ev.roles[0].id == 100 && ev.ev.roles[0].permissions == 1024);
  CT_CHECK(ev.ev.member_role_count == 1 && ev.ev.member_roles[0].guild_id == 100 && ev.ev.member_roles[0].user_id == 42 &&
           ev.ev.member_roles[0].count == 1 && ev.ev.member_roles[0].role_ids[0] == 200);
  size_t with_ow = 0;
  for (size_t i = 0; i < ev.ev.overwrite_count; i++) {
    const ccord_channel_overwrites *o = &ev.ev.overwrites[i];
    if (o->channel_id == 20) {
      CT_CHECK(o->count == 2 && o->items[0].id == 100 && o->items[0].kind == CCORD_OVERWRITE_ROLE && o->items[0].deny == 1024 &&
               o->items[1].id == 200 && o->items[1].allow == 1024 && o->items[1].deny == 0);
      with_ow++;
    } else if (o->channel_id == 16) {
      CT_CHECK(o->count == 1 && o->items[0].deny == 1024);
      with_ow++;
    }
  }
  CT_CHECK(with_ow == 2);
  ccord_live_event_free(&ev);
}

static void test_ready_merged_members_and_message_edge_cases(void)
{
  ccord_live_event ev;
  ccord_error err;
  // Gateway v9 READY where guild.members is omitted and self roles live in d.merged_members[guild_idx],
  // while user_settings.guild_positions reorders guild 200 before guild 100.
  const char *ready =
    "{\"op\":0,\"t\":\"READY\",\"d\":{"
    "\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"user_settings\":{\"guild_positions\":[\"200\",\"100\"]},"
    "\"guilds\":["
    "{\"id\":\"100\",\"name\":\"FirstInArray\","
    "\"roles\":[{\"id\":\"100\",\"name\":\"@everyone\",\"permissions\":\"0\"},"
    "{\"id\":\"150\",\"name\":\"MemberRole\",\"permissions\":\"1024\",\"position\":1}],"
    "\"channels\":[{\"id\":\"1001\",\"type\":0,\"name\":\"members-only\",\"position\":0}]},"
    "{\"id\":\"200\",\"name\":\"SecondInArray\","
    "\"roles\":[{\"id\":\"200\",\"name\":\"@everyone\",\"permissions\":\"1024\"}],"
    "\"channels\":[{\"id\":\"2001\",\"type\":0,\"name\":\"public-general\",\"position\":0}]}"
    "],"
    "\"merged_members\":["
    "[{\"user_id\":\"42\",\"roles\":[\"150\"]}],"
    "[{\"user_id\":\"42\",\"roles\":[]}]"
    "]}}";
  CT_CHECK(ccord_live_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ccord_snapshot_validate(&ev.snapshot, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.guild_count == 2);
  // Ordered by user_settings.guild_positions: 200 then 100.
  CT_CHECK(ev.snapshot.guilds[0].id == 200);
  CT_CHECK(ev.snapshot.guilds[1].id == 100);
  // Guild 100's channel 1001 is visible only because merged_members[0] granted role 150 (VIEW_CHANNEL=1024).
  bool saw_1001 = false;
  for (size_t i = 0; i < ev.snapshot.channel_count; i++) {
    if (ev.snapshot.channels[i].id == 1001) saw_1001 = true;
  }
  CT_CHECK(saw_1001);
  // Verify READY merged_members also seeds the member entries into snapshot.members for both guilds!
  CT_CHECK(ev.snapshot.member_count == 2);
  bool saw_m100 = false, saw_m200 = false;
  for (size_t i = 0; i < ev.snapshot.member_count; i++) {
    if (ev.snapshot.members[i].guild_id == 100 && ev.snapshot.members[i].user_id == 42) {
      saw_m100 = true;
      CT_CHECK(strcmp(ev.snapshot.members[i].role_name, "MemberRole") == 0);
    }
    if (ev.snapshot.members[i].guild_id == 200 && ev.snapshot.members[i].user_id == 42) {
      saw_m200 = true;
      CT_CHECK(strcmp(ev.snapshot.members[i].role_name, "Members") == 0);
    }
  }
  CT_CHECK(saw_m100 && saw_m200);
  ccord_live_event_free(&ev);

  // Test Opcode 14 LAZY_REQUEST builder with channel range subscription and guild-level fallback:
  ccord_buf lazy_buf;
  ccord_buf_init(&lazy_buf, 256);
  CT_CHECK(ccord_gateway_build_lazy_request_channel(100, 1001, &lazy_buf, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&lazy_buf), "\"op\":14") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&lazy_buf), "\"guild_id\":\"100\"") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&lazy_buf), "\"channels\":{\"1001\":[[0,99]]}") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&lazy_buf), "\"typing\":true") != nullptr);
  ccord_buf_clear(&lazy_buf);
  CT_CHECK(ccord_gateway_build_lazy_request_channel(100, CCORD_ID_NONE, &lazy_buf, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&lazy_buf), "\"channels\":{}") != nullptr);
  // More of the list: up to three hundreds whole; beyond that the top and the two newest (a request replaces the last).
  const struct { uint32_t chunks; const char *ranges; } asks[] = {
    {0, "[[0,99]]"}, {1, "[[0,99]]"}, {2, "[[0,99],[100,199]]"}, {3, "[[0,99],[100,199],[200,299]]"},
    {4, "[[0,99],[200,299],[300,399]]"}, {7, "[[0,99],[500,599],[600,699]]"},
    {50, "[[0,99],[4800,4899],[4900,4999]]"}, {51, "[[0,99],[4800,4899],[4900,4999]]"}, {UINT32_MAX, "[[0,99],[4800,4899],[4900,4999]]"},
  };
  for (size_t i = 0; i < sizeof asks / sizeof asks[0]; i++) {
    ccord_buf_clear(&lazy_buf);
    CT_CHECK(ccord_gateway_build_lazy_request_chunks(100, 1001, asks[i].chunks, &lazy_buf, &err) == CCORD_OK);
    char want[96];
    snprintf(want, sizeof want, "\"channels\":{\"1001\":%s}", asks[i].ranges);
    CT_CHECK(strstr(ccord_buf_cstr(&lazy_buf), want) != nullptr);
    uint32_t r[3][2];
    size_t nr = ccord_gateway_member_ranges(asks[i].chunks, r);
    CT_CHECK(nr >= 1 && nr <= 3 && r[0][0] == 0 && r[0][1] == 99);
    for (size_t k = 0; k < nr; k++) CT_CHECK(r[k][1] - r[k][0] == 99 && (k == 0 || r[k][0] > r[k - 1][1]));
  }
  ccord_buf_clear(&lazy_buf);
  CT_CHECK(ccord_gateway_build_lazy_request_chunks(CCORD_DM_GUILD_ID, 1001, 2, &lazy_buf, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_lazy_request_chunks(CCORD_ID_NONE, 1001, 2, &lazy_buf, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_gateway_build_lazy_request_chunks(100, 1001, 2, nullptr, &err) == CCORD_ERR_INVALID);
  ccord_buf_free(&lazy_buf);

  // Test GUILD_MEMBER_LIST_UPDATE decoding (SYNC items[] and INSERT/UPDATE item, with roles, nick, and presence):
  const char *gmlu_json =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{"
    "\"guild_id\":\"100\",\"id\":\"everyone\","
    "\"ops\":["
    "{\"op\":\"SYNC\",\"range\":[0,99],\"items\":["
    "{\"group\":{\"id\":\"150\",\"count\":1}},"
    "{\"member\":{\"user\":{\"id\":\"501\",\"username\":\"alice_u\",\"global_name\":\"Alice Global\",\"avatar\":\"av501\"},"
    "\"nick\":\"Alice Nick\",\"roles\":[\"150\"],"
    "\"presence\":{\"status\":\"dnd\",\"activities\":[{\"type\":4,\"state\":\"Coding in C\"}]}}}"
    "]},"
    "{\"op\":\"INSERT\",\"index\":2,\"item\":{"
    "\"member\":{\"user\":{\"id\":\"502\",\"username\":\"bob_u\",\"global_name\":\"Bob Global\"},"
    "\"roles\":[],\"presence\":{\"status\":\"online\"}}}}"
    "]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_json, strlen(gmlu_json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS);
  CT_CHECK(ev.ev.guild_id == 100);
  CT_CHECK(ev.member_count == 2 && ev.user_count == 2);
  if (ev.member_count == 2 && ev.user_count == 2) {
    CT_CHECK(ev.members[0].guild_id == 100 && ev.members[0].user_id == 501);
    CT_CHECK(strcmp(ev.members[0].role_name, "MemberRole") == 0);
    CT_CHECK(ev.users[0].id == 501 && strcmp(ev.users[0].name, "Alice Nick") == 0);
    CT_CHECK(ev.users[0].presence == CCORD_PRESENCE_DND);
    CT_CHECK(strcmp(ev.users[0].status_text, "Coding in C") == 0);
    CT_CHECK(ev.members[1].guild_id == 100 && ev.members[1].user_id == 502);
    CT_CHECK(strcmp(ev.members[1].role_name, "Members") == 0);
    CT_CHECK(ev.users[1].id == 502 && strcmp(ev.users[1].name, "Bob Global") == 0);
    CT_CHECK(ev.users[1].presence == CCORD_PRESENCE_ONLINE);
  }
  CT_CHECK(ev.ev.member_total == 0 && ev.ev.member_online == 0 && ev.ev.member_group_count == 0 && ev.ev.member_groups == nullptr);
  CT_CHECK(ev.ev.member_synced_to == 100);   // the SYNC covered positions 0..99
  ccord_live_event_free(&ev);

  // The size of the whole list and of each group in it comes with the update (the list itself arrives a range at a time).
  const char *gmlu_sizes =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{"
    "\"guild_id\":\"100\",\"id\":\"everyone\",\"member_count\":1234,\"online_count\":56,"
    "\"groups\":[{\"id\":\"150\",\"count\":1},{\"id\":\"online\",\"count\":55},{\"id\":\"offline\",\"count\":1178}],"
    "\"ops\":[{\"op\":\"SYNC\",\"range\":[0,99],\"items\":[{\"group\":{\"id\":\"150\",\"count\":1}},"
    "{\"member\":{\"user\":{\"id\":\"501\",\"username\":\"alice_u\"},\"roles\":[\"150\"],\"presence\":{\"status\":\"online\"}}}]}]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_sizes, strlen(gmlu_sizes), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.guild_id == 100 && ev.ev.guild_member_count == 1);
  CT_CHECK(ev.ev.member_total == 1234 && ev.ev.member_online == 56 && ev.ev.member_group_count == 3);
  if (ev.ev.member_group_count == 3) {
    CT_CHECK(ev.ev.member_groups[0].kind == CCORD_MEMBER_GROUP_ROLE && ev.ev.member_groups[0].role_id == 150 &&
             ev.ev.member_groups[0].count == 1);
    CT_CHECK(ev.ev.member_groups[1].kind == CCORD_MEMBER_GROUP_ONLINE && ev.ev.member_groups[1].count == 55);
    CT_CHECK(ev.ev.member_groups[2].kind == CCORD_MEMBER_GROUP_OFFLINE && ev.ev.member_groups[2].count == 1178);
  }
  ccord_live_event_free(&ev);
  // What cannot be read is not told: a zero total, a count that is not a number, a group without an id or count or with a negative count.
  const char *gmlu_odd =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{"
    "\"guild_id\":\"100\",\"id\":\"everyone\",\"member_count\":0,\"online_count\":\"many\","
    "\"groups\":[{\"count\":4},{\"id\":\"online\"},{\"id\":\"online\",\"count\":-1},{\"id\":\"nonsense\",\"count\":2},{\"id\":\"offline\",\"count\":7}],"
    "\"ops\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_odd, strlen(gmlu_odd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_total == 0 && ev.ev.member_online == 0);
  CT_CHECK(ev.ev.member_group_count == 1 && ev.ev.member_groups[0].kind == CCORD_MEMBER_GROUP_OFFLINE &&
           ev.ev.member_groups[0].count == 7);
  ccord_live_event_free(&ev);
  const char *gmlu_all_bad_groups =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"100\",\"groups\":[{\"id\":\"nonsense\",\"count\":2}],\"ops\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_all_bad_groups, strlen(gmlu_all_bad_groups), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_group_count == 0 && ev.ev.member_groups == nullptr);
  ccord_live_event_free(&ev);
  const char *gmlu_empty =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"100\",\"groups\":[],\"online_count\":0,\"ops\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_empty, strlen(gmlu_empty), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_total == 0 && ev.ev.member_online == 0 && ev.ev.member_group_count == 0);   // nobody is online: told, and zero
  CT_CHECK(ev.ev.member_synced_to == 0);   // no SYNC in it
  ccord_live_event_free(&ev);
  // Numbers beyond what a count can be are not told (not wrapped into small ones).
  const char *gmlu_huge =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"100\",\"member_count\":5000000000,\"online_count\":-3,"
    "\"groups\":[{\"id\":\"online\",\"count\":5000000000},{\"id\":\"offline\",\"count\":4}],\"ops\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_huge, strlen(gmlu_huge), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_total == 0 && ev.ev.member_online == 0);
  CT_CHECK(ev.ev.member_group_count == 1 && ev.ev.member_groups[0].kind == CCORD_MEMBER_GROUP_OFFLINE);
  ccord_live_event_free(&ev);
  const char *gmlu_huge_online =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"100\",\"online_count\":5000000000,\"ops\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_huge_online, strlen(gmlu_huge_online), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_online == 0);
  ccord_live_event_free(&ev);
  // A group list as long as a server can have roles (each hoisted, plus online and offline) is read; a longer one is not.
  {
    static char big[24 * 1024];
    for (size_t groups = CCORD_MAX_ROLES_PER_GUILD + 2u; groups <= CCORD_MAX_ROLES_PER_GUILD + 3u; groups++) {
      size_t len = (size_t)snprintf(big, sizeof big, "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"100\",\"member_count\":9,\"groups\":[");
      for (size_t i = 0; i < groups; i++) {
        len += (size_t)snprintf(big + len, sizeof big - len, "%s{\"id\":\"%zu\",\"count\":1}", i ? "," : "", 1000 + i);
      }
      len += (size_t)snprintf(big + len, sizeof big - len, "],\"ops\":[]}}");
      CT_CHECK(len < sizeof big);
      CT_CHECK(ccord_live_gateway_decode(big, len, &ev, &err) == CCORD_OK);
      CT_CHECK(ev.ev.member_total == 9);
      CT_CHECK(groups == CCORD_MAX_ROLES_PER_GUILD + 2u ? ev.ev.member_group_count == groups : ev.ev.member_group_count == 0);
      ccord_live_event_free(&ev);
    }
  }
  // How far the list has been delivered is the end of the furthest SYNC (they answer a request for ranges; the order does not matter).
  const char *gmlu_sync3 =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"100\",\"ops\":["
    "{\"op\":\"SYNC\",\"range\":[200,299],\"items\":[]},{\"op\":\"SYNC\",\"range\":[0,99],\"items\":[]},"
    "{\"op\":\"SYNC\",\"range\":[100,149],\"items\":[]}]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_sync3, strlen(gmlu_sync3), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_synced_to == 300);
  ccord_live_event_free(&ev);
  // INSERT, UPDATE and DELETE are changes to what is held, not deliveries; a SYNC with a range that makes no sense is no delivery.
  const char *gmlu_nosync =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"100\",\"ops\":["
    "{\"op\":\"INSERT\",\"index\":250,\"range\":[0,500],\"item\":{\"member\":{\"user\":{\"id\":\"601\",\"username\":\"u601\"}}}},"
    "{\"op\":\"UPDATE\",\"index\":260,\"item\":{\"member\":{\"user\":{\"id\":\"602\",\"username\":\"u602\"}}}},"
    "{\"op\":\"DELETE\",\"index\":270},"
    "{\"op\":\"SYNC\",\"range\":[50,10],\"items\":[]},{\"op\":\"SYNC\",\"range\":[-5,-1],\"items\":[]},"
    "{\"op\":\"SYNC\",\"range\":[-5,3],\"items\":[]},{\"op\":\"SYNC\",\"range\":[7],\"items\":[]},"
    "{\"op\":\"SYNC\",\"range\":[0,99,200],\"items\":[]},"
    "{\"op\":\"SYNC\",\"range\":[\"a\",\"b\"],\"items\":[]},{\"op\":\"SYNC\",\"items\":[]}]}}";
  CT_CHECK(ccord_live_gateway_decode(gmlu_nosync, strlen(gmlu_nosync), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_synced_to == 0 && ev.member_count == 2);
  ccord_live_event_free(&ev);

  // Test guild MESSAGE_CREATE carrying d.guild_id and d.member attaches resolved member to the event:
  const char *msg_with_member =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{"
    "\"id\":\"7001\",\"channel_id\":\"1001\",\"guild_id\":\"100\","
    "\"author\":{\"id\":\"503\",\"username\":\"carol_u\",\"global_name\":\"Carol\"},"
    "\"member\":{\"nick\":\"Carol Mod\",\"roles\":[\"150\"]},"
    "\"content\":\"hello guild\",\"timestamp\":\"2026-11-08T00:00:00Z\"}}";
  CT_CHECK(ccord_live_gateway_decode(msg_with_member, strlen(msg_with_member), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE);
  CT_CHECK(ev.ev.guild_id == 100);
  CT_CHECK(ev.member_count == 1 && ev.ev.guild_member_count == 1);
  if (ev.member_count == 1) {
    CT_CHECK(ev.members[0].guild_id == 100 && ev.members[0].user_id == 503);
    CT_CHECK(strcmp(ev.members[0].role_name, "MemberRole") == 0);
    CT_CHECK(strcmp(ev.members[0].nick, "Carol Mod") == 0);   // the nickname rides along, so seeing the author does not clear it
  }
  CT_CHECK(ev.user_count == 1 && strcmp(ev.users[0].name, "Carol Mod") == 0);
  ccord_live_event_free(&ev);
  const char *msg_no_nick =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{"
    "\"id\":\"7002\",\"channel_id\":\"1001\",\"guild_id\":\"100\","
    "\"author\":{\"id\":\"503\",\"username\":\"carol_u\",\"global_name\":\"Carol\"},"
    "\"member\":{\"nick\":null,\"roles\":[\"150\"]},"
    "\"content\":\"again\",\"timestamp\":\"2026-11-08T00:00:01Z\"}}";
  CT_CHECK(ccord_live_gateway_decode(msg_no_nick, strlen(msg_no_nick), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].nick[0] == '\0');
  ccord_live_event_free(&ev);
  // A nickname longer than any Discord allows is not copied (and cannot overrun the member's field).
  char long_nick[200];
  memset(long_nick, 'n', sizeof long_nick - 1);
  long_nick[sizeof long_nick - 1] = '\0';
  char msg_long_nick[1024];
  snprintf(msg_long_nick, sizeof msg_long_nick,
           "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{"
           "\"id\":\"7003\",\"channel_id\":\"1001\",\"guild_id\":\"100\","
           "\"author\":{\"id\":\"503\",\"username\":\"carol_u\",\"global_name\":\"Carol\"},"
           "\"member\":{\"nick\":\"%s\",\"roles\":[\"150\"]},"
           "\"content\":\"long\",\"timestamp\":\"2026-11-08T00:00:02Z\"}}", long_nick);
  CT_CHECK(ccord_live_gateway_decode(msg_long_nick, strlen(msg_long_nick), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].nick[0] == '\0');
  ccord_live_event_free(&ev);

  // History JSON with CRLF/CR/control chars, sticker, forwarded snapshot, system message, and missing author.id:
  const char *hist_json =
    "["
    "{\"id\":\"904\",\"channel_id\":\"2001\",\"type\":7,\"content\":\"\"},"
    "{\"id\":\"903\",\"channel_id\":\"2001\",\"webhook_id\":\"888\",\"author\":{\"username\":\"DeployHook\"},"
    "\"content\":\"\",\"message_snapshots\":[{\"message\":{\"content\":\"hi\"}}]},"
    "{\"id\":\"902\",\"channel_id\":\"2001\",\"author\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"content\":\"\",\"sticker_items\":[{\"id\":\"1\",\"name\":\"WumpusWave\"}]},"
    "{\"id\":\"901\",\"channel_id\":\"2001\",\"author\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"content\":\"line1\\r\\nline2\\rline3\\b\\f\\u007f\\u0085\"}"
    "]";
  CT_CHECK(ccord_live_json_history(hist_json, strlen(hist_json), 2001, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_HISTORY);
  CT_CHECK(ev.message_count == 4);
  for (size_t i = 0; i < ev.message_count; i++) {
    CT_CHECK(ccord_message_validate(&ev.messages[i], &err) == CCORD_OK);
  }
  // Text is left empty: a system message, a forward and a sticker are shown from their structure, not a placeholder.
  CT_CHECK(ev.messages[0].content[0] == '\0' && ev.messages[0].rich && ev.messages[0].rich->type == 7);
  CT_CHECK(ev.messages[0].author_id == 904);
  CT_CHECK(ev.messages[1].content[0] == '\0' && ev.messages[1].rich && ev.messages[1].rich->forward.present &&
           strcmp(ev.messages[1].rich->forward.content, "hi") == 0);
  CT_CHECK(ev.messages[1].author_id == 888);
  CT_CHECK(ev.messages[2].content[0] == '\0' && ev.messages[2].rich && ev.messages[2].rich->sticker_count == 1 &&
           strcmp(ev.messages[2].rich->stickers[0].name, "WumpusWave") == 0 && ev.messages[2].rich->stickers[0].format == 0);
  CT_CHECK(strcmp(ev.messages[3].content, "line1\nline2\nline3") == 0);
  CT_CHECK(ev.user_count >= 2);
  ccord_live_event_free(&ev);

  // Oversized UTF-8 message (> 4000 bytes with multi-byte codepoints) clamps cleanly on a UTF-8 boundary.
  ccord_buf big;
  ccord_buf_init(&big, 65536);
  CT_CHECK(ccord_buf_append_str(&big, "[{\"id\":\"999\",\"channel_id\":\"2001\",\"author\":{\"id\":\"42\"},\"content\":\"") == CCORD_OK);
  for (size_t i = 0; i < 1500; i++) {
    CT_CHECK(ccord_buf_append_str(&big, "\xe2\x9c\x85") == CCORD_OK); // 3-byte UTF-8 check mark, 4500 bytes total
  }
  CT_CHECK(ccord_buf_append_str(&big, "\"}]") == CCORD_OK);
  CT_CHECK(ccord_live_json_history(ccord_buf_cstr(&big), big.len, 2001, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1);
  CT_CHECK(strlen(ev.messages[0].content) <= CCORD_MESSAGE_MAX_BYTES);
  CT_CHECK(ccord_message_validate(&ev.messages[0], &err) == CCORD_OK);
  ccord_live_event_free(&ev);
  ccord_buf_free(&big);
}

static void test_guild_folders_proto_dm_read_state_and_user_profile(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. d.user_settings.guild_folders ordering + read_state DM handling:
  //    - read_state does NOT overwrite an authoritative private_channels last_message_id (DM 801 keeps 1000 < DM 802's 5000)
  //    - non-channel read_state_type != 0 and sentinel ack IDs are ignored
  //    - when private_channels has last_message_id: null (DM 803), a valid channel read_state (read_state_type == 0) supplies fallback
  const char *ready_folders =
    "{\"op\":0,\"t\":\"READY\",\"d\":{"
    "\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"user_settings\":{\"guild_folders\":[{\"guild_ids\":[\"300\",\"100\"]},{\"guild_ids\":[\"200\"]}]},"
    "\"guilds\":["
    "{\"id\":\"100\",\"name\":\"G100\",\"channels\":[],\"members\":[]},"
    "{\"id\":\"200\",\"name\":\"G200\",\"channels\":[],\"members\":[]},"
    "{\"id\":\"300\",\"name\":\"G300\",\"channels\":[],\"members\":[]}"
    "],"
    "\"read_state\":["
    "{\"id\":\"801\",\"last_message_id\":\"9000\"},"
    "{\"id\":\"802\",\"read_state_type\":2,\"last_message_id\":\"99999\"},"
    "{\"id\":\"803\",\"read_state_type\":0,\"last_message_id\":\"9000\"},"
    "{\"id\":\"804\",\"read_state_type\":0,\"last_message_id\":\"18446744073709551615\"}"
    "],"
    "\"private_channels\":["
    "{\"id\":\"801\",\"type\":1,\"last_message_id\":\"1000\",\"recipients\":[{\"id\":\"501\",\"username\":\"ada\"}]},"
    "{\"id\":\"802\",\"type\":1,\"last_message_id\":\"5000\",\"recipients\":[{\"id\":\"502\",\"username\":\"bob\"}]},"
    "{\"id\":\"803\",\"type\":1,\"last_message_id\":null,\"recipients\":[{\"id\":\"503\",\"username\":\"cara\"}]},"
    "{\"id\":\"804\",\"type\":1,\"last_message_id\":null,\"recipients\":[{\"id\":\"504\",\"username\":\"dan\"}]}"
    "]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_folders, strlen(ready_folders), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.guild_count == 3);
  CT_CHECK(ev.snapshot.guilds[0].id == 300);
  CT_CHECK(ev.snapshot.guilds[1].id == 100);
  CT_CHECK(ev.snapshot.guilds[2].id == 200);
  // DM 803 (fallback 9000) > DM 802 (authoritative 5000) > DM 801 (authoritative 1000, not overwritten by ack 9000) > DM 804 (id=804, sentinel ignored).
  CT_CHECK(ev.snapshot.channel_count == 4);
  CT_CHECK(ev.snapshot.channels[0].id == 803 && ev.snapshot.channels[0].last_message_id == 9000);
  CT_CHECK(ev.snapshot.channels[1].id == 802 && ev.snapshot.channels[1].last_message_id == 5000);
  CT_CHECK(ev.snapshot.channels[2].id == 801 && ev.snapshot.channels[2].last_message_id == 1000);
  CT_CHECK(ev.snapshot.channels[3].id == 804 && ev.snapshot.channels[3].last_message_id == 0);
  // The read position follows the same rules: only a channel read state with a sane id counts.
  CT_CHECK(ev.snapshot.channels[0].last_read_id == 9000);   // 803
  CT_CHECK(ev.snapshot.channels[1].last_read_id == CCORD_ID_NONE);   // 802: read_state_type 2 is not a channel
  CT_CHECK(ev.snapshot.channels[2].last_read_id == 9000);   // 801: the position is the ack even where the newest message is authoritative
  CT_CHECK(ev.snapshot.channels[3].last_read_id == CCORD_ID_NONE);   // 804: sentinel ack ids are ignored
  ccord_live_event_free(&ev);

  // 1b. READY with > 1000 private_channels sorts all entries before truncating to CCORD_MAX_CHANNELS_PER_GUILD (1000)
  //     and resolves recipient_ids against d.users.
  {
    ccord_buf jb;
    ccord_buf_init(&jb, 262144);
    (void)ccord_buf_append_str(&jb,
      "{\"op\":0,\"t\":\"READY\",\"d\":{"
      "\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
      "\"users\":["
      "{\"id\":\"7001\",\"username\":\"recent_one\",\"global_name\":\"Recent One\",\"avatar\":\"av7001\"},"
      "{\"id\":\"7002\",\"username\":\"recent_two\",\"global_name\":\"Recent Two\",\"avatar\":\"av7002\"}"
      "],"
      "\"guilds\":[],"
      "\"private_channels\":[");
    for (size_t i = 0; i < 1050; i++) {
      if (i > 0) (void)ccord_buf_append_str(&jb, ",");
      if (i == 1048) {
        (void)ccord_buf_append_str(&jb,
          "{\"id\":\"91048\",\"type\":1,\"last_message_id\":\"900000\",\"recipient_ids\":[\"7001\"]}");
      } else if (i == 1049) {
        (void)ccord_buf_append_str(&jb,
          "{\"id\":\"91049\",\"type\":1,\"last_message_id\":\"950000\",\"recipient_ids\":[\"42\",\"7002\"]}");
      } else {
        char entry[160];
        unsigned long cid = 10000ul + (unsigned long)i;
        unsigned long lmid = 20000ul + (unsigned long)i;
        unsigned long uid = 50000ul + (unsigned long)i;
        int n = snprintf(entry, sizeof entry,
                         "{\"id\":\"%lu\",\"type\":1,\"last_message_id\":\"%lu\","
                         "\"recipients\":[{\"id\":\"%lu\",\"username\":\"u%lu\"}]}",
                         cid, lmid, uid, (unsigned long)i);
        if (n > 0) (void)ccord_buf_append(&jb, entry, (size_t)n);
      }
    }
    (void)ccord_buf_append_str(&jb, "]}}");
    CT_CHECK(ccord_live_gateway_decode(ccord_buf_cstr(&jb), jb.len, &ev, &err) == CCORD_OK);
    CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
    CT_CHECK(ev.snapshot.channel_count == CCORD_MAX_CHANNELS_PER_GUILD);
    CT_CHECK(ev.snapshot.channels[0].id == 91049 && ev.snapshot.channels[0].last_message_id == 950000);
    CT_CHECK(strcmp(ev.snapshot.channels[0].name, "Recent Two") == 0);
    CT_CHECK(ev.snapshot.channels[0].user_id == 7002);
    CT_CHECK(strstr(ev.snapshot.channels[0].icon_url, "avatars/7002/av7002.png") != nullptr);
    CT_CHECK(ev.snapshot.channels[1].id == 91048 && ev.snapshot.channels[1].last_message_id == 900000);
    CT_CHECK(strcmp(ev.snapshot.channels[1].name, "Recent One") == 0);
    CT_CHECK(ev.snapshot.channels[1].user_id == 7001);
    CT_CHECK(ev.snapshot.channels[2].id == 11047 && ev.snapshot.channels[2].last_message_id == 21047);
    ccord_live_event_free(&ev);
    ccord_buf_free(&jb);
  }

  // 2. d.user_settings_proto base64-encoded PreloadedUserSettings.guild_folders ordering ([300, 200], then remaining 100)
  const char *ready_proto =
    "{\"op\":0,\"t\":\"READY\",\"d\":{"
    "\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"user_settings_proto\":\"cggKBgisAgjIAQ==\","
    "\"guilds\":["
    "{\"id\":\"100\",\"name\":\"G100\",\"channels\":[],\"members\":[]},"
    "{\"id\":\"200\",\"name\":\"G200\",\"channels\":[],\"members\":[]},"
    "{\"id\":\"300\",\"name\":\"G300\",\"channels\":[],\"members\":[]}"
    "],"
    "\"private_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_proto, strlen(ready_proto), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.guild_count == 3);
  CT_CHECK(ev.snapshot.guilds[0].id == 300);
  CT_CHECK(ev.snapshot.guilds[1].id == 200);
  CT_CHECK(ev.snapshot.guilds[2].id == 100);
  ccord_live_event_free(&ev);

  // 3. ccord_live_json_user_profile decoding GET /api/v9/users/<id>/profile
  const char *profile_json =
    "{\"user\":{\"id\":\"501\",\"username\":\"ada_lovelace\",\"global_name\":\"Ada Lovelace\","
    "\"avatar\":\"av_hash\",\"banner\":\"bn_hash\",\"accent_color\":3447003},"
    "\"user_profile\":{\"bio\":\"First programmer\",\"pronouns\":\"she/her\","
    "\"banner\":\"bn_prof_hash\",\"accent_color\":15158332}}";
  CT_CHECK(ccord_live_json_user_profile(profile_json, strlen(profile_json), 501, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_USER_PROFILE);
  CT_CHECK(ev.ev.user_profile != nullptr);
  CT_CHECK(ev.ev.user_profile->user_id == 501);
  CT_CHECK(strcmp(ev.ev.user_profile->display_name, "Ada Lovelace") == 0);
  CT_CHECK(strcmp(ev.ev.user_profile->handle, "ada_lovelace") == 0);
  CT_CHECK(strstr(ev.ev.user_profile->avatar_url, "avatars/501/av_hash.png") != nullptr);
  CT_CHECK(strstr(ev.ev.user_profile->banner_url, "banners/501/bn_prof_hash.png") != nullptr);
  CT_CHECK(ev.ev.user_profile->accent_color == 15158332u);
  CT_CHECK(strcmp(ev.ev.user_profile->bio, "First programmer") == 0);
  CT_CHECK(strcmp(ev.ev.user_profile->pronouns, "she/her") == 0);
  ccord_live_event_free(&ev);

  // 4. Group DM user_id isolation, 1:1 DM deduplication, and server nickname isolation from global DM user names:
  const char *ready_gdm_dedup =
    "{\"op\":0,\"t\":\"READY\",\"d\":{"
    "\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"guilds\":[{\"id\":\"100\",\"name\":\"SharedGuild\",\"channels\":[],"
    "\"members\":[{\"user\":{\"id\":\"501\",\"username\":\"pat_u\",\"global_name\":\"Real Pat\"},\"nick\":\"ServerNickPat\"}]}],"
    "\"private_channels\":["
    "{\"id\":\"9001\",\"type\":1,\"last_message_id\":\"1000\",\"recipients\":[{\"id\":\"501\",\"username\":\"pat_u\",\"global_name\":\"Real Pat\"}]},"
    "{\"id\":\"9002\",\"type\":1,\"last_message_id\":\"5000\",\"recipients\":[{\"id\":\"501\",\"username\":\"pat_u\",\"global_name\":\"Real Pat\"}]},"
    "{\"id\":\"9003\",\"type\":3,\"last_message_id\":\"8000\",\"name\":null,"
    "\"recipients\":[{\"id\":\"501\",\"username\":\"pat_u\",\"global_name\":\"Real Pat\"},{\"id\":\"502\",\"username\":\"sam_u\",\"global_name\":\"Sam\"}]}"
    "]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_gdm_dedup, strlen(ready_gdm_dedup), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  // Duplicate 1:1 DMs (9001 and 9002 for user 501) are deduplicated to 9002 (higher last_message_id), plus Group DM 9003 -> 2 channels total.
  CT_CHECK(ev.snapshot.channel_count == 2);
  if (ev.snapshot.channel_count == 2) {
    // Group DM 9003 is first (last_message_id 8000), has user_id == 0, discord_type == 3, and comma-separated recipient names.
    CT_CHECK(ev.snapshot.channels[0].id == 9003);
    CT_CHECK(ev.snapshot.channels[0].discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM);
    CT_CHECK(ev.snapshot.channels[0].user_id == CCORD_ID_NONE);
    CT_CHECK(ev.snapshot.channels[0].recipient_count == 2);
    CT_CHECK(strcmp(ev.snapshot.channels[0].name, "Real Pat, Sam") == 0);
    // 1:1 DM 9002 is second, has user_id == 501, and uses global name "Real Pat" (NOT guild nick "ServerNickPat").
    CT_CHECK(ev.snapshot.channels[1].id == 9002);
    CT_CHECK(ev.snapshot.channels[1].user_id == 501);
    CT_CHECK(strcmp(ev.snapshot.channels[1].name, "Real Pat") == 0);
  }
  // Guild member has nick "ServerNickPat", while global user 501 keeps name "Real Pat".
  CT_CHECK(ev.snapshot.member_count == 1 && strcmp(ev.snapshot.members[0].nick, "ServerNickPat") == 0);
  for (size_t ui = 0; ui < ev.snapshot.user_count; ui++) {
    if (ev.snapshot.users[ui].id == 501) {
      CT_CHECK(strcmp(ev.snapshot.users[ui].name, "Real Pat") == 0);
    }
  }
  ccord_live_event_free(&ev);
}

static void test_forum_json_and_gateway_events(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY with a type-15 forum channel (available_tags, default_sort_order) and an active forum thread in guild.threads:
  //    Verify the forum thread is NOT emitted as a sidebar channel in snapshot.channels, and is emitted in snapshot.forum_posts!
  const char *ready_forum =
    "{\"op\":0,\"t\":\"READY\",\"d\":{"
    "\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"guilds\":[{\"id\":\"100\",\"name\":\"ForumGuild\","
    "\"channels\":["
    "{\"id\":\"500\",\"type\":15,\"name\":\"design-forum\",\"flags\":16,\"default_sort_order\":1,"
    "\"available_tags\":["
    "{\"id\":\"601\",\"name\":\"RFC\",\"emoji_name\":\"\\ud83d\\udcd0\",\"moderated\":false},"
    "{\"id\":\"602\",\"name\":\"Bug\",\"emoji_id\":\"999111\",\"moderated\":true}"
    "]}"
    "],"
    "\"threads\":["
    "{\"id\":\"7001\",\"parent_id\":\"500\",\"guild_id\":\"100\",\"type\":11,\"owner_id\":\"42\","
    "\"name\":\"Pinned Forum Thread\",\"flags\":2,\"message_count\":5,"
    "\"applied_tags\":[\"601\",\"602\"],"
    "\"thread_metadata\":{\"archived\":false,\"locked\":false,\"create_timestamp\":\"2026-11-08T10:00:00Z\"}}"
    "],"
    "\"members\":[]}],\"private_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_forum, strlen(ready_forum), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.channel_count == 1);
  CT_CHECK(ev.snapshot.channels[0].id == 500 && ev.snapshot.channels[0].discord_type == CCORD_DISCORD_CHANNEL_GUILD_FORUM);
  CT_CHECK(ev.ev.forum_meta_count == 1 && ev.ev.forum_metas != nullptr);
  if (ev.ev.forum_meta_count == 1) {
    CT_CHECK(ev.ev.forum_metas[0].forum_id == 500);
    CT_CHECK(ev.ev.forum_metas[0].require_tag);
    CT_CHECK(ev.ev.forum_metas[0].default_sort == CCORD_FORUM_SORT_CREATION);
    CT_CHECK(ev.ev.forum_metas[0].tag_count == 2);
    CT_CHECK(ev.ev.forum_metas[0].tags[0].id == 601 && strcmp(ev.ev.forum_metas[0].tags[0].name, "RFC") == 0);
    CT_CHECK(ev.ev.forum_metas[0].tags[1].id == 602 && ev.ev.forum_metas[0].tags[1].emoji_id == 999111ULL);
  }
  CT_CHECK(ev.ev.forum_post_count == 1 && ev.ev.forum_posts != nullptr);
  if (ev.ev.forum_post_count == 1) {
    CT_CHECK(ev.ev.forum_posts[0].thread_id == 7001 && ev.ev.forum_posts[0].forum_id == 500);
    CT_CHECK(ev.ev.forum_posts[0].pinned);
    CT_CHECK(ev.ev.forum_posts[0].applied_tag_count == 2);
    CT_CHECK(strcmp(ev.ev.forum_posts[0].title, "Pinned Forum Thread") == 0);
  }
  ccord_live_event_free(&ev);

  // 2. REST GET /channels/{forum_id}/threads/search response (ccord_live_json_forum_posts)
  const char *search_resp =
    "{\"threads\":["
    "{\"id\":\"7002\",\"parent_id\":\"500\",\"guild_id\":\"100\",\"type\":11,\"owner_id\":\"88\","
    "\"name\":\"Search Result Post\",\"message_count\":3,\"applied_tags\":[\"601\"],"
    "\"thread_metadata\":{\"archived\":true,\"locked\":false,\"create_timestamp\":\"2026-11-08T12:00:00Z\"}}"
    "],"
    "\"first_messages\":["
    "{\"id\":\"7002\",\"channel_id\":\"7002\",\"author\":{\"id\":\"88\",\"username\":\"chen_u\",\"global_name\":\"Chen\"},"
    "\"content\":\"Starter excerpt with **markdown**\",\"timestamp\":\"2026-11-08T12:00:00Z\","
    "\"reactions\":[{\"count\":7,\"emoji\":{\"name\":\"\\ud83d\\udd25\"}}]}"
    "],"
    "\"has_more\":true,\"total_results\":42}";
  CT_CHECK(ccord_live_json_forum_posts(search_resp, strlen(search_resp), 500, 0, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_FORUM_POSTS && ev.ev.channel_id == 500);
  CT_CHECK(ev.ev.forum_has_more && ev.ev.forum_total == 42 && ev.ev.forum_offset == 0);
  CT_CHECK(ev.ev.forum_post_count == 1 && ev.ev.forum_posts != nullptr);
  if (ev.ev.forum_post_count == 1) {
    CT_CHECK(ev.ev.forum_posts[0].thread_id == 7002);
    CT_CHECK(ev.ev.forum_posts[0].first_author_id == 88);
    CT_CHECK(strcmp(ev.ev.forum_posts[0].first_content, "Starter excerpt with **markdown**") == 0);
    CT_CHECK(ev.ev.forum_posts[0].reaction_count == 7);
  }
  CT_CHECK(ev.user_count == 1 && ev.users[0].id == 88);
  ccord_live_event_free(&ev);

  // 3. REST POST /channels/{forum_id}/threads response (ccord_live_json_forum_post_created)
  const char *create_resp =
    "{\"id\":\"7003\",\"parent_id\":\"500\",\"guild_id\":\"100\",\"type\":11,\"owner_id\":\"42\","
    "\"name\":\"Created Forum Thread\",\"applied_tags\":[\"601\"],"
    "\"message\":{\"id\":\"7003\",\"channel_id\":\"7003\",\"author\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"content\":\"Initial post body\",\"timestamp\":\"2026-11-09T01:00:00Z\"}}";
  CT_CHECK(ccord_live_json_forum_post_created(create_resp, strlen(create_resp), 500, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_FORUM_POST_CREATED && ev.ev.channel_id == 500);
  CT_CHECK(ev.ev.forum_post_count == 1 && ev.ev.forum_posts[0].thread_id == 7003);
  CT_CHECK(strcmp(ev.ev.forum_posts[0].first_content, "Initial post body") == 0);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 7003);
  ccord_live_event_free(&ev);
}

// Member changes pushed by Discord (and the REST answer to our own admin calls, which has the same shape) must turn
// into the same GUILD_MEMBERS event, carrying the nickname and the timeout end as unix milliseconds.
// Roles, member role ids and channel overwrites from the gateway and REST, as the permission engine needs them.
static void test_gateway_permission_data(void)
{
  ccord_live_event ev;
  ccord_error err;

  // GUILD_CREATE: role table, the roles of the members it lists, and overwrites per channel (member kind, numeric type).
  const char *create =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"d\":{\"id\":\"77\",\"name\":\"HQ\",\"owner_id\":\"5\","
    "\"roles\":[{\"id\":\"77\",\"name\":\"@everyone\",\"permissions\":\"1049600\",\"position\":0},"
    "{\"id\":\"300\",\"name\":\"Mods\",\"permissions\":\"1099511627782\",\"position\":2,\"color\":3447003,"
    "\"hoist\":true,\"mentionable\":true},"
    "{\"id\":\"301\",\"name\":\"Bot\",\"permissions\":\"8\",\"position\":1,\"managed\":true},"
    "{\"id\":\"bad\",\"name\":\"skipped\"}],"
    "\"members\":[{\"user\":{\"id\":\"43\",\"username\":\"Ada\"},\"roles\":[\"300\",\"301\"]},"
    "{\"user\":{\"id\":\"44\",\"username\":\"Bob\"},\"roles\":[]},"
    "{\"user\":{\"id\":\"45\",\"username\":\"Cy\"}}],"
    "\"channels\":[{\"id\":\"11\",\"type\":0,\"name\":\"general\","
    "\"permission_overwrites\":[{\"id\":\"43\",\"type\":1,\"allow\":\"2048\",\"deny\":\"1024\"},"
    "{\"id\":\"300\",\"type\":\"0\",\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"x\",\"type\":0,\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"301\",\"type\":7,\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"302\",\"type\":\"bogus\",\"allow\":\"1\",\"deny\":\"0\"},"
    "{\"id\":\"303\",\"type\":\"member\",\"allow\":\"4\",\"deny\":\"0\"},"
    "{\"id\":\"304\",\"type\":\"role\",\"allow\":\"8\",\"deny\":\"0\"}]},"
    "{\"id\":\"12\",\"type\":0,\"name\":\"open\",\"permission_overwrites\":[]},"
    "{\"id\":\"13\",\"type\":0,\"name\":\"unknown-overwrites\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(create, strlen(create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT);
  CT_CHECK(ev.ev.role_count == 3);                                   // the entry without a usable id is skipped
  CT_CHECK(ev.ev.roles[1].id == 300 && ev.ev.roles[1].guild_id == 77 && ev.ev.roles[1].hoist && ev.ev.roles[1].mentionable &&
           ev.ev.roles[1].permissions == 1099511627782ull && ev.ev.roles[1].color == 3447003u);
  CT_CHECK(ev.ev.roles[2].id == 301 && ev.ev.roles[2].managed);
  CT_CHECK(ev.ev.member_role_count == 2);                            // Cy has no roles key: unknown, not "none"
  CT_CHECK(ev.ev.member_roles[0].user_id == 43 && ev.ev.member_roles[0].count == 2 && ev.ev.member_roles[0].role_ids[1] == 301);
  CT_CHECK(ev.ev.member_roles[1].user_id == 44 && ev.ev.member_roles[1].count == 0);
  CT_CHECK(ev.ev.overwrite_count == 2);                              // #unknown-overwrites has no key: nothing said
  CT_CHECK(ev.ev.overwrites[0].channel_id == 11 && ev.ev.overwrites[0].count == 4);   // bad id and bad types dropped
  CT_CHECK(ev.ev.overwrites[0].items[0].kind == CCORD_OVERWRITE_MEMBER && ev.ev.overwrites[0].items[0].id == 43 &&
           ev.ev.overwrites[0].items[0].allow == 2048 && ev.ev.overwrites[0].items[0].deny == 1024);
  CT_CHECK(ev.ev.overwrites[0].items[1].kind == CCORD_OVERWRITE_ROLE && ev.ev.overwrites[0].items[1].id == 300);
  CT_CHECK(ev.ev.overwrites[0].items[2].kind == CCORD_OVERWRITE_MEMBER && ev.ev.overwrites[0].items[2].id == 303 &&
           ev.ev.overwrites[0].items[3].kind == CCORD_OVERWRITE_ROLE && ev.ev.overwrites[0].items[3].id == 304);
  CT_CHECK(ev.ev.overwrites[1].channel_id == 12 && ev.ev.overwrites[1].count == 0);    // an empty list is a statement
  ccord_live_event_free(&ev);

  // A guild without roles in the payload (e.g. GUILD_UPDATE without them) says nothing about roles.
  const char *update = "{\"op\":0,\"t\":\"GUILD_UPDATE\",\"d\":{\"id\":\"77\",\"name\":\"HQ2\"}}";
  CT_CHECK(ccord_live_gateway_decode(update, strlen(update), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.role_count == 0 && ev.ev.roles == nullptr && ev.ev.member_role_count == 0 && ev.ev.overwrite_count == 0);
  ccord_live_event_free(&ev);

  // GUILD_ROLE_CREATE / UPDATE / DELETE.
  const char *rc = "{\"op\":0,\"t\":\"GUILD_ROLE_CREATE\",\"d\":{\"guild_id\":\"77\",\"role\":{\"id\":\"310\","
                   "\"name\":\"Helpers\",\"permissions\":\"8192\",\"position\":4,\"color\":0}}}";
  CT_CHECK(ccord_live_gateway_decode(rc, strlen(rc), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ROLE_UPSERT && ev.ev.guild_id == 77 && ev.ev.role_id == 310 && ev.ev.role_count == 1);
  CT_CHECK(ev.ev.roles[0].guild_id == 77 && strcmp(ev.ev.roles[0].name, "Helpers") == 0 && ev.ev.roles[0].permissions == 8192);
  ccord_live_event_free(&ev);
  const char *ru = "{\"op\":0,\"t\":\"GUILD_ROLE_UPDATE\",\"d\":{\"guild_id\":\"77\",\"role\":{\"id\":\"310\","
                   "\"name\":\"Helpers+\",\"permissions\":\"8200\",\"position\":5}}}";
  CT_CHECK(ccord_live_gateway_decode(ru, strlen(ru), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ROLE_UPSERT && ev.ev.roles[0].position == 5 && ev.ev.roles[0].permissions == 8200);
  ccord_live_event_free(&ev);
  const char *rd = "{\"op\":0,\"t\":\"GUILD_ROLE_DELETE\",\"d\":{\"guild_id\":\"77\",\"role_id\":\"310\"}}";
  CT_CHECK(ccord_live_gateway_decode(rd, strlen(rd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_ROLE_DELETE && ev.ev.guild_id == 77 && ev.ev.role_id == 310);
  ccord_live_event_free(&ev);
  const char *bad1 = "{\"op\":0,\"t\":\"GUILD_ROLE_CREATE\",\"d\":{\"guild_id\":\"77\",\"role\":{\"name\":\"no id\"}}}";
  CT_CHECK(ccord_live_gateway_decode(bad1, strlen(bad1), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
  const char *bad2 = "{\"op\":0,\"t\":\"GUILD_ROLE_DELETE\",\"d\":{\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(bad2, strlen(bad2), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // A member's roles change (GUILD_MEMBER_UPDATE) and arrive as member role ids; a payload without them says nothing.
  const char *mu = "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\","
                   "\"username\":\"ada\"},\"roles\":[\"300\"]}}";
  CT_CHECK(ccord_live_gateway_decode(mu, strlen(mu), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.member_role_count == 1 && ev.ev.member_roles[0].guild_id == 77 &&
           ev.ev.member_roles[0].user_id == 43 && ev.ev.member_roles[0].count == 1 && ev.ev.member_roles[0].role_ids[0] == 300);
  ccord_live_event_free(&ev);
  const char *mn = "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\","
                   "\"username\":\"ada\"},\"nick\":\"x\"}}";
  CT_CHECK(ccord_live_gateway_decode(mn, strlen(mn), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_role_count == 0);
  ccord_live_event_free(&ev);

  // Member chunks and the lazy member list carry roles too.
  const char *chunk = "{\"op\":0,\"t\":\"GUILD_MEMBERS_CHUNK\",\"d\":{\"guild_id\":\"77\",\"members\":["
                      "{\"user\":{\"id\":\"50\",\"username\":\"u50\"},\"roles\":[\"300\"]},"
                      "{\"user\":{\"id\":\"51\",\"username\":\"u51\"},\"roles\":[]}]}}";
  CT_CHECK(ccord_live_gateway_decode(chunk, strlen(chunk), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_role_count == 2 && ev.ev.member_roles[1].user_id == 51 && ev.ev.member_roles[1].count == 0);
  ccord_live_event_free(&ev);
  const char *list = "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"77\",\"ops\":[{\"op\":\"SYNC\","
                     "\"range\":[0,1],\"items\":[{\"member\":{\"user\":{\"id\":\"60\",\"username\":\"u60\"},"
                     "\"roles\":[\"300\",\"301\"]}},{\"group\":{\"id\":\"online\",\"count\":1}}]}]}}";
  CT_CHECK(ccord_live_gateway_decode(list, strlen(list), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_role_count == 1 && ev.ev.member_roles[0].user_id == 60 && ev.ev.member_roles[0].count == 2);
  ccord_live_event_free(&ev);

  // CHANNEL_UPDATE carries the new overwrites (here: the channel is hidden from @everyone); REST answers do too.
  const char *cu = "{\"op\":0,\"t\":\"CHANNEL_UPDATE\",\"d\":{\"id\":\"11\",\"guild_id\":\"77\",\"type\":0,\"name\":\"general\","
                   "\"permission_overwrites\":[{\"id\":\"77\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(cu, strlen(cu), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_UPSERT && ev.ev.overwrite_count == 1 && ev.ev.overwrites[0].channel_id == 11 &&
           ev.ev.overwrites[0].count == 1 && ev.ev.overwrites[0].items[0].deny == 1024);
  ccord_live_event_free(&ev);
  const char *rest = "{\"id\":\"14\",\"guild_id\":\"77\",\"type\":0,\"name\":\"new\","
                     "\"permission_overwrites\":[{\"id\":\"300\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]}";
  CT_CHECK(ccord_live_json_channel(rest, strlen(rest), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.overwrite_count == 1 && ev.ev.overwrites[0].channel_id == 14 && ev.ev.overwrites[0].items[0].allow == 1024);
  ccord_live_event_free(&ev);
  const char *no_ow = "{\"op\":0,\"t\":\"CHANNEL_UPDATE\",\"d\":{\"id\":\"11\",\"guild_id\":\"77\",\"type\":0,\"name\":\"general\"}}";
  CT_CHECK(ccord_live_gateway_decode(no_ow, strlen(no_ow), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.overwrite_count == 0);
  ccord_live_event_free(&ev);

  // Limits: a role table is capped, and 600 overwrites on a channel are cut to Discord's 500.
  ccord_buf big;
  ccord_buf_init(&big, 65536);
  (void)ccord_buf_append_str(&big, "{\"op\":0,\"t\":\"CHANNEL_UPDATE\",\"d\":{\"id\":\"11\",\"guild_id\":\"77\",\"type\":0,\"name\":\"g\","
                                   "\"permission_overwrites\":[");
  for (int i = 1; i <= 600; i++) (void)ccord_buf_appendf(&big, "%s{\"id\":\"%d\",\"type\":1,\"allow\":\"1\",\"deny\":\"0\"}", i > 1 ? "," : "", 1000 + i);
  (void)ccord_buf_append_str(&big, "]}}");
  CT_CHECK(!big.failed && ccord_live_gateway_decode(ccord_buf_cstr(&big), big.len, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.overwrite_count == 1 && ev.ev.overwrites[0].count == CCORD_MAX_OVERWRITES_PER_CHANNEL);
  ccord_live_event_free(&ev);
  ccord_buf_free(&big);
}

static void test_gateway_member_events(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *upd =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\",\"username\":\"ada\"},"
    "\"nick\":\"Ace\",\"roles\":[],\"communication_disabled_until\":\"2026-11-10T12:10:00.000000+00:00\"}}";
  CT_CHECK(ccord_live_gateway_decode(upd, strlen(upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.guild_id == 77);
  CT_CHECK(ev.member_count == 1 && ev.members[0].user_id == 43);
  CT_CHECK(strcmp(ev.members[0].nick, "Ace") == 0);
  CT_CHECK(ev.members[0].timeout_until_ms == 1794312600000LL);
  ccord_live_event_free(&ev);

  // Cleared nickname and lifted timeout arrive as null, and must come through as "none", not be skipped.
  const char *cleared =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\",\"username\":\"ada\"},"
    "\"nick\":null,\"roles\":[],\"communication_disabled_until\":null}}";
  CT_CHECK(ccord_live_gateway_decode(cleared, strlen(cleared), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].nick[0] == '\0' && ev.members[0].timeout_until_ms == 0);
  ccord_live_event_free(&ev);

  const char *add =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_ADD\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},\"roles\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(add, strlen(add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.members[0].user_id == 44);
  ccord_live_event_free(&ev);

  const char *rm = "{\"op\":0,\"t\":\"GUILD_MEMBER_REMOVE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"43\"}}}";
  CT_CHECK(ccord_live_gateway_decode(rm, strlen(rm), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MEMBER_REMOVE && ev.ev.guild_id == 77 && ev.ev.user_id == 43);
  ccord_live_event_free(&ev);

  // Malformed payloads are rejected, never turned into a removal of user 0.
  const char *bad = "{\"op\":0,\"t\":\"GUILD_MEMBER_REMOVE\",\"d\":{\"guild_id\":\"77\"}}";
  CT_CHECK(ccord_live_gateway_decode(bad, strlen(bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);   // a failed decode still owns what it parsed
  const char *bad2 = "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"user\":{\"id\":\"43\"}}}";
  CT_CHECK(ccord_live_gateway_decode(bad2, strlen(bad2), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // The REST answer to PATCH /guilds/{g}/members/{u} is the same object without guild_id.
  const char *rest = "{\"user\":{\"id\":\"43\",\"username\":\"ada\"},\"nick\":\"Ace\",\"roles\":[],"
                     "\"communication_disabled_until\":\"2026-11-10T12:10:00.000000+00:00\"}";
  CT_CHECK(ccord_live_json_member(rest, strlen(rest), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.guild_id == 77 && ev.members[0].timeout_until_ms == 1794312600000LL);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_member(rest, strlen(rest), 0, &ev, &err) == CCORD_ERR_INVALID);   // refused before it parses anything
  CT_CHECK(ccord_live_json_member("[]", 2, 77, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);

  // The REST answer to PATCH /guilds/{g}: a rename, with no channel list in it.
  const char *g = "{\"id\":\"77\",\"name\":\"New Name\",\"owner_id\":\"42\",\"icon\":null}";
  CT_CHECK(ccord_live_json_guild(g, strlen(g), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_UPSERT && ev.ev.guild_id == 77 && strcmp(ev.ev.guild->name, "New Name") == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_guild("{}", 2, &ev, &err) == CCORD_ERR_PARSE);
  ccord_live_event_free(&ev);
}

// What the user reads when Discord refuses: its own message, plus the nested detail for form errors.
static void test_discord_error_message(void)
{
  char out[200];
  const char *perm = "{\"message\":\"Missing Permissions\",\"code\":50013}";
  CT_CHECK(ccord_live_json_error_message(perm, strlen(perm), out, sizeof out));
  CT_CHECK(strcmp(out, "Missing Permissions") == 0);

  const char *form =
    "{\"message\":\"Invalid Form Body\",\"code\":50035,\"errors\":{\"name\":{\"_errors\":"
    "[{\"code\":\"BASE_TYPE_MAX_LENGTH\",\"message\":\"Must be 100 or fewer in length.\"}]}}}";
  CT_CHECK(ccord_live_json_error_message(form, strlen(form), out, sizeof out));
  CT_CHECK(strcmp(out, "Invalid Form Body: Must be 100 or fewer in length.") == 0);

  // Deeper nesting (channel permission overwrites, etc.) is still found.
  const char *deep = "{\"message\":\"Invalid Form Body\",\"errors\":{\"a\":{\"b\":{\"c\":{\"_errors\":[{\"message\":\"deep\"}]}}}}}";
  CT_CHECK(ccord_live_json_error_message(deep, strlen(deep), out, sizeof out));
  CT_CHECK(strstr(out, "deep") != nullptr);

  // Not Discord's shape: nothing to say, and the buffer is left a valid empty string.
  CT_CHECK(!ccord_live_json_error_message("<html>502</html>", 16, out, sizeof out));
  CT_CHECK(out[0] == '\0');
  CT_CHECK(!ccord_live_json_error_message("{\"code\":1}", 10, out, sizeof out));
  CT_CHECK(!ccord_live_json_error_message(nullptr, 0, out, sizeof out));

  // A long message is truncated safely, never overrun.
  char small[12];
  CT_CHECK(ccord_live_json_error_message(perm, strlen(perm), small, sizeof small));
  CT_CHECK(strlen(small) < sizeof small);
}

static const ccord_notify_scope *find_notify(const ccord_net_event *e, ccord_id g, ccord_id c)
{
  for (size_t i = 0; i < e->notify_count; i++) {
    if (e->notify[i].guild_id == g && e->notify[i].channel_id == c) return &e->notify[i];
  }
  return nullptr;
}

// READY's user_guild_settings and the gateway's USER_GUILD_SETTINGS_UPDATE: mute (timed or open ended), level,
// suppress flags and per-channel overrides, for servers and for direct messages.
static void test_gateway_notify_settings(void)
{
  ccord_live_event ev;
  ccord_error err;
  const char *ready_a =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
      "\"guilds\":[{\"id\":\"77\",\"name\":\"HQ\",\"channels\":[{\"id\":\"11\",\"type\":0,\"name\":\"general\"}],\"members\":[]}],"
      "\"user_guild_settings\":{\"version\":3,\"partial\":false,\"entries\":["
      "{\"guild_id\":\"77\",\"message_notifications\":1,\"muted\":true,"
      "\"mute_config\":{\"selected_time_window\":3600,\"end_time\":\"2026-11-10T12:30:00.000Z\"},"
      "\"suppress_everyone\":true,\"suppress_roles\":false,\"channel_overrides\":["
      "{\"channel_id\":\"11\",\"message_notifications\":2,\"muted\":false,\"mute_config\":null},"
      "{\"channel_id\":\"12\",\"message_notifications\":3,\"muted\":true,\"mute_config\":{\"end_time\":null}},"
      "{\"channel_id\":\"oops\",\"muted\":true},"
      "{\"muted\":true}]},"
      "{\"guild_id\":null,\"message_notifications\":3,\"muted\":false,\"channel_overrides\":["
      "{\"channel_id\":\"900\",\"muted\":true,\"mute_config\":{\"end_time\":\"2026-11-10T12:30:00Z\"}}]},"
      "{\"guild_id\":\"not-an-id\",\"muted\":true},"
      "{\"message_notifications\":0,\"muted\":true}]}}}";
  CT_CHECK(ccord_live_gateway_decode(ready_a, strlen(ready_a), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.ev.notify_count == 5);                           // 77 + 2 good overrides, DMs + 1 override; the bad entries are gone
  const ccord_notify_scope *srv = find_notify(&ev.ev, 77, 0);
  CT_CHECK(srv && srv->muted && srv->level == CCORD_NOTIFY_MENTIONS && srv->suppress_everyone && !srv->suppress_roles);
  CT_CHECK(srv && srv->mute_until_ms == 1794313800000LL);    // 2026-11-10T12:30:00Z
  const ccord_notify_scope *c11 = find_notify(&ev.ev, 77, 11);
  CT_CHECK(c11 && !c11->muted && c11->level == CCORD_NOTIFY_NOTHING && c11->mute_until_ms == 0);
  const ccord_notify_scope *c12 = find_notify(&ev.ev, 77, 12);
  CT_CHECK(c12 && c12->muted && c12->mute_until_ms == 0 && c12->level == CCORD_NOTIFY_INHERIT);   // until turned back on
  const ccord_notify_scope *dm = find_notify(&ev.ev, CCORD_DM_GUILD_ID, 0);
  CT_CHECK(dm && !dm->muted && dm->level == CCORD_NOTIFY_INHERIT);                                 // guild_id null = direct messages
  const ccord_notify_scope *dm900 = find_notify(&ev.ev, CCORD_DM_GUILD_ID, 900);
  CT_CHECK(dm900 && dm900->muted && dm900->mute_until_ms == 1794313800000LL);
  CT_CHECK(find_notify(&ev.ev, 77, 0) < find_notify(&ev.ev, 77, 12));                              // rows stay grouped per server
  ccord_live_event_free(&ev);

  // The same settings as a bare array (older gateway shape).
  const char *ready_b =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"guilds\":[],"
      "\"user_guild_settings\":[{\"guild_id\":\"77\",\"message_notifications\":0,\"muted\":false}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_b, strlen(ready_b), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.notify_count == 1 && ev.ev.notify[0].guild_id == 77 && ev.ev.notify[0].level == CCORD_NOTIFY_ALL);
  ccord_live_event_free(&ev);

  // No settings in the payload: nothing is claimed.
  const char *ready_c = "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"guilds\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_c, strlen(ready_c), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.notify_count == 0 && ev.ev.notify == nullptr);
  ccord_live_event_free(&ev);

  // A muted row with an end time that cannot be read is open ended; an unmuted row ignores its end time.
  const char *ready_d =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"guilds\":[],"
      "\"user_guild_settings\":[{\"guild_id\":\"5\",\"muted\":true,\"mute_config\":{\"end_time\":\"soon\"}},"
      "{\"guild_id\":\"6\",\"muted\":false,\"mute_config\":{\"end_time\":\"2026-11-10T12:30:00Z\"}}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_d, strlen(ready_d), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.notify_count == 2 && ev.ev.notify[0].muted && ev.ev.notify[0].mute_until_ms == 0);
  CT_CHECK(!ev.ev.notify[1].muted && ev.ev.notify[1].mute_until_ms == 0);
  // No level in the entry means none was chosen: "use the default", never "All Messages" by accident.
  CT_CHECK(ev.ev.notify[0].level == CCORD_NOTIFY_INHERIT && ev.ev.notify[1].level == CCORD_NOTIFY_INHERIT);
  ccord_live_event_free(&ev);

  // USER_GUILD_SETTINGS_UPDATE carries one server whole.
  const char *upd =
      "{\"op\":0,\"t\":\"USER_GUILD_SETTINGS_UPDATE\",\"d\":{\"guild_id\":\"77\",\"message_notifications\":2,\"muted\":false,"
      "\"suppress_roles\":true,\"channel_overrides\":[{\"channel_id\":\"11\",\"muted\":true,\"mute_config\":null}]}}";
  CT_CHECK(ccord_live_gateway_decode(upd, strlen(upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_NOTIFY_SETTINGS && ev.ev.guild_id == 77 && ev.ev.notify_count == 2);
  CT_CHECK(ev.ev.notify[0].channel_id == 0 && ev.ev.notify[0].level == CCORD_NOTIFY_NOTHING && ev.ev.notify[0].suppress_roles &&
           !ev.ev.notify[0].suppress_everyone);
  CT_CHECK(ev.ev.notify[1].channel_id == 11 && ev.ev.notify[1].muted && ev.ev.notify[1].mute_until_ms == 0);
  ccord_live_event_free(&ev);
  const char *upd_dm = "{\"op\":0,\"t\":\"USER_GUILD_SETTINGS_UPDATE\",\"d\":{\"guild_id\":null,\"muted\":true}}";
  CT_CHECK(ccord_live_gateway_decode(upd_dm, strlen(upd_dm), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_NOTIFY_SETTINGS && ev.ev.guild_id == CCORD_DM_GUILD_ID && ev.ev.notify_count == 1 &&
           ev.ev.notify[0].muted);
  ccord_live_event_free(&ev);
  const char *upd_bad = "{\"op\":0,\"t\":\"USER_GUILD_SETTINGS_UPDATE\",\"d\":{\"muted\":true}}";
  CT_CHECK(ccord_live_gateway_decode(upd_bad, strlen(upd_bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);
}

static void test_polls_components_and_modals_json(void)
{
  ccord_error err;
  ccord_live_event ev;

  // 1. Message with components: buttons, selects, emoji, disabled, default option, invalid items skipped, application_id.
  const char *msg_json =
      "{\"id\":\"900\",\"channel_id\":\"10\",\"application_id\":\"555001\","
      "\"author\":{\"id\":\"42\",\"username\":\"HelperBot\",\"bot\":true},"
      "\"content\":\"\",\"timestamp\":\"2026-11-10T12:00:00Z\","
      "\"components\":["
      "{\"type\":1,\"components\":["
      "{\"type\":2,\"style\":1,\"label\":\"Confirm\",\"custom_id\":\"act:yes\",\"emoji\":{\"name\":\"ok\",\"id\":\"701\"}},"
      "{\"type\":2,\"style\":5,\"label\":\"Docs\",\"url\":\"https://example.com\",\"disabled\":true},"
      "{\"type\":2,\"style\":6},"
      "{\"type\":2,\"style\":5,\"label\":\"BrokenLink\"},"
      "{\"type\":2,\"style\":1,\"label\":\"BrokenBtn\"},"
      "{\"type\":4,\"custom_id\":\"modal_only\"}"
      "]},"
      "{\"type\":1,\"components\":["
      "{\"type\":3,\"custom_id\":\"sel:lang\",\"placeholder\":\"Language\",\"min_values\":0,\"max_values\":2,"
      "\"options\":["
      "{\"label\":\"C23\",\"value\":\"c23\",\"description\":\"Systems\",\"default\":true,\"emoji\":{\"name\":\"gear\",\"id\":\"702\"}},"
      "{\"value\":\"zig\"},"
      "{\"label\":\"EmptyVal\",\"value\":\"\"}"
      "]}"
      "]},"
      "{\"type\":1,\"components\":["
      "{\"type\":6,\"custom_id\":\"sel:role\",\"min_values\":1,\"max_values\":3}"
      "]}"
      "]}";
  CT_CHECK(ccord_live_json_message(msg_json, strlen(msg_json), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].rich != nullptr);
  const ccord_message_rich *r = ev.messages[0].rich;
  CT_CHECK(r->application_id == 555001 && r->component_row_count == 3);
  CT_CHECK(r->components[0].component_count == 3);
  CT_CHECK(r->components[0].components[0].kind == CCORD_COMP_BUTTON &&
           r->components[0].components[0].style == CCORD_BTN_PRIMARY &&
           strcmp(r->components[0].components[0].custom_id, "act:yes") == 0 &&
           strcmp(r->components[0].components[0].emoji, "ok") == 0 &&
           r->components[0].components[0].emoji_id == 701 &&
           !r->components[0].components[0].disabled);
  CT_CHECK(r->components[0].components[1].style == CCORD_BTN_LINK &&
           strcmp(r->components[0].components[1].url, "https://example.com") == 0 &&
           r->components[0].components[1].disabled);
  CT_CHECK(r->components[0].components[2].style == CCORD_BTN_PREMIUM);
  const ccord_component *sel = &r->components[1].components[0];
  CT_CHECK(sel->kind == CCORD_COMP_STRING_SELECT && sel->min_values == 0 && sel->max_values == 2 && sel->option_count == 2);
  CT_CHECK(strcmp(sel->options[0].label, "C23") == 0 && strcmp(sel->options[0].value, "c23") == 0 &&
           strcmp(sel->options[0].description, "Systems") == 0 && sel->options[0].default_ &&
           sel->options[0].emoji_id == 702);
  CT_CHECK(strcmp(sel->options[1].label, "zig") == 0 && strcmp(sel->options[1].value, "zig") == 0 && !sel->options[1].default_);
  CT_CHECK(r->components[2].components[0].kind == CCORD_COMP_ROLE_SELECT &&
           r->components[2].components[0].min_values == 1 && r->components[2].components[0].max_values == 3);
  CT_CHECK(ccord_message_rich_validate(r, &err) == CCORD_OK);
  ccord_live_event_free(&ev);

  // Bot author fallback when application_id is absent vs non-bot author leaving application_id == 0.
  const char *bot_fallback =
      "{\"id\":\"901\",\"channel_id\":\"10\",\"author\":{\"id\":\"888\",\"username\":\"Bot\",\"bot\":true},"
      "\"content\":\"hi\",\"timestamp\":\"2026-11-10T12:00:00Z\","
      "\"components\":[{\"type\":1,\"components\":[{\"type\":2,\"style\":2,\"label\":\"Ping\",\"custom_id\":\"ping\"}]}]}";
  CT_CHECK(ccord_live_json_message(bot_fallback, strlen(bot_fallback), CCORD_NET_MESSAGE_CREATE, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.messages[0].rich && ev.messages[0].rich->application_id == 888);
  ccord_live_event_free(&ev);

  // Partial MESSAGE_UPDATE clearing components sets CCORD_RICH_HAS_COMPONENTS and text_absent.
  const char *comp_clear_upd =
      "{\"op\":0,\"t\":\"MESSAGE_UPDATE\",\"d\":{\"id\":\"901\",\"channel_id\":\"10\",\"components\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(comp_clear_upd, strlen(comp_clear_upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_UPDATE && ev.messages[0].rich != nullptr);
  CT_CHECK((ev.messages[0].rich->present & CCORD_RICH_HAS_COMPONENTS) != 0 &&
           ev.messages[0].rich->component_row_count == 0 && ev.messages[0].rich->text_absent);
  ccord_live_event_free(&ev);

  // 2. MESSAGE_POLL_VOTE_ADD / REMOVE gateway events.
  const char *pv_add =
      "{\"op\":0,\"t\":\"MESSAGE_POLL_VOTE_ADD\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\","
      "\"message_id\":\"500\",\"guild_id\":\"7\",\"answer_id\":3}}";
  CT_CHECK(ccord_live_gateway_decode(pv_add, strlen(pv_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_POLL_VOTE && ev.ev.user_id == 42 && ev.ev.channel_id == 10 &&
           ev.ev.message_id == 500 && ev.ev.guild_id == 7 && ev.ev.poll_answer_id == 3 && ev.ev.poll_vote_added);
  ccord_live_event_free(&ev);

  const char *pv_rem =
      "{\"op\":0,\"t\":\"MESSAGE_POLL_VOTE_REMOVE\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\","
      "\"message_id\":\"500\",\"answer_id\":3}}";
  CT_CHECK(ccord_live_gateway_decode(pv_rem, strlen(pv_rem), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_POLL_VOTE && ev.ev.guild_id == 0 && ev.ev.poll_answer_id == 3 && !ev.ev.poll_vote_added);
  ccord_live_event_free(&ev);

  const char *pv_bad = "{\"op\":0,\"t\":\"MESSAGE_POLL_VOTE_ADD\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\",\"message_id\":\"500\",\"answer_id\":0}}";
  CT_CHECK(ccord_live_gateway_decode(pv_bad, strlen(pv_bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // 3. INTERACTION_MODAL_CREATE gateway events (nested d.data and flat d).
  const char *modal_gw =
      "{\"op\":0,\"t\":\"INTERACTION_MODAL_CREATE\",\"d\":{\"id\":\"9901\",\"application_id\":\"555001\","
      "\"guild_id\":\"7\",\"channel_id\":\"10\",\"data\":{\"custom_id\":\" ticket:open \",\"title\":\"New Ticket\","
      "\"components\":["
      "{\"type\":1,\"components\":[{\"type\":4,\"custom_id\":\"subj\",\"label\":\"Subject\",\"style\":1,\"min_length\":3,\"max_length\":100}]},"
      "{\"type\":1,\"components\":[{\"type\":4,\"custom_id\":\"body\",\"label\":\"Details\",\"placeholder\":\"Optional\","
      "\"value\":\"init\",\"style\":2,\"required\":false}]}"
      "]}}}";
  CT_CHECK(ccord_live_gateway_decode(modal_gw, strlen(modal_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_INTERACTION_MODAL && ev.ev.modal != nullptr);
  if (ev.ev.modal) {
    CT_CHECK(ev.ev.modal->id == 9901 && ev.ev.modal->application_id == 555001 &&
             ev.ev.modal->guild_id == 7 && ev.ev.modal->channel_id == 10);
    CT_CHECK(strcmp(ev.ev.modal->title, "New Ticket") == 0 && ev.ev.modal->field_count == 2);
    CT_CHECK(strcmp(ev.ev.modal->fields[0].custom_id, "subj") == 0 && ev.ev.modal->fields[0].style == 1 &&
             ev.ev.modal->fields[0].min_length == 3 && ev.ev.modal->fields[0].max_length == 100 &&
             ev.ev.modal->fields[0].required);
    CT_CHECK(strcmp(ev.ev.modal->fields[1].custom_id, "body") == 0 && ev.ev.modal->fields[1].style == 2 &&
             strcmp(ev.ev.modal->fields[1].value, "init") == 0 && !ev.ev.modal->fields[1].required);
  }
  ccord_live_event_free(&ev);

  const char *modal_bad =
      "{\"op\":0,\"t\":\"INTERACTION_MODAL_CREATE\",\"d\":{\"application_id\":\"555\",\"custom_id\":\"m\",\"title\":\"Empty\",\"components\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(modal_bad, strlen(modal_bad), &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // 4. Wire JSON builders: poll votes & interactions.
  ccord_buf b;
  ccord_buf_init(&b, 0);
  CT_CHECK(ccord_live_build_poll_vote_body(nullptr, 0, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"answer_ids\":[]}") == 0);
  ccord_buf_clear(&b);
  uint32_t aids[2] = {1, 4};
  CT_CHECK(ccord_live_build_poll_vote_body(aids, 2, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b), "{\"answer_ids\":[1,4]}") == 0);
  uint32_t zero_aid = 0;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_poll_vote_body(&zero_aid, 1, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_build_poll_vote_body(aids, CCORD_RICH_MAX_POLL_ANSWERS + 1, &b, &err) == CCORD_ERR_RANGE);

  ccord_buf_clear(&b);
  ccord_interaction_request btn_req = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 555, .guild_id = 7, .channel_id = 10, .message_id = 500, .message_flags = 64,
    .component_type = CCORD_COMP_BUTTON, .custom_id = "act:yes",
  };
  CT_CHECK(ccord_live_build_interaction_body(&btn_req, "sess1", "999", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":3") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"guild_id\":\"7\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"component_type\":2") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"values\"") == nullptr);

  ccord_buf_clear(&b);
  const char *svals[2] = {"a\"b", "c"};
  ccord_interaction_request sel_req = {
    .type = CCORD_INTERACTION_MESSAGE_COMPONENT,
    .application_id = 555, .guild_id = CCORD_DM_GUILD_ID, .channel_id = 10, .message_id = 500,
    .component_type = CCORD_COMP_STRING_SELECT, .custom_id = "sel:x", .values = svals, .value_count = 2,
  };
  CT_CHECK(ccord_live_build_interaction_body(&sel_req, "sess1", "1000", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"guild_id\"") == nullptr &&
           strstr(ccord_buf_cstr(&b), "\"values\":[\"a\\\"b\",\"c\"]") != nullptr);

  ccord_buf_clear(&b);
  const char *fids[2] = {"subj", "body"};
  const char *fvals[2] = {"Hello", "Line 1\nLine 2"};
  ccord_interaction_request mod_req = {
    .type = CCORD_INTERACTION_MODAL_SUBMIT,
    .application_id = 555, .guild_id = 7, .channel_id = 10,
    .custom_id = "ticket:open", .modal_id = 9901,
    .field_ids = fids, .field_values = fvals, .field_count = 2,
  };
  CT_CHECK(ccord_live_build_interaction_body(&mod_req, "sess1", "1001", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":5") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"id\":\"9901\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"type\":4,\"custom_id\":\"subj\",\"value\":\"Hello\"") != nullptr);
  ccord_buf_free(&b);
}

static void test_slash_commands_ephemeral_reply_and_uploads_json(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 0);

  // 1. Reply builder: mention_author true vs false ("replied_user":false).
  CT_CHECK(ccord_live_build_reply_body("hello", 5, 77, true, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"message_id\":\"77\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"replied_user\"") == nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_reply_body("quiet", 5, 77, false, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"message_id\":\"77\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"allowed_mentions\":{\"replied_user\":false}") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_reply_body("bad", 3, CCORD_ID_NONE, true, &b, &err) == CCORD_ERR_INVALID);

  // 2. Multi-file upload slots + final message body builders (SPOILER_ prefix idempotent).
  static const uint8_t k_dummy_bytes[4] = {1, 2, 3, 4};
  ccord_upload_file ufiles[3] = {
    {.filename = "a.png", .data = k_dummy_bytes, .size = 120, .spoiler = false},
    {.filename = "b.png", .data = k_dummy_bytes, .size = 240, .spoiler = true},
    {.filename = "SPOILER_c.png", .data = k_dummy_bytes, .size = 360, .spoiler = true},
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_slots_body(ufiles, 3, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"filename\":\"a.png\",\"file_size\":120,\"id\":\"0\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"filename\":\"SPOILER_b.png\",\"file_size\":240,\"id\":\"1\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"filename\":\"SPOILER_c.png\",\"file_size\":360,\"id\":\"2\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "SPOILER_SPOILER_") == nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_slots_body(ufiles, 0, &b, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_build_upload_slots_body(ufiles, CCORD_RICH_MAX_ATTACHMENTS + 1, &b, &err) == CCORD_ERR_RANGE);

  char up_names[3][256] = {"up/0/a.png", "up/1/SPOILER_b.png", "up/2/SPOILER_c.png"};
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, 88, false, ufiles, up_names, 3, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"content\":\"\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"message_id\":\"88\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"replied_user\":false") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"id\":\"0\",\"filename\":\"a.png\",\"uploaded_filename\":\"up/0/a.png\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"id\":\"1\",\"filename\":\"SPOILER_b.png\",\"uploaded_filename\":\"up/1/SPOILER_b.png\"") != nullptr);

  // 2b. Voice upload: duration_secs + waveform on the attachment and flags 8192 on the message; an ordinary upload
  //     body carries none of them (the checks above already pin its exact attachment text).
  CT_CHECK(strstr(ccord_buf_cstr(&b), "duration_secs") == nullptr && strstr(ccord_buf_cstr(&b), "waveform") == nullptr &&
           strstr(ccord_buf_cstr(&b), "\"flags\"") == nullptr);
  ccord_upload_file vfile = {.filename = "voice-message.ogg", .data = k_dummy_bytes, .size = 4,
                             .duration_ms = 2500, .waveform_b64 = "AAECAwQFBgcICQ=="};
  char vnames[1][256] = {"up/0/voice-message.ogg"};
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&b),
                  "{\"content\":\"\",\"attachments\":[{\"id\":\"0\",\"filename\":\"voice-message.ogg\","
                  "\"uploaded_filename\":\"up/0/voice-message.ogg\",\"duration_secs\":2.5,"
                  "\"waveform\":\"AAECAwQFBgcICQ==\"}],\"flags\":8192}") == 0);
  vfile.waveform_b64 = nullptr;   // no waveform: Discord draws none, but the message is still a voice message
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"duration_secs\":2.5") != nullptr && strstr(ccord_buf_cstr(&b), "waveform") == nullptr &&
           strstr(ccord_buf_cstr(&b), "\"flags\":8192") != nullptr);
  vfile.waveform_b64 = "not base64!";
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_ERR_INVALID);
  char long_wf[CCORD_RICH_WAVEFORM_MAX_BYTES + 2];
  memset(long_wf, 'A', sizeof long_wf - 1);
  long_wf[sizeof long_wf - 1] = '\0';
  vfile.waveform_b64 = long_wf;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_ERR_INVALID);
  // A waveform on an ordinary (duration 0) file is ignored: the body stays the plain upload body.
  vfile.duration_ms = 0;
  vfile.waveform_b64 = "AAEC";
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_upload_message_body("", 0, CCORD_ID_NONE, true, &vfile, vnames, 1, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "waveform") == nullptr && strstr(ccord_buf_cstr(&b), "\"flags\"") == nullptr);

  // 3. Slash command & autocomplete interaction JSON builders (type 2 & type 4).
  const char *onames[5] = {"count", "ratio", "quiet", "text", "target"};
  uint8_t okinds[5] = {CCORD_SLASH_OPT_INTEGER, CCORD_SLASH_OPT_NUMBER, CCORD_SLASH_OPT_BOOLEAN,
                       CCORD_SLASH_OPT_STRING, CCORD_SLASH_OPT_USER};
  const char *ovals[5] = {"42", "3.5", "true", "hi \"there\"", "1002"};
  ccord_interaction_request sreq = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 5,
    .channel_id = 10,
    .command_id = 9101,
    .command_version = 0, // falls back to command_id
    .command_name = "run",
    .option_names = onames,
    .option_kinds = okinds,
    .option_values = ovals,
    .option_count = 5,
    .focused_option = -1,
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&sreq, "sess1", "2001", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":2") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"guild_id\":\"5\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"version\":\"9101\",\"id\":\"9101\",\"name\":\"run\",\"type\":1") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":4,\"name\":\"count\",\"value\":42}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":10,\"name\":\"ratio\",\"value\":3.5}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":5,\"name\":\"quiet\",\"value\":true}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":3,\"name\":\"text\",\"value\":\"hi \\\"there\\\"\"}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":6,\"name\":\"target\",\"value\":\"1002\"}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"focused\"") == nullptr);

  // Type 4 autocomplete marks only focused_option with "focused":true, omits guild_id in DMs.
  sreq.type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE;
  sreq.guild_id = CCORD_DM_GUILD_ID;
  sreq.focused_option = 3;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&sreq, "sess1", "2002", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":4") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"guild_id\"") == nullptr &&
           strstr(ccord_buf_cstr(&b), "{\"type\":3,\"name\":\"text\",\"value\":\"hi \\\"there\\\"\",\"focused\":true}") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"name\":\"count\",\"value\":42,\"focused\"") == nullptr);
  sreq.focused_option = 5;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&sreq, "sess1", "2003", &b, &err) == CCORD_ERR_INVALID);
  ccord_buf_free(&b);

  // 4. Multi-slot upload response parser (including out-of-order numeric and string slot IDs).
  const char *slots_json =
      "{\"attachments\":["
      "{\"id\":\"2\",\"upload_url\":\"https://upload.discord.com/put/2\",\"upload_filename\":\"buck/2\"},"
      "{\"id\":1,\"upload_url\":\"https://upload.discord.com/put/1\",\"upload_filename\":\"buck/1\"},"
      "{\"id\":\"0\",\"upload_url\":\"https://upload.discord.com/put/0\",\"upload_filename\":\"buck/0\"}"
      "]}";
  char urls[4][2048], names[4][256];
  CT_CHECK(ccord_live_json_upload_slots(slots_json, strlen(slots_json), 3, urls, names, &err) == CCORD_OK);
  CT_CHECK(strcmp(urls[0], "https://upload.discord.com/put/0") == 0 && strcmp(names[0], "buck/0") == 0);
  CT_CHECK(strcmp(urls[1], "https://upload.discord.com/put/1") == 0 && strcmp(names[1], "buck/1") == 0);
  CT_CHECK(strcmp(urls[2], "https://upload.discord.com/put/2") == 0 && strcmp(names[2], "buck/2") == 0);
  CT_CHECK(ccord_live_json_upload_slots(slots_json, strlen(slots_json), 4, urls, names, &err) != CCORD_OK);

  // 5. Slash command index decoder (object & array, CHAT_INPUT + USER/MESSAGE context commands,
  // default_member_permissions, dm_permission, options).
  const char *idx_json =
      "{\"application_commands\":["
      "{\"id\":\"9101\",\"application_id\":\"777\",\"type\":1,\"name\":\"ping\",\"description\":\"Check latency\","
      "\"default_member_permissions\":null,\"dm_permission\":true},"
      "{\"id\":\"9102\",\"application_id\":\"777\",\"type\":2,\"name\":\"UserInfo\",\"description\":\"Context menu\"},"
      "{\"id\":\"9103\",\"application_id\":\"777\",\"guild_id\":\"5\",\"version\":\"9999\",\"type\":1,"
      "\"name\":\"purge\",\"description\":\"Mod purge\",\"default_member_permissions\":\"8192\",\"dm_permission\":false,"
      "\"options\":["
      "{\"type\":4,\"name\":\"count\",\"description\":\"How many\",\"required\":true},"
      "{\"type\":3,\"name\":\"topic\",\"description\":\"Filter\",\"required\":false,\"autocomplete\":true}"
      "]},"
      "{\"id\":\"9104\",\"application_id\":\"777\",\"type\":1,\"name\":\"rootonly\",\"description\":\"Admin only\","
      "\"default_member_permissions\":\"0\"}"
      "]}";
  ccord_live_event ev;
  CT_CHECK(ccord_live_json_slash_commands(idx_json, strlen(idx_json), 5, 10, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SLASH_COMMANDS && ev.ev.guild_id == 5 && ev.ev.channel_id == 10 &&
           ev.ev.slash_command_count == 4);
  if (ev.ev.slash_command_count == 4) {
    CT_CHECK(strcmp(ev.ev.slash_commands[0].name, "ping") == 0 &&
             ev.ev.slash_commands[0].command_type == 1 &&
             ev.ev.slash_commands[0].default_member_permissions == UINT64_MAX &&
             ev.ev.slash_commands[0].dm_permission &&
             ev.ev.slash_commands[0].version == 9101);
    CT_CHECK(strcmp(ev.ev.slash_commands[1].name, "UserInfo") == 0 &&
             ev.ev.slash_commands[1].command_type == 2 &&
             ev.ev.slash_commands[1].option_count == 0);
    CT_CHECK(strcmp(ev.ev.slash_commands[2].name, "purge") == 0 &&
             ev.ev.slash_commands[2].version == 9999 &&
             ev.ev.slash_commands[2].default_member_permissions == 8192u &&
             !ev.ev.slash_commands[2].dm_permission &&
             ev.ev.slash_commands[2].option_count == 2 &&
             ev.ev.slash_commands[2].options[0].kind == CCORD_SLASH_OPT_INTEGER &&
             ev.ev.slash_commands[2].options[0].required &&
             !ev.ev.slash_commands[2].options[0].autocomplete &&
             ev.ev.slash_commands[2].options[1].kind == CCORD_SLASH_OPT_STRING &&
             ev.ev.slash_commands[2].options[1].autocomplete);
    CT_CHECK(strcmp(ev.ev.slash_commands[3].name, "rootonly") == 0 &&
             ev.ev.slash_commands[3].default_member_permissions == 0u);
  }
  ccord_live_event_free(&ev);

  // 6. Gateway APPLICATION_COMMAND_AUTOCOMPLETE_RESULT and ephemeral MESSAGE_CREATE (flags: 64).
  const char *ac_gw =
      "{\"op\":0,\"t\":\"APPLICATION_COMMAND_AUTOCOMPLETE_RESULT\",\"d\":{\"nonce\":\"n-42\",\"choices\":["
      "{\"name\":\"SDL3 GPU\",\"value\":\"sdl3\"},"
      "{\"name\":\"Answer\",\"value\":42}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(ac_gw, strlen(ac_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SLASH_AUTOCOMPLETE && ev.ev.slash_choice_count == 2 &&
           ev.ev.interaction_nonce && strcmp(ev.ev.interaction_nonce, "n-42") == 0);
  if (ev.ev.slash_choice_count == 2) {
    CT_CHECK(strcmp(ev.ev.slash_choices[0].name, "SDL3 GPU") == 0 &&
             strcmp(ev.ev.slash_choices[0].value, "sdl3") == 0);
    CT_CHECK(strcmp(ev.ev.slash_choices[1].name, "Answer") == 0 &&
             strcmp(ev.ev.slash_choices[1].value, "42") == 0);
  }
  ccord_live_event_free(&ev);

  const char *ephem_gw =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"999\",\"channel_id\":\"10\",\"type\":20,\"flags\":64,"
      "\"author\":{\"id\":\"777\",\"username\":\"Bot\",\"bot\":true},\"content\":\"Only for you\","
      "\"timestamp\":\"2026-11-10T12:00:00Z\"}}";
  CT_CHECK(ccord_live_gateway_decode(ephem_gw, strlen(ephem_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE && ev.message_count == 1 &&
           ccord_message_is_ephemeral(&ev.messages[0]));
  ccord_live_event_free(&ev);
}

static void test_search_pins_threads_and_burst_reactions_json(void)
{
  ccord_error err;
  ccord_live_event ev;

  // 1. ccord_live_build_search_path: guild vs DM, all operators, offset, empty rejection.
  ccord_search_query sq;
  ccord_search_parse("has:image has:poll pinned:true hello world", 42, &sq);
  sq.author_id = 101;
  sq.mentions_id = 202;
  sq.channel_id = 303;
  sq.min_id = 404;
  sq.max_id = 505;
  sq.offset = 50;
  ccord_buf path;
  ccord_buf_init(&path, 512);
  CT_CHECK(ccord_live_build_search_path(9001, CCORD_ID_NONE, &sq, &path, &err) == CCORD_OK);
  const char *ps = ccord_buf_cstr(&path);
  CT_CHECK(strncmp(ps, "/guilds/9001/messages/search?", 29) == 0);
  CT_CHECK(strstr(ps, "content=hello%20world") != nullptr);
  CT_CHECK(strstr(ps, "channel_id=303") != nullptr);
  CT_CHECK(strstr(ps, "author_id=101") != nullptr);
  CT_CHECK(strstr(ps, "mentions=202") != nullptr);
  CT_CHECK(strstr(ps, "has=image") != nullptr && strstr(ps, "has=poll") != nullptr);
  CT_CHECK(strstr(ps, "min_id=404") != nullptr && strstr(ps, "max_id=505") != nullptr);
  CT_CHECK(strstr(ps, "pinned=true") != nullptr && strstr(ps, "offset=50") != nullptr);
  ccord_buf_free(&path);

  ccord_buf_init(&path, 256);
  ccord_search_parse("dm query", 8, &sq);
  CT_CHECK(ccord_live_build_search_path(CCORD_DM_GUILD_ID, 777, &sq, &path, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_buf_cstr(&path), "/channels/777/messages/search?content=dm%20query") == 0);
  ccord_buf_free(&path);

  ccord_buf_init(&path, 256);
  ccord_search_parse("", 0, &sq);
  CT_CHECK(ccord_live_build_search_path(9001, CCORD_ID_NONE, &sq, &path, &err) == CCORD_ERR_INVALID);
  ccord_buf_free(&path);

  // 2. ccord_live_json_search: total_results & flattened hits.
  const char *sjson =
      "{\"total_results\":137,\"messages\":[[{\"id\":\"1\",\"channel_id\":\"10\","
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"hit 1\","
      "\"timestamp\":\"2026-11-10T00:00:00Z\"}]]}";
  CT_CHECK(ccord_live_json_search(sjson, strlen(sjson), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SEARCH_RESULTS && ev.ev.search_total_results == 137 &&
           ev.message_count == 1 && ev.messages[0].id == 1);
  ccord_live_event_free(&ev);

  // 3. ccord_live_json_pins: modern {items, has_more} + cursor AND legacy array.
  const char *pjson_new =
      "{\"items\":["
      "{\"pinned_at\":\"2026-11-10T05:00:00.000Z\",\"message\":{\"id\":\"12\",\"channel_id\":\"10\",\"pinned\":true,"
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"p1\",\"timestamp\":\"2026-11-10T01:00:00Z\"}},"
      "{\"pinned_at\":\"2026-11-09T04:00:00.000Z\",\"message\":{\"id\":\"11\",\"channel_id\":\"10\",\"pinned\":true,"
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"p2\",\"timestamp\":\"2026-11-09T01:00:00Z\"}}"
      "],\"has_more\":true}";
  CT_CHECK(ccord_live_json_pins(pjson_new, strlen(pjson_new), 10, false, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PIN_LIST && ev.message_count == 2 && ev.ev.pins_has_more &&
           ev.ev.pins_next_cursor && strcmp(ev.ev.pins_next_cursor, "2026-11-09T04:00:00.000Z") == 0);
  ccord_live_event_free(&ev);

  const char *pjson_old =
      "[{\"id\":\"12\",\"channel_id\":\"10\",\"pinned\":true,"
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"p1\",\"timestamp\":\"2026-11-10T01:00:00Z\"}]";
  CT_CHECK(ccord_live_json_pins(pjson_old, strlen(pjson_old), 10, true, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PIN_LIST && ev.message_count == 1 && ev.ev.pins_append && !ev.ev.pins_has_more);
  ccord_live_event_free(&ev);

  // 4. ccord_live_json_reaction_users.
  const char *ru_json =
      "[{\"id\":\"2\",\"username\":\"ada\",\"global_name\":\"Ada Lovelace\"},"
      "{\"id\":\"3\",\"username\":\"bot\",\"bot\":true}]";
  CT_CHECK(ccord_live_json_reaction_users(ru_json, strlen(ru_json), 10, 500, "\xF0\x9F\x94\xA5", &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_USERS && ev.ev.channel_id == 10 && ev.ev.message_id == 500 &&
           ev.user_count == 2 && strcmp(ev.users[0].display_name, "Ada Lovelace") == 0 && ev.users[1].is_bot);
  ccord_live_event_free(&ev);

  // 5. Burst reactions on messages & gateway MESSAGE_REACTION_ADD / REMOVE.
  const char *burst_msg =
      "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"700\",\"channel_id\":\"10\","
      "\"author\":{\"id\":\"2\",\"username\":\"ada\"},\"content\":\"wow\",\"timestamp\":\"2026-11-10T12:00:00Z\","
      "\"reactions\":[{\"count\":5,\"count_details\":{\"burst\":2,\"normal\":3},\"me\":false,\"me_burst\":true,"
      "\"emoji\":{\"id\":null,\"name\":\"\xF0\x9F\x94\xA5\"}}]}}";
  CT_CHECK(ccord_live_gateway_decode(burst_msg, strlen(burst_msg), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.message_count == 1 && ev.messages[0].reaction_kind_count == 1 &&
           ev.messages[0].reactions[0].count == 5 && ev.messages[0].reactions[0].burst_count == 2 &&
           ev.messages[0].reactions[0].me_burst && !ev.messages[0].reactions[0].self_reacted);
  ccord_live_event_free(&ev);

  const char *burst_add =
      "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{\"user_id\":\"42\",\"channel_id\":\"10\","
      "\"message_id\":\"700\",\"burst\":true,\"type\":1,\"emoji\":{\"id\":null,\"name\":\"\xF0\x9F\x94\xA5\"}}}";
  CT_CHECK(ccord_live_gateway_decode(burst_add, strlen(burst_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_REACTION_UPDATE && ev.ev.user_id == 42 && ev.ev.reaction_delta == 1 &&
           ev.ev.reaction_burst && ev.ev.self_reacted);
  ccord_live_event_free(&ev);

  // 6. Gateway THREAD_CREATE, THREAD_MEMBER_UPDATE, and THREAD_MEMBERS_UPDATE.
  const char *th_create =
      "{\"op\":0,\"t\":\"THREAD_CREATE\",\"d\":{\"id\":\"8801\",\"guild_id\":\"5\",\"parent_id\":\"10\",\"type\":11,"
      "\"name\":\"release-notes\",\"message_count\":7,"
      "\"thread_metadata\":{\"archived\":true,\"locked\":true,\"auto_archive_duration\":60},"
      "\"member\":{\"user_id\":\"42\"}}}";
  CT_CHECK(ccord_live_gateway_decode(th_create, strlen(th_create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CHANNEL_UPSERT && ev.ev.channel && ev.ev.channel->id == 8801 &&
           ev.ev.channel->thread_archived && ev.ev.channel->thread_locked &&
           ev.ev.channel->thread_auto_archive_mins == 60 && ev.ev.channel->thread_message_count == 7 &&
           ev.ev.channel->thread_has_member && ev.ev.channel->thread_joined);
  ccord_live_event_free(&ev);

  const char *tm_upd = "{\"op\":0,\"t\":\"THREAD_MEMBER_UPDATE\",\"d\":{\"id\":\"8801\",\"guild_id\":\"5\",\"user_id\":\"42\"}}";
  CT_CHECK(ccord_live_gateway_decode(tm_upd, strlen(tm_upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_THREAD_MEMBER_SELF && ev.ev.channel_id == 8801 && ev.ev.user_id == 42 && ev.ev.thread_joined);
  ccord_live_event_free(&ev);

  const char *tms_rem =
      "{\"op\":0,\"t\":\"THREAD_MEMBERS_UPDATE\",\"d\":{\"id\":\"8801\",\"guild_id\":\"5\","
      "\"removed_member_ids\":[\"42\"]}}";
  CT_CHECK(ccord_live_gateway_decode(tms_rem, strlen(tms_rem), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_THREAD_MEMBER_SELF && ev.ev.channel_id == 8801 && ev.ev.user_id == 42 && !ev.ev.thread_joined);
  ccord_live_event_free(&ev);
}

static void test_m3b_folders_invite_and_discovery_json(void)
{
  ccord_error err;
  ccord_live_event ev;

  // 1. READY with guild_folders (metadata: folder_id, folder_name, folder_color) + USER_SETTINGS_UPDATE + PROTO + REST.
  const char *ready_gf =
      "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
      "\"guilds\":[{\"id\":\"10\",\"name\":\"Ten\"},{\"id\":\"20\",\"name\":\"Twenty\"},{\"id\":\"30\",\"name\":\"Thirty\"}],"
      "\"user_settings\":{\"guild_folders\":["
      "{\"id\":77,\"name\":\"Core\",\"color\":5793266,\"guild_ids\":[\"30\",\"10\"]},"
      "{\"id\":null,\"name\":null,\"color\":null,\"guild_ids\":[\"20\"]}"
      "]}}}";
  CT_CHECK(ccord_live_gateway_decode(ready_gf, strlen(ready_gf), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT && ev.snapshot.guild_count == 3);
  if (ev.snapshot.guild_count == 3) {
    CT_CHECK(ev.snapshot.guilds[0].id == 30 && ev.snapshot.guilds[0].folder_id == 77 &&
             strcmp(ev.snapshot.guilds[0].folder_name, "Core") == 0 &&
             ev.snapshot.guilds[0].folder_color == 5793266u);
    CT_CHECK(ev.snapshot.guilds[1].id == 10 && ev.snapshot.guilds[1].folder_id == 77 &&
             strcmp(ev.snapshot.guilds[1].folder_name, "Core") == 0);
    CT_CHECK(ev.snapshot.guilds[2].id == 20 && ev.snapshot.guilds[2].folder_id == 0);
  }
  ccord_live_event_free(&ev);

  const char *us_gw =
      "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{\"guild_folders\":["
      "{\"id\":null,\"guild_ids\":[\"20\"]},"
      "{\"id\":88,\"name\":\"Gaming\",\"color\":5763719,\"guild_ids\":[\"10\",\"30\"]}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(us_gw, strlen(us_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 3);
  if (ev.ev.folder_item_count == 3) {
    CT_CHECK(ev.ev.folder_items[0].guild_id == 20 && ev.ev.folder_items[0].folder_id == 0);
    CT_CHECK(ev.ev.folder_items[1].guild_id == 10 && ev.ev.folder_items[1].folder_id == 88 &&
             strcmp(ev.ev.folder_items[1].folder_name, "Gaming") == 0 &&
             ev.ev.folder_items[1].folder_color == 5763719u);
    CT_CHECK(ev.ev.folder_items[2].guild_id == 30 && ev.ev.folder_items[2].folder_id == 88);
  }
  ccord_live_event_free(&ev);

  const char *proto_gw =
      "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"settings\":{\"type\":1,"
      "\"proto\":\"choKFAgeCAoSAgg3GgUKA09wcyIDCP8BCgIIFA==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(proto_gw, strlen(proto_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 3);
  if (ev.ev.folder_item_count == 3) {
    CT_CHECK(ev.ev.folder_items[0].guild_id == 30 && ev.ev.folder_items[0].folder_id == 55 &&
             strcmp(ev.ev.folder_items[0].folder_name, "Ops") == 0 && ev.ev.folder_items[0].folder_color == 255u);
    CT_CHECK(ev.ev.folder_items[1].guild_id == 10 && ev.ev.folder_items[1].folder_id == 55);
    CT_CHECK(ev.ev.folder_items[2].guild_id == 20 && ev.ev.folder_items[2].folder_id == 0);
  }
  ccord_live_event_free(&ev);

  const char *us_rest = "{\"guild_folders\":[{\"id\":99,\"name\":\"Dev\",\"color\":255,\"guild_ids\":[\"10\"]}]}";
  CT_CHECK(ccord_live_json_user_settings(us_rest, strlen(us_rest), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_LAYOUT && ev.ev.folder_item_count == 1 &&
           ev.ev.folder_items[0].folder_id == 99 && ev.ev.folder_items[0].folder_color == 255u);
  ccord_live_event_free(&ev);

  // 2. Invite preview & accept JSON decoder.
  const char *inv_json =
      "{\"code\":\"c23sys\",\"guild\":{\"id\":\"5001\",\"name\":\"C23 Systems\",\"icon\":\"ab12\","
      "\"description\":\"Low-level C23\"},\"channel\":{\"id\":\"6001\",\"name\":\"welcome\"},"
      "\"approximate_presence_count\":42,\"approximate_member_count\":300}";
  CT_CHECK(ccord_live_json_invite_preview(inv_json, strlen(inv_json), false, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_INVITE_PREVIEW && ev.ev.invite_preview != nullptr);
  if (ev.ev.invite_preview) {
    CT_CHECK(strcmp(ev.ev.invite_preview->code, "c23sys") == 0 && ev.ev.invite_preview->guild_id == 5001 &&
             strcmp(ev.ev.invite_preview->guild_name, "C23 Systems") == 0 &&
             strcmp(ev.ev.invite_preview->channel_name, "welcome") == 0 &&
             ev.ev.invite_preview->online_count == 42 && ev.ev.invite_preview->member_count == 300 &&
             strstr(ev.ev.invite_preview->guild_icon_url, "/icons/5001/ab12.png") != nullptr);
  }
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_invite_preview(inv_json, strlen(inv_json), true, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_INVITE_ACCEPTED && ev.ev.guild_id == 5001 && ev.ev.channel_id == 6001);
  ccord_live_event_free(&ev);

  // 3. Discoverable guilds JSON decoder.
  const char *disc_json =
      "{\"total\":99,\"guilds\":["
      "{\"id\":\"7001\",\"name\":\"Vulkan Hub\",\"description\":\"GPU programming\",\"icon\":\"a_anim\","
      "\"vanity_url_code\":\"vulkan\",\"primary_category_id\":5,"
      "\"approximate_presence_count\":1200,\"approximate_member_count\":8500}"
      "]}";
  CT_CHECK(ccord_live_json_discoverable_guilds(disc_json, strlen(disc_json), "vulkan", 5, 24, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_DISCOVERY_RESULTS && ev.ev.discovery_total == 99 &&
           ev.ev.discovery_category_id == 5 && ev.ev.discovery_offset == 24 &&
           ev.ev.discovery_query && strcmp(ev.ev.discovery_query, "vulkan") == 0 &&
           ev.ev.discovery_count == 1);
  if (ev.ev.discovery_count == 1) {
    CT_CHECK(ev.ev.discovery_guilds[0].guild_id == 7001 && strcmp(ev.ev.discovery_guilds[0].name, "Vulkan Hub") == 0 &&
             strcmp(ev.ev.discovery_guilds[0].vanity_code, "vulkan") == 0 &&
             ev.ev.discovery_guilds[0].primary_category_id == 5 &&
             ev.ev.discovery_guilds[0].online_count == 1200 && ev.ev.discovery_guilds[0].member_count == 8500 &&
             strstr(ev.ev.discovery_guilds[0].icon_url, ".gif") != nullptr);
  }
  ccord_live_event_free(&ev);
}

static void test_m3c_social_presence_requests_and_profile_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY with Group DM owner_id, message requests (is_message_request/is_spam/consent_status),
  //    personal notes, clan tags, Rich Presence, and Custom Status.
  const char *ready_m3c =
      "{\"op\":0,\"t\":\"READY\",\"d\":{"
      "\"user\":{\"id\":\"42\",\"username\":\"me\",\"primary_guild\":{\"tag\":\"CORE\"}},"
      "\"notes\":{\"501\":\"Met at conference\"},"
      "\"user_settings\":{\"custom_status\":{\"text\":\"Coding C23\",\"emoji_name\":\"\\ud83d\\udd25\","
      "\"expires_at\":\"2026-11-10T14:00:00.000Z\"}},"
      "\"guilds\":[{\"id\":\"100\",\"name\":\"HQ\",\"channels\":[],"
      "\"members\":[{\"user\":{\"id\":\"501\",\"username\":\"ada\",\"clan\":{\"tag\":\"ADA\"}}}],"
      "\"presences\":[{\"user\":{\"id\":\"501\"},\"status\":\"dnd\",\"activities\":["
      "{\"type\":4,\"state\":\"In flow\",\"emoji\":{\"name\":\"\\u26a1\"},\"timestamps\":{\"end\":1794320000000}},"
      "{\"type\":0,\"name\":\"Neovim\",\"details\":\"Editing session.c\",\"state\":\"Workspace C-Cord\","
      "\"timestamps\":{\"start\":1794310000000,\"end\":1794319000000}}"
      "]}]}],"
      "\"private_channels\":["
      "{\"id\":\"9101\",\"type\":3,\"owner_id\":\"501\",\"name\":\"Core Team\","
      "\"recipients\":[{\"id\":\"501\",\"username\":\"ada\"}]},"
      "{\"id\":\"9102\",\"type\":1,\"is_message_request\":true,\"is_spam\":false,"
      "\"recipients\":[{\"id\":\"502\",\"username\":\"stranger\"}]},"
      "{\"id\":\"9103\",\"type\":1,\"consent_status\":1,\"is_spam\":true,"
      "\"recipients\":[{\"id\":\"503\",\"username\":\"spammer\"}]},"
      "{\"id\":\"9104\",\"type\":1,\"is_message_request\":true,\"consent_status\":2,"
      "\"recipients\":[{\"id\":\"504\",\"username\":\"accepted_friend\"}]}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_m3c, strlen(ready_m3c), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT);
  CT_CHECK(ev.snapshot.note_count == 1 && ev.snapshot.notes[0].user_id == 501 &&
           strcmp(ev.snapshot.notes[0].note, "Met at conference") == 0);

  const ccord_user *u_me = nullptr, *u_ada = nullptr;
  for (size_t i = 0; i < ev.snapshot.user_count; i++) {
    if (ev.snapshot.users[i].id == 42) u_me = &ev.snapshot.users[i];
    if (ev.snapshot.users[i].id == 501) u_ada = &ev.snapshot.users[i];
  }
  CT_CHECK(u_me && strcmp(u_me->clan_tag, "CORE") == 0 && strcmp(u_me->custom_status_text, "Coding C23") == 0 &&
           u_me->custom_status_expires_ms > 0);
  CT_CHECK(u_ada && strcmp(u_ada->clan_tag, "ADA") == 0 && u_ada->presence == CCORD_PRESENCE_DND &&
           strcmp(u_ada->custom_status_text, "In flow") == 0 && u_ada->custom_status_expires_ms == 1794320000000LL &&
           u_ada->has_activity && u_ada->activity_type == CCORD_ACTIVITY_PLAYING &&
           strcmp(u_ada->activity_name, "Neovim") == 0 &&
           strcmp(u_ada->activity_details, "Editing session.c") == 0 &&
           strcmp(u_ada->activity_state, "Workspace C-Cord") == 0 &&
           u_ada->activity_start_ms == 1794310000000LL && u_ada->activity_end_ms == 1794319000000LL);

  bool saw_9101 = false, saw_9102 = false, saw_9103 = false, saw_9104 = false;
  for (size_t i = 0; i < ev.snapshot.channel_count; i++) {
    const ccord_channel *c = &ev.snapshot.channels[i];
    if (c->id == 9101) { saw_9101 = true; CT_CHECK(c->owner_id == 501 && !c->is_message_request); }
    if (c->id == 9102) { saw_9102 = true; CT_CHECK(c->is_message_request && !c->is_spam); }
    if (c->id == 9103) { saw_9103 = true; CT_CHECK(c->is_message_request && c->is_spam); }
    if (c->id == 9104) { saw_9104 = true; CT_CHECK(!c->is_message_request && !c->is_spam); }
  }
  CT_CHECK(saw_9101 && saw_9102 && saw_9103 && saw_9104);
  ccord_live_event_free(&ev);

  // 2. Gateway CHANNEL_RECIPIENT_ADD / REMOVE, USER_NOTE_UPDATE, USER_SETTINGS_UPDATE custom_status, PRESENCE_UPDATE.
  const char *rec_add =
      "{\"op\":0,\"t\":\"CHANNEL_RECIPIENT_ADD\",\"d\":{\"channel_id\":\"9101\","
      "\"user\":{\"id\":\"505\",\"username\":\"grace\",\"global_name\":\"Grace\"}}}";
  CT_CHECK(ccord_live_gateway_decode(rec_add, strlen(rec_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_DM_RECIPIENT_CHANGE && ev.ev.channel_id == 9101 &&
           ev.ev.user_id == 505 && ev.ev.recipient_added && ev.ev.user_count == 1 &&
           strcmp(ev.ev.users[0].name, "Grace") == 0);
  ccord_live_event_free(&ev);

  const char *rec_rem =
      "{\"op\":0,\"t\":\"CHANNEL_RECIPIENT_REMOVE\",\"d\":{\"channel_id\":\"9101\","
      "\"user\":{\"id\":\"505\",\"username\":\"grace\"}}}";
  CT_CHECK(ccord_live_gateway_decode(rec_rem, strlen(rec_rem), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_DM_RECIPIENT_CHANGE && ev.ev.channel_id == 9101 &&
           ev.ev.user_id == 505 && !ev.ev.recipient_added);
  ccord_live_event_free(&ev);

  const char *note_gw = "{\"op\":0,\"t\":\"USER_NOTE_UPDATE\",\"d\":{\"id\":\"501\",\"note\":\"Updated note\"}}";
  CT_CHECK(ccord_live_gateway_decode(note_gw, strlen(note_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_USER_NOTE_UPDATE && ev.ev.user_id == 501 &&
           ev.ev.text && strcmp(ev.ev.text, "Updated note") == 0);
  ccord_live_event_free(&ev);

  const char *cs_gw =
      "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{\"custom_status\":{\"text\":\"AFK\",\"emoji_name\":\"\\u2615\"}}}";
  CT_CHECK(ccord_live_gateway_decode(cs_gw, strlen(cs_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CUSTOM_STATUS_UPDATE && !ev.ev.custom_status_clear &&
           strcmp(ev.ev.custom_status_text, "AFK") == 0);
  ccord_live_event_free(&ev);

  const char *cs_clr = "{\"op\":0,\"t\":\"USER_SETTINGS_UPDATE\",\"d\":{\"custom_status\":null}}";
  CT_CHECK(ccord_live_gateway_decode(cs_clr, strlen(cs_clr), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_CUSTOM_STATUS_UPDATE && ev.ev.custom_status_clear);
  ccord_live_event_free(&ev);

  const char *pres_gw =
      "{\"op\":0,\"t\":\"PRESENCE_UPDATE\",\"d\":{\"user\":{\"id\":\"501\"},\"status\":\"online\",\"activities\":["
      "{\"type\":2,\"name\":\"Spotify\",\"details\":\"Synthwave\",\"state\":\"Midnight\","
      "\"timestamps\":{\"start\":1700000000000}},"
      "{\"type\":4,\"state\":\"Vibing\",\"emoji\":{\"name\":\"tune\",\"id\":\"888999\"}}"
      "]}}";
  CT_CHECK(ccord_live_gateway_decode(pres_gw, strlen(pres_gw), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PRESENCE_UPDATE && ev.ev.has_presence_detail &&
           ev.ev.has_activity && ev.ev.activity_type == CCORD_ACTIVITY_LISTENING &&
           strcmp(ev.ev.activity_name, "Spotify") == 0 && strcmp(ev.ev.activity_details, "Synthwave") == 0 &&
           strcmp(ev.ev.custom_status_text, "Vibing") == 0 &&
           strcmp(ev.ev.custom_status_emoji, "tune:888999") == 0);
  ccord_live_event_free(&ev);

  // 3. Gateway op 3 presence_ex builder & extended user profile JSON decoder.
  ccord_buf b;
  ccord_buf_init(&b, 0);
  CT_CHECK(ccord_gateway_build_presence_ex(CCORD_PRESENCE_IDLE, "tune", 888999, "Vibing", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"op\":3") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"status\":\"idle\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"type\":4") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"state\":\"Vibing\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"name\":\"tune\",\"id\":\"888999\"") != nullptr);
  ccord_buf_clear(&b);
  CT_CHECK(ccord_gateway_build_presence_ex(CCORD_PRESENCE_OFFLINE, nullptr, 0, nullptr, &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"status\":\"invisible\"") != nullptr &&
           strstr(ccord_buf_cstr(&b), "\"activities\":[]") != nullptr);
  ccord_buf_free(&b);

  const char *ext_prof =
      "{\"user\":{\"id\":\"501\",\"username\":\"ada\",\"global_name\":\"Ada\",\"clan\":{\"tag\":\"C23\"}},"
      "\"user_profile\":{\"bio\":\"Engine\",\"pronouns\":\"she/her\"},"
      "\"connected_accounts\":["
      "{\"type\":\"github\",\"name\":\"adalovelace\",\"verified\":true},"
      "{\"type\":\"twitch\",\"name\":\"ada_live\",\"verified\":false}"
      "],"
      "\"mutual_guilds\":[{\"id\":\"100\"},{\"id\":\"200\"}],"
      "\"mutual_friends\":[{\"id\":\"777\",\"username\":\"babbage\",\"global_name\":\"Charles\",\"avatar\":\"av1\"}],"
      "\"mutual_friends_count\":9}";
  CT_CHECK(ccord_live_json_user_profile(ext_prof, strlen(ext_prof), 501, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_USER_PROFILE && ev.ev.user_profile != nullptr);
  if (ev.ev.user_profile) {
    const ccord_user_profile *p = ev.ev.user_profile;
    CT_CHECK(strcmp(p->clan_tag, "C23") == 0 && p->connection_count == 2);
    CT_CHECK(strcmp(p->connections[0].type, "github") == 0 && strcmp(p->connections[0].name, "adalovelace") == 0 &&
             p->connections[0].verified);
    CT_CHECK(strcmp(p->connections[1].type, "twitch") == 0 && !p->connections[1].verified);
    CT_CHECK(p->mutual_guild_count == 2 && p->mutual_guild_ids[0] == 100 && p->mutual_guild_ids[1] == 200);
    CT_CHECK(p->mutual_friend_count == 1 && p->mutual_friend_total == 9 &&
             p->mutual_friends[0].user_id == 777 && strcmp(p->mutual_friends[0].name, "Charles") == 0 &&
             strcmp(p->mutual_friends[0].handle, "babbage") == 0);
  }
  ccord_live_event_free(&ev);
}

static void test_m4a_guild_settings_incidents_vanity_and_audit_changes_json(void)
{
  ccord_error err;
  ccord_live_event ev = {0};

  // 1. Guild JSON decoding: banner CDN URL (animated + static), system/AFK channels & flags, default notifications,
  // verification_level, explicit_content_filter, premium_tier/boosts, vanity_url_code, features, incidents_data.
  const char *g_json =
      "{\"id\":\"1111\",\"name\":\"C-Cord HQ\",\"owner_id\":\"42\","
      "\"icon\":\"ic_hash\",\"banner\":\"a_bn_hash\","
      "\"system_channel_id\":\"2222\",\"system_channel_flags\":11,"
      "\"default_message_notifications\":1,"
      "\"afk_channel_id\":\"2225\",\"afk_timeout\":900,"
      "\"verification_level\":3,\"explicit_content_filter\":2,"
      "\"premium_tier\":3,\"premium_subscription_count\":18,"
      "\"vanity_url_code\":\"ccord-hq\","
      "\"features\":[\"VANITY_URL\",\"BANNER\",\"COMMUNITY\",\"DISCOVERABLE\"],"
      "\"incidents_data\":{\"invites_disabled_until\":\"2026-11-12T00:00:00.000000+00:00\","
      "\"dms_disabled_until\":\"2026-11-11T06:00:00.000000+00:00\"}}";
  CT_CHECK(ccord_live_json_guild(g_json, strlen(g_json), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_UPSERT && ev.ev.guild != nullptr);
  if (ev.ev.guild) {
    const ccord_guild *g = ev.ev.guild;
    CT_CHECK(g->id == 1111 && g->owner_id == 42 && g->has_guild_settings && g->has_incidents_data);
    CT_CHECK(strstr(g->banner_url, "/banners/1111/a_bn_hash.gif") != nullptr);
    CT_CHECK(g->system_channel_id == 2222 && g->system_channel_flags == 11);
    CT_CHECK(g->default_message_notifications == 1);
    CT_CHECK(g->afk_channel_id == 2225 && g->afk_timeout == 900);
    CT_CHECK(g->verification_level == 3 && g->explicit_content_filter == 2);
    CT_CHECK(g->premium_tier == 3 && g->premium_subscription_count == 18);
    CT_CHECK(strcmp(g->vanity_url_code, "ccord-hq") == 0);
    CT_CHECK(g->feature_vanity_url && g->feature_banner && g->feature_community);
    CT_CHECK(g->invites_disabled_until_ms > 0 && g->dms_disabled_until_ms > 0 &&
             g->invites_disabled_until_ms > g->dms_disabled_until_ms);
  }
  ccord_live_event_free(&ev);

  // Gateway GUILD_UPDATE with null banner/system/afk/incidents_data clears them cleanly.
  const char *gw_gupd =
      "{\"op\":0,\"t\":\"GUILD_UPDATE\",\"d\":{\"id\":\"1111\",\"name\":\"C-Cord HQ\",\"owner_id\":\"3333\","
      "\"banner\":\"bn_static\",\"system_channel_id\":null,\"system_channel_flags\":0,"
      "\"default_message_notifications\":0,\"afk_channel_id\":null,\"afk_timeout\":300,"
      "\"verification_level\":1,\"explicit_content_filter\":0,\"premium_tier\":2,"
      "\"premium_subscription_count\":7,\"vanity_url_code\":null,\"features\":[\"BANNER\"],"
      "\"incidents_data\":null}}";
  CT_CHECK(ccord_live_gateway_decode(gw_gupd, strlen(gw_gupd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && ev.guild_count == 1 && ev.guilds != nullptr);
  if (ev.guilds && ev.guild_count == 1) {
    const ccord_guild *g = &ev.guilds[0];
    CT_CHECK(g->owner_id == 3333 && g->has_guild_settings && g->has_incidents_data);
    CT_CHECK(strstr(g->banner_url, "/banners/1111/bn_static.png") != nullptr);
    CT_CHECK(g->system_channel_id == CCORD_ID_NONE && g->afk_channel_id == CCORD_ID_NONE && g->afk_timeout == 300);
    CT_CHECK(g->vanity_url_code[0] == '\0' && !g->feature_vanity_url && g->feature_banner);
    CT_CHECK(g->invites_disabled_until_ms == 0 && g->dms_disabled_until_ms == 0);
  }
  ccord_live_event_free(&ev);

  // 2. Incident actions & vanity URL REST decoders (including null clears and malformed input).
  const char *inc_json =
      "{\"invites_disabled_until\":\"2026-11-12T12:00:00.000000+00:00\",\"dms_disabled_until\":null}";
  CT_CHECK(ccord_live_json_incident_actions(inc_json, strlen(inc_json), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_INCIDENTS && ev.ev.guild_id == 1111 &&
           ev.ev.invites_disabled_until_ms > 0 && ev.ev.dms_disabled_until_ms == 0);
  ccord_live_event_free(&ev);

  CT_CHECK(ccord_live_json_incident_actions("null", 4, 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_INCIDENTS &&
           ev.ev.invites_disabled_until_ms == 0 && ev.ev.dms_disabled_until_ms == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_incident_actions("{bad", 4, 1111, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_incident_actions("{}", 2, CCORD_ID_NONE, &ev, &err) == CCORD_ERR_INVALID);

  const char *van_json = "{\"code\":\"systems-c23\",\"uses\":42}";
  CT_CHECK(ccord_live_json_vanity_url(van_json, strlen(van_json), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_VANITY && ev.ev.guild_id == 1111 &&
           ev.ev.invite_code && strcmp(ev.ev.invite_code, "systems-c23") == 0);
  ccord_live_event_free(&ev);

  const char *van_null = "{\"code\":null}";
  CT_CHECK(ccord_live_json_vanity_url(van_null, strlen(van_null), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_VANITY && (!ev.ev.invite_code || ev.ev.invite_code[0] == '\0'));
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_vanity_url("[]", 2, 1111, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_vanity_url("{}", 2, CCORD_ID_NONE, &ev, &err) == CCORD_ERR_INVALID);

  // 3. Structured audit log changes[] decoding (strings, ints, bools, nulls, arrays/objects).
  const char *aud_json =
      "{\"audit_log_entries\":[{\"id\":\"900001\",\"user_id\":\"42\",\"target_id\":\"1111\",\"action_type\":1,"
      "\"reason\":\"Lockdown\",\"changes\":["
      "{\"key\":\"name\",\"old_value\":\"Old\",\"new_value\":\"New\"},"
      "{\"key\":\"verification_level\",\"old_value\":1,\"new_value\":3},"
      "{\"key\":\"widget_enabled\",\"old_value\":false,\"new_value\":true},"
      "{\"key\":\"vanity_url_code\",\"old_value\":\"oldslug\",\"new_value\":null},"
      "{\"key\":\"$add\",\"new_value\":[{\"id\":\"1\",\"name\":\"Mod\"}]}"
      "]}],\"users\":[{\"id\":\"42\",\"username\":\"admin\"}]}";
  CT_CHECK(ccord_live_json_audit_log(aud_json, strlen(aud_json), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUDIT_LOG && ev.ev.audit_count == 1 && ev.ev.audit != nullptr);
  if (ev.ev.audit && ev.ev.audit_count == 1) {
    const ccord_audit_entry *a = &ev.ev.audit[0];
    CT_CHECK(a->change_count == 5);
    CT_CHECK(strcmp(a->changes[0].key, "name") == 0 && strcmp(a->changes[0].old_val, "Old") == 0 &&
             strcmp(a->changes[0].new_val, "New") == 0);
    CT_CHECK(strcmp(a->changes[1].key, "verification_level") == 0 && strcmp(a->changes[1].old_val, "1") == 0 &&
             strcmp(a->changes[1].new_val, "3") == 0);
    CT_CHECK(strcmp(a->changes[2].key, "widget_enabled") == 0 && strcmp(a->changes[2].old_val, "no") == 0 &&
             strcmp(a->changes[2].new_val, "yes") == 0);
    CT_CHECK(strcmp(a->changes[3].key, "vanity_url_code") == 0 && strcmp(a->changes[3].old_val, "oldslug") == 0 &&
             strcmp(a->changes[3].new_val, "none") == 0);
    CT_CHECK(strcmp(a->changes[4].key, "$add") == 0 && a->changes[4].old_val[0] == '\0' &&
             strcmp(a->changes[4].new_val, "1 items") == 0);
  }
  ccord_live_event_free(&ev);
}

static void test_m4b_expressions_json_and_gateway(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. REST decoders: guild emojis (array + single object + uploader info).
  const char *em_arr =
      "[{\"id\":\"501\",\"name\":\"wave\",\"animated\":false,\"available\":true,"
      "\"user\":{\"id\":\"42\",\"username\":\"you_h\",\"global_name\":\"You\"}},"
      "{\"id\":\"502\",\"name\":\"spin\",\"animated\":true,\"user\":{\"id\":\"43\",\"username\":\"ada\"}}]";
  CT_CHECK(ccord_live_json_guild_emojis(em_arr, strlen(em_arr), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && ev.ev.guild_id == 1111 &&
           ev.ev.expressions_admin && ev.ev.replace_emojis && ev.ev.emoji_count == 2);
  CT_CHECK(ev.ev.emojis[0].id == 501 && ev.ev.emojis[0].user_id == 42 &&
           strcmp(ev.ev.emojis[0].user_name, "You") == 0);
  CT_CHECK(ev.ev.emojis[1].id == 502 && ev.ev.emojis[1].animated && ev.ev.emojis[1].user_id == 43 &&
           strcmp(ev.ev.emojis[1].user_name, "ada") == 0);
  ccord_live_event_free(&ev);

  const char *em_one = "{\"id\":\"503\",\"name\":\"fresh\",\"animated\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}}";
  CT_CHECK(ccord_live_json_guild_emojis(em_one, strlen(em_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_EMOJI_UPSERT && ev.ev.emoji_count == 1 &&
           ev.ev.emojis[0].id == 503 && ev.ev.emojis[0].user_id == 42);
  ccord_live_event_free(&ev);

  // 2. REST decoders: guild stickers (array + single object).
  const char *st_arr =
      "[{\"id\":\"601\",\"name\":\"Salute\",\"description\":\"o7\",\"tags\":\"salute\",\"format_type\":2,"
      "\"available\":true,\"user\":{\"id\":\"42\",\"global_name\":\"You\"}}]";
  CT_CHECK(ccord_live_json_guild_stickers(st_arr, strlen(st_arr), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && ev.ev.expressions_admin && ev.ev.replace_stickers &&
           ev.ev.sticker_count == 1 && ev.ev.stickers[0].format == CCORD_STICKER_APNG &&
           ev.ev.stickers[0].user_id == 42 && strcmp(ev.ev.stickers[0].user_name, "You") == 0);
  ccord_live_event_free(&ev);

  const char *st_one =
      "{\"id\":\"602\",\"name\":\"Vec\",\"description\":null,\"tags\":\"vec\",\"format_type\":3,\"available\":true}";
  CT_CHECK(ccord_live_json_guild_stickers(st_one, strlen(st_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_STICKER_UPSERT && ev.ev.sticker_count == 1 &&
           ev.ev.stickers[0].format == CCORD_STICKER_LOTTIE);
  ccord_live_event_free(&ev);

  // 3. REST decoders: guild soundboard ({"items":[...]} + array + single object + volume rounding/clamping).
  const char *sb_items =
      "{\"items\":[{\"sound_id\":\"701\",\"name\":\"Airhorn\",\"volume\":0.75,\"emoji_name\":\"📯\","
      "\"available\":true,\"user\":{\"id\":\"42\",\"username\":\"me\"}},"
      "{\"sound_id\":\"702\",\"name\":\"Loud\",\"volume\":1.8,\"available\":true}]}";
  CT_CHECK(ccord_live_json_guild_soundboard(sb_items, strlen(sb_items), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && ev.ev.expressions_admin && ev.ev.replace_soundboard &&
           ev.ev.soundboard_count == 2);
  CT_CHECK(ev.ev.soundboard[0].id == 701 && ev.ev.soundboard[0].volume_pct == 75 &&
           ev.ev.soundboard[0].user_id == 42 && strcmp(ev.ev.soundboard[0].emoji_name, "📯") == 0);
  CT_CHECK(ev.ev.soundboard[1].id == 702 && ev.ev.soundboard[1].volume_pct == 100);
  ccord_live_event_free(&ev);

  const char *sb_one = "{\"sound_id\":\"703\",\"name\":\"Rimshot\",\"volume\":0.25,\"emoji_id\":\"501\",\"available\":true}";
  CT_CHECK(ccord_live_json_guild_soundboard(sb_one, strlen(sb_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_UPSERT && ev.ev.soundboard_count == 1 &&
           ev.ev.soundboard[0].id == 703 && ev.ev.soundboard[0].volume_pct == 25 &&
           ev.ev.soundboard[0].emoji_id == 501);
  ccord_live_event_free(&ev);

  // 4. Gateway dispatches: GUILD_SOUNDBOARD_SOUNDS_UPDATE / SOUND_CREATE / SOUND_UPDATE / SOUND_DELETE.
  const char *gw_list =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUNDS_UPDATE\",\"d\":{\"guild_id\":\"1111\","
      "\"soundboard_sounds\":[{\"sound_id\":\"710\",\"name\":\"Bell\",\"volume\":0.5}]}}";
  CT_CHECK(ccord_live_gateway_decode(gw_list, strlen(gw_list), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_EXPRESSIONS && !ev.ev.expressions_admin && ev.ev.replace_soundboard &&
           ev.ev.soundboard_count == 1 && ev.ev.soundboard[0].id == 710 && ev.ev.soundboard[0].volume_pct == 50);
  ccord_live_event_free(&ev);

  const char *gw_create =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUND_CREATE\",\"d\":{\"guild_id\":\"1111\",\"sound_id\":\"711\",\"name\":\"Gong\",\"volume\":1.0}}";
  CT_CHECK(ccord_live_gateway_decode(gw_create, strlen(gw_create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_UPSERT && ev.ev.guild_id == 1111 &&
           ev.ev.soundboard_count == 1 && ev.ev.soundboard[0].id == 711);
  ccord_live_event_free(&ev);

  const char *gw_upd =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUND_UPDATE\",\"d\":{\"guild_id\":\"1111\",\"sound_id\":\"711\",\"name\":\"Gong2\",\"volume\":0.4}}";
  CT_CHECK(ccord_live_gateway_decode(gw_upd, strlen(gw_upd), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_UPSERT && ev.ev.soundboard[0].volume_pct == 40 &&
           strcmp(ev.ev.soundboard[0].name, "Gong2") == 0);
  ccord_live_event_free(&ev);

  const char *gw_del =
      "{\"op\":0,\"t\":\"GUILD_SOUNDBOARD_SOUND_DELETE\",\"d\":{\"guild_id\":\"1111\",\"sound_id\":\"711\"}}";
  CT_CHECK(ccord_live_gateway_decode(gw_del, strlen(gw_del), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SOUNDBOARD_DELETE && ev.ev.guild_id == 1111 && ev.ev.expr_id == 711);
  ccord_live_event_free(&ev);
}

static void test_m4c_members_prune_and_automod_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. Gateway GUILD_MEMBER_ADD decodes ISO-8601 joined_at -> joined_at_ms > 0:
  const char *mem_add =
      "{\"op\":0,\"t\":\"GUILD_MEMBER_ADD\",\"d\":{\"guild_id\":\"1111\","
      "\"user\":{\"id\":\"3333\",\"username\":\"ada\",\"global_name\":\"Ada\"},"
      "\"roles\":[\"7001\"],\"joined_at\":\"2026-01-15T08:30:00.000000+00:00\"}}";
  CT_CHECK(ccord_live_gateway_decode(mem_add, strlen(mem_add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_MEMBERS && ev.ev.guild_member_count == 1 &&
           ev.ev.guild_members[0].user_id == 3333 && ev.ev.guild_members[0].joined_at_ms > 1700000000000LL);
  ccord_live_event_free(&ev);

  // 2. REST prune decoders (preview integer + execute null):
  const char *pr_prev = "{\"pruned\":5}";
  CT_CHECK(ccord_live_json_prune(pr_prev, strlen(pr_prev), 1111, 14, false, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PRUNE_RESULT && ev.ev.guild_id == 1111 && ev.ev.prune_info.has_result &&
           !ev.ev.prune_info.executed && ev.ev.prune_info.days == 14 && ev.ev.prune_info.pruned == 5);
  ccord_live_event_free(&ev);

  const char *pr_null = "{\"pruned\":null}";
  CT_CHECK(ccord_live_json_prune(pr_null, strlen(pr_null), 1111, 7, true, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_PRUNE_RESULT && ev.ev.prune_info.executed && ev.ev.prune_info.pruned == -1);
  ccord_live_event_free(&ev);

  // 3. REST AutoMod rules decoder (array -> AUTOMOD_LIST, single object -> AUTOMOD_UPSERT):
  const char *am_arr =
      "[{\"id\":\"8801\",\"guild_id\":\"1111\",\"name\":\"Scam Links\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":1,\"enabled\":true,"
      "\"trigger_metadata\":{\"keyword_filter\":[\"free nitro\",\"*scam*\"],"
      "\"regex_patterns\":[\"d[i1]scord\\\\.gift\"],\"allow_list\":[\"discord.com\"]},"
      "\"actions\":[{\"type\":1,\"metadata\":{\"custom_message\":\"Blocked scam\"}},"
      "{\"type\":2,\"metadata\":{\"channel_id\":\"2222\"}},{\"type\":3,\"metadata\":{\"duration_seconds\":300}}],"
      "\"exempt_roles\":[\"7001\"],\"exempt_channels\":[\"2223\"]},"
      "{\"id\":\"8802\",\"guild_id\":\"1111\",\"name\":\"Presets\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":4,\"enabled\":false,"
      "\"trigger_metadata\":{\"presets\":[1,3],\"allow_list\":[\"ok\"]},"
      "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]},"
      "{\"id\":\"8803\",\"guild_id\":\"1111\",\"name\":\"Mention Cap\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":5,\"enabled\":true,"
      "\"trigger_metadata\":{\"mention_total_limit\":8,\"mention_raid_protection_enabled\":true},"
      "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}]";
  CT_CHECK(ccord_live_json_automod_rules(am_arr, strlen(am_arr), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_LIST && ev.ev.guild_id == 1111 && ev.ev.automod_rule_count == 3);
  const ccord_automod_rule *r0 = &ev.ev.automod_rules[0];
  CT_CHECK(r0->id == 8801 && r0->creator_id == 42 && r0->trigger_type == CCORD_AUTOMOD_TRIGGER_KEYWORD && r0->enabled);
  CT_CHECK(r0->keyword_count == 2 && strcmp(r0->keywords[0], "free nitro") == 0);
  CT_CHECK(r0->regex_count == 1 && strcmp(r0->regex_patterns[0], "d[i1]scord\\.gift") == 0);
  CT_CHECK(r0->allow_count == 1 && strcmp(r0->allow_list[0], "discord.com") == 0);
  CT_CHECK(r0->action_block && strcmp(r0->custom_message, "Blocked scam") == 0);
  CT_CHECK(r0->action_alert && r0->alert_channel_id == 2222);
  CT_CHECK(r0->action_timeout && r0->timeout_seconds == 300);
  CT_CHECK(r0->exempt_role_count == 1 && r0->exempt_role_ids[0] == 7001);
  CT_CHECK(r0->exempt_channel_count == 1 && r0->exempt_channel_ids[0] == 2223);
  CT_CHECK(ev.ev.automod_rules[1].presets == (CCORD_AUTOMOD_PRESET_PROFANITY | CCORD_AUTOMOD_PRESET_SLURS));
  CT_CHECK(ev.ev.automod_rules[2].mention_total_limit == 8 && ev.ev.automod_rules[2].mention_raid_protection);
  ccord_live_event_free(&ev);

  const char *am_one =
      "{\"id\":\"8804\",\"guild_id\":\"1111\",\"name\":\"Spam\",\"creator_id\":\"42\","
      "\"event_type\":1,\"trigger_type\":3,\"enabled\":true,\"trigger_metadata\":{},"
      "\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}";
  CT_CHECK(ccord_live_json_automod_rules(am_one, strlen(am_one), 1111, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_UPSERT && ev.ev.automod_rule_count == 1 && ev.ev.automod_rules[0].id == 8804);
  ccord_live_event_free(&ev);

  // 4. Gateway AUTO_MODERATION_RULE_CREATE / UPDATE / DELETE:
  const char *gw_am_c =
      "{\"op\":0,\"t\":\"AUTO_MODERATION_RULE_CREATE\",\"d\":{\"id\":\"8810\",\"guild_id\":\"1111\","
      "\"name\":\"GW Rule\",\"creator_id\":\"42\",\"event_type\":1,\"trigger_type\":3,\"enabled\":true,"
      "\"trigger_metadata\":{},\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gw_am_c, strlen(gw_am_c), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_UPSERT && ev.ev.guild_id == 1111 && ev.ev.automod_rules[0].id == 8810);
  ccord_live_event_free(&ev);

  const char *gw_am_u =
      "{\"op\":0,\"t\":\"AUTO_MODERATION_RULE_UPDATE\",\"d\":{\"id\":\"8810\",\"guild_id\":\"1111\","
      "\"name\":\"GW Rule Off\",\"creator_id\":\"42\",\"event_type\":1,\"trigger_type\":3,\"enabled\":false,"
      "\"trigger_metadata\":{},\"actions\":[{\"type\":1,\"metadata\":{}}],\"exempt_roles\":[],\"exempt_channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(gw_am_u, strlen(gw_am_u), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_UPSERT && !ev.ev.automod_rules[0].enabled &&
           strcmp(ev.ev.automod_rules[0].name, "GW Rule Off") == 0);
  ccord_live_event_free(&ev);

  const char *gw_am_d =
      "{\"op\":0,\"t\":\"AUTO_MODERATION_RULE_DELETE\",\"d\":{\"id\":\"8810\",\"guild_id\":\"1111\"}}";
  CT_CHECK(ccord_live_gateway_decode(gw_am_d, strlen(gw_am_d), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_AUTOMOD_DELETE && ev.ev.guild_id == 1111 && ev.ev.expr_id == 8810);
  ccord_live_event_free(&ev);
}

static void test_interaction_subcommands_and_context_menus(void)
{
  ccord_error err;
  ccord_buf b;
  ccord_buf_init(&b, 0);

  // 1. Subcommand (type 1 wrapper around leaf options; command_type=0 defaults to 1).
  const char *sub_names[2] = {"count", "label"};
  uint8_t sub_kinds[2] = {CCORD_SLASH_OPT_INTEGER, CCORD_SLASH_OPT_STRING};
  const char *sub_vals[2] = {"7", "alpha"};
  ccord_interaction_request sub_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9101,
    .command_version = 9102,
    .command_type = 0,
    .command_name = "ticket",
    .subcommand = "open",
    .option_names = sub_names,
    .option_kinds = sub_kinds,
    .option_values = sub_vals,
    .option_count = 2,
    .focused_option = -1,
  };
  CT_CHECK(ccord_live_build_interaction_body(&sub_req, "sess-sub", "3001", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"version\":\"9102\",\"id\":\"9101\",\"name\":\"ticket\",\"type\":1") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b),
                  "\"options\":[{\"type\":1,\"name\":\"open\",\"options\":["
                  "{\"type\":4,\"name\":\"count\",\"value\":7},"
                  "{\"type\":3,\"name\":\"label\",\"value\":\"alpha\"}]}]") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"target_id\"") == nullptr);

  // 2. Subcommand group + subcommand + autocomplete focus inside nested leaf options.
  const char *grp_names[1] = {"role"};
  uint8_t grp_kinds[1] = {CCORD_SLASH_OPT_STRING};
  const char *grp_vals[1] = {"mod"};
  ccord_interaction_request grp_req = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9103,
    .command_type = 1,
    .command_name = "config",
    .subcommand_group = "permissions",
    .subcommand = "grant",
    .option_names = grp_names,
    .option_kinds = grp_kinds,
    .option_values = grp_vals,
    .option_count = 1,
    .focused_option = 0,
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&grp_req, "sess-grp", "3002", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"type\":4") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b),
                  "\"options\":[{\"type\":2,\"name\":\"permissions\",\"options\":["
                  "{\"type\":1,\"name\":\"grant\",\"options\":["
                  "{\"type\":3,\"name\":\"role\",\"value\":\"mod\",\"focused\":true}]}]}]") != nullptr);

  // 3. Subcommand with zero leaf options emits empty inner options array.
  ccord_interaction_request empty_sub = sub_req;
  empty_sub.subcommand = "list";
  empty_sub.option_count = 0;
  empty_sub.option_names = nullptr;
  empty_sub.option_kinds = nullptr;
  empty_sub.option_values = nullptr;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&empty_sub, "sess-sub", "3003", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"options\":[{\"type\":1,\"name\":\"list\",\"options\":[]}]") != nullptr);

  // 4. User (type 2) and Message (type 3) context-menu commands allow spaces in name and emit target_id.
  ccord_interaction_request user_ctx = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9201,
    .command_type = 2,
    .command_name = "View User Info",
    .target_id = 3333,
    .focused_option = -1,
  };
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&user_ctx, "sess-ctx", "3004", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"name\":\"View User Info\",\"type\":2") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"target_id\":\"3333\"") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"options\":[]") != nullptr);

  ccord_interaction_request msg_ctx = user_ctx;
  msg_ctx.command_id = 9202;
  msg_ctx.command_type = 3;
  msg_ctx.command_name = "Bookmark Message";
  msg_ctx.target_id = 8888;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&msg_ctx, "sess-ctx", "3005", &b, &err) == CCORD_OK);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"name\":\"Bookmark Message\",\"type\":3") != nullptr);
  CT_CHECK(strstr(ccord_buf_cstr(&b), "\"target_id\":\"8888\"") != nullptr);

  // 5. Validation gates: spaces rejected for CHAT_INPUT, missing target_id or extra options rejected for context menus,
  // and subcommand_group without subcommand rejected.
  ccord_interaction_request bad = sub_req;
  bad.command_name = "has space";
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = user_ctx;
  bad.target_id = CCORD_ID_NONE;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = msg_ctx;
  bad.option_names = grp_names;
  bad.option_kinds = grp_kinds;
  bad.option_values = grp_vals;
  bad.option_count = 1;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = grp_req;
  bad.type = CCORD_INTERACTION_APPLICATION_COMMAND;
  bad.focused_option = -1;
  bad.subcommand = nullptr;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  bad = sub_req;
  bad.target_id = 3333;
  ccord_buf_clear(&b);
  CT_CHECK(ccord_live_build_interaction_body(&bad, "s", "1", &b, &err) == CCORD_ERR_INVALID);

  ccord_buf_free(&b);
}

typedef struct hook_fx_cfg {
  char initial_proto_json[512];
  const fd_server *srv;   // the server forks inside fd_start, so its bound port (srv->port) is what the hook can see
} hook_fx_cfg;

static size_t respond_hook_fx(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)idx;
  const hook_fx_cfg *cfg = ud;
  const char *m = req->method;
  const char *p = req->path;
  bool get = strcmp(m, "GET") == 0;
  bool post = strcmp(m, "POST") == 0;
  bool patch = strcmp(m, "PATCH") == 0;
  bool put = strcmp(m, "PUT") == 0;
  bool del = strcmp(m, "DELETE") == 0;

  if (post && strcmp(p, "/channels/2222/attachments") == 0) {
    char slot[256];
    (void)snprintf(slot, sizeof slot,
                   "{\"attachments\":[{\"id\":0,\"upload_url\":\"http://127.0.0.1:%d/upload/0\","
                   "\"upload_filename\":\"u0/voice-message.ogg\"}]}", cfg->srv->port);
    return fd_reply(out, cap, 200, "OK", nullptr, slot);
  }
  if (put && strcmp(p, "/upload/0") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "");
  }
  if (post && strcmp(p, "/channels/2222/messages") == 0) {
    if (strstr(req->body, "\"duration_secs\"") != nullptr) {
      // Echo what Discord would: the voice flag and the attachment's length + waveform.
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"9110\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                      "\"content\":\"\",\"flags\":8192,\"timestamp\":\"2026-11-10T12:00:09Z\","
                      "\"attachments\":[{\"id\":\"77\",\"filename\":\"voice-message.ogg\","
                      "\"url\":\"https://cdn.example/voice-message.ogg\",\"content_type\":\"audio/ogg\","
                      "\"size\":4,\"duration_secs\":2.5,\"waveform\":\"AAECAwQFBgcICQ==\"}]}");
    }
    if (strstr(req->body, "\"tts\":true") != nullptr) {
      return fd_reply(out, cap, 200, "OK", nullptr,
                      "{\"id\":\"9100\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                      "\"content\":\"heads up\",\"tts\":true,\"timestamp\":\"2026-11-10T12:00:00Z\"}");
    }
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9102\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"\",\"timestamp\":\"2026-11-10T12:00:02Z\"}");
  }
  if (post && strcmp(p, "/channels/4444/messages") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9101\",\"channel_id\":\"4444\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"\",\"timestamp\":\"2026-11-10T12:00:01Z\"}");
  }
  if (post && strcmp(p, "/channels/2222/messages/9003/crosspost") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"9003\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"announcement\",\"flags\":1,\"timestamp\":\"2026-11-10T12:00:03Z\"}");
  }
  if (post && (strcmp(p, "/channels/2222/messages/9004/ack") == 0 ||
               strcmp(p, "/channels/2222/messages/8989/ack") == 0 ||
               strcmp(p, "/channels/4444/messages/9101/ack") == 0)) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"token\":null}");
  }
  if (post && strcmp(p, "/channels/4444/messages/9100/ack") == 0) {
    return fd_reply(out, cap, 500, "Internal Server Error", nullptr, "{\"message\":\"ack failed\"}");
  }
  if (get && strcmp(p, "/users/@me/settings-proto/2") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, cfg->initial_proto_json);
  }
  if (patch && strcmp(p, "/users/@me/settings-proto/2") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, req->body);
  }
  if (post && strcmp(p, "/interactions") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (post && strcmp(p, "/stage-instances") == 0) {
    return fd_reply(out, cap, 201, "Created", nullptr,
                    "{\"id\":\"6601\",\"guild_id\":\"1111\",\"channel_id\":\"2223\","
                    "\"topic\":\"Town Hall\",\"privacy_level\":2}");
  }
  if (del && strcmp(p, "/stage-instances/2223") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/guilds/1111/scheduled-events?with_user_count=true") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"7701\",\"guild_id\":\"1111\",\"channel_id\":\"2223\",\"creator_id\":\"42\","
                    "\"name\":\"Town Hall Event\",\"description\":\"Q&A\","
                    "\"scheduled_start_time\":\"2026-11-15T18:00:00.000Z\","
                    "\"privacy_level\":2,\"status\":1,\"entity_type\":1,\"user_count\":5}]");
  }
  if (post && strcmp(p, "/guilds/1111/scheduled-events") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"7702\",\"guild_id\":\"1111\",\"channel_id\":\"2223\",\"creator_id\":\"42\","
                    "\"name\":\"Launch Party\",\"description\":\"Bring snacks\","
                    "\"scheduled_start_time\":\"2026-11-16T18:00:00.000Z\","
                    "\"privacy_level\":2,\"status\":1,\"entity_type\":1,\"user_count\":1}");
  }
  if (patch && strcmp(p, "/guilds/1111/scheduled-events/7702") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"id\":\"7702\",\"guild_id\":\"1111\",\"channel_id\":\"2223\",\"creator_id\":\"42\","
                    "\"name\":\"Launch Party Live\",\"description\":\"Bring snacks\","
                    "\"scheduled_start_time\":\"2026-11-16T18:00:00.000Z\","
                    "\"privacy_level\":2,\"status\":2,\"entity_type\":1,\"user_count\":4}");
  }
  if ((put || del) && strcmp(p, "/guilds/1111/scheduled-events/7702/users/@me") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, "{}");
  }
  if (del && strcmp(p, "/guilds/1111/scheduled-events/7702") == 0) {
    return fd_reply(out, cap, 204, "No Content", nullptr, "");
  }
  if (get && strcmp(p, "/guilds/1111/member-verification?with_guild=true") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"version\":\"2026-11-01T00:00:00.000000+00:00\",\"description\":\"Town rules\","
                    "\"form_fields\":[{\"field_type\":\"TERMS\",\"label\":\"Rules\",\"required\":true,"
                    "\"values\":[\"Be kind\",\"No spam\"]}],\"guild\":{\"id\":\"1111\",\"name\":\"Town\"}}");
  }
  if (put && strcmp(p, "/guilds/1111/requests/@me") == 0) {
    // A form labelled "silent" gets the bare 204; anything else gets the member object back with pending cleared.
    if (strstr(req->body, "\"label\":\"silent\"") != nullptr) return fd_reply(out, cap, 204, "No Content", nullptr, "");
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[],\"pending\":false}");
  }
  if (get && strncmp(p, "/channels/2222/messages?limit=", 30) == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr,
                    "[{\"id\":\"9004\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"after\",\"timestamp\":\"2026-11-10T11:59:00Z\"},"
                    "{\"id\":\"9003\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"announcement\",\"timestamp\":\"2026-11-10T11:58:00Z\"},"
                    "{\"id\":\"8990\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
                    "\"content\":\"earliest\",\"timestamp\":\"2026-11-10T11:57:00Z\"}]");
  }
  if (get && strncmp(p, "/channels/", 10) == 0 && strstr(p, "/messages?limit=") != nullptr) {
    return fd_reply(out, cap, 200, "OK", nullptr, "[]");
  }
  return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"unhandled route\"}");
}

static bool poll_live_for(ccord_transport *t, ccord_net_event_type want, ccord_net_event *out, uint64_t timeout_ms)
{
  uint64_t end = ccord_clock_ms() + timeout_ms;
  while (ccord_clock_ms() < end) {
    if (ccord_transport_poll(t, out) && out->type == want) return true;
    usleep(2000);
  }
  return false;
}

static const fd_hit *find_hit(const fd_hit *hits, size_t n, const char *method, const char *path, size_t *cursor)
{
  size_t start = cursor ? *cursor : 0;
  for (size_t i = start; i < n; i++) {
    if (strcmp(hits[i].method, method) == 0 && strcmp(hits[i].path, path) == 0) {
      if (cursor) *cursor = i + 1;
      return &hits[i];
    }
  }
  return nullptr;
}

static void test_loopback_messaging_proto_stage_and_events(void)
{
  ccord_error err;
  char cache_dir[512];
  if (!make_temp(cache_dir, sizeof cache_dir)) {
    fprintf(stderr, "test_live_transport: no TMPDIR, skipping loopback hook test\n");
    return;
  }

  // Seed initial settings-proto/2 with an unknown wire field (field 99 varint = 0x42 -> 0x98,0x06,0x42, base64 "mAZC")
  // plus one favorite GIF so we can verify raw_proto exposure and lossless round-trip across GET -> PATCH -> cache.
  const uint8_t unknown_seed[] = {0x98u, 0x06u, 0x42u};
  ccord_proto_gif init_gif = {
    .url = "https://tenor.com/view/cat-1",
    .src = "https://media.tenor.com/cat-1.gif",
    .format = 1,
    .width = 320,
    .height = 240,
    .order = 1,
  };
  uint8_t *init_raw = nullptr;
  size_t init_raw_len = 0;
  char *init_json = nullptr;
  CT_CHECK(ccord_user_expressions_encode_gifs(&init_gif, 1, unknown_seed, sizeof unknown_seed, &init_raw, &init_raw_len,
                                              &err) == CCORD_OK);
  const char *seed_json = "{\"settings\":\"mAZC\"}";
  CT_CHECK(ccord_user_expressions_encode_gifs_json(&init_gif, 1, seed_json, strlen(seed_json), &init_json, &err) ==
           CCORD_OK);

  hook_fx_cfg fx_cfg = {0};
  CT_CHECK(init_json != nullptr && strlen(init_json) < sizeof fx_cfg.initial_proto_json);
  if (init_json) snprintf(fx_cfg.initial_proto_json, sizeof fx_cfg.initial_proto_json, "%s", init_json);
  ccord_free(init_json);

  fd_server srv;
  fx_cfg.srv = &srv;
  CT_CHECK(fd_start(&srv, respond_hook_fx, &fx_cfg));

  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = srv.base,
    .disable_gateway = true,
    .request_timeout_ms = 3000,
  };
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);

  ccord_net_event ev;
  CT_CHECK(poll_live_for(t, CCORD_NET_STATE, &ev, 3000) && ev.state == CCORD_CONN_CONNECTED);

  // Register guild-channel mappings (1111 -> 2222, 1111 -> 2223) and a DM channel (4444), and load 2222 history.
  CT_CHECK(ccord_transport_select_channel(t, 1111, 2222, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_select_channel(t, 1111, 2223, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_select_channel(t, CCORD_DM_GUILD_ID, 4444, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_request_history(t, 2222, 50, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_HISTORY, &ev, 3000) && ev.message_count == 3);

  // 1. send_tts_message -> POST /channels/2222/messages with "tts":true -> CCORD_NET_MESSAGE_CREATE.
  CT_CHECK(ccord_transport_send_tts_message(t, 2222, "heads up", 8, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9100 && ev.messages[0].tts &&
           strcmp(ev.messages[0].content, "heads up") == 0);

  // 2. forward_message: guild source includes guild_id; DM source omits guild_id.
  CT_CHECK(ccord_transport_forward_message(t, 4444, 2222, 9001, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9101 && ev.channel_id == 4444);

  CT_CHECK(ccord_transport_forward_message(t, 2222, 4444, 9002, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9102 && ev.channel_id == 2222);

  // 3. crosspost_message -> POST /channels/2222/messages/9003/crosspost -> CCORD_NET_MESSAGE_UPDATE.
  CT_CHECK(ccord_transport_crosspost_message(t, 2222, 9003, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_UPDATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9003 &&
           (ccord_message_flags(&ev.messages[0]) & CCORD_MSGFLAG_CROSSPOSTED) != 0);

  // 4. mark_unread acks the message BEFORE the target:
  //    - channel 2222 has [8990, 9003, 9004, 9100, 9102], so marking 9100 unread acks 9004 (never 9100);
  //    - marking earliest message 8990 unread acks 8990 - 1 == 8989;
  //    - snowflake <= 1 has no preceding message and returns CCORD_ERR_INVALID;
  //    - a failed mark_unread emits ACTION_FAILED and reconciles MESSAGE_ACK back to last_ack_id.
  CT_CHECK(ccord_transport_mark_unread(t, 2222, 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_transport_mark_unread(t, 2222, 9100, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_mark_unread(t, 2222, 8990, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_ack_message(t, 4444, 9101, &err) == CCORD_OK);
  for (int i = 0; i < 30; i++) {
    (void)ccord_transport_poll(t, &ev);
    usleep(3000);
  }
  CT_CHECK(ccord_transport_mark_unread(t, 4444, 9101, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_ACTION_FAILED, &ev, 3000));
  CT_CHECK(ev.channel_id == 4444 && ev.message_id == 9101);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_ACK, &ev, 3000));
  CT_CHECK(ev.channel_id == 4444 && ev.message_id == 9101);

  // 5. settings-proto/2 GET -> raw_proto + PATCH -> raw_proto.
  CT_CHECK(ccord_transport_request_expression_data(t, CCORD_EXPR_SETTINGS_PROTO, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_EXPRESSION_DATA, &ev, 3000));
  CT_CHECK(ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.expr_body != nullptr && ev.expr_body_len > 0);
  CT_CHECK(ev.raw_proto != nullptr && ev.raw_proto_len == init_raw_len &&
           memcmp(ev.raw_proto, init_raw, init_raw_len) == 0);
  ccord_user_expressions ux = {0};
  CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &ux, &err) == CCORD_OK && ux.gif_count == 1);
  ccord_user_expressions_free(&ux);

  ccord_proto_gif next_gifs[2] = {
    init_gif,
    {
      .url = "https://tenor.com/view/dog-2",
      .src = "https://media.tenor.com/dog-2.mp4",
      .format = 2,
      .width = 400,
      .height = 300,
      .order = 2,
    },
  };
  uint8_t *patched_raw = nullptr;
  size_t patched_raw_len = 0;
  CT_CHECK(ccord_user_expressions_encode_gifs(next_gifs, 2, ev.raw_proto, ev.raw_proto_len, &patched_raw,
                                              &patched_raw_len, &err) == CCORD_OK);
  CT_CHECK(patched_raw != nullptr && patched_raw_len > sizeof unknown_seed &&
           memcmp(patched_raw, unknown_seed, sizeof unknown_seed) == 0);

  CT_CHECK(ccord_transport_patch_settings_proto(t, patched_raw, patched_raw_len, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_EXPRESSION_DATA, &ev, 3000));
  CT_CHECK(ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.raw_proto != nullptr &&
           ev.raw_proto_len == patched_raw_len && memcmp(ev.raw_proto, patched_raw, patched_raw_len) == 0);
  CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &ux, &err) == CCORD_OK && ux.gif_count == 2);
  ccord_user_expressions_free(&ux);

  // 6. send_interaction for Message context-menu command over loopback.
  ccord_interaction_request ireq = {
    .type = CCORD_INTERACTION_APPLICATION_COMMAND,
    .application_id = 777,
    .guild_id = 1111,
    .channel_id = 2222,
    .command_id = 9202,
    .command_type = 3,
    .command_name = "Pin Fast",
    .target_id = 9003,
    .focused_option = -1,
  };
  CT_CHECK(ccord_transport_send_interaction(t, &ireq, &err) == CCORD_OK);

  // 7. Stage instance create & delete (channel 2223 belongs to guild 1111 via chan_guilds).
  // Select DM channel first so selected_guild is DM, proving STAGE_DELETE resolves guild 1111 from chan_guilds!
  CT_CHECK(ccord_transport_select_channel(t, CCORD_DM_GUILD_ID, 4444, &err) == CCORD_OK);

  ccord_admin_request ar;
  CT_CHECK(ccord_admin_stage_create(&ar, 1111, 2223, "Town Hall", 0, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 3000));
  CT_CHECK(!ev.stage_deleted && ev.guild_id == 1111 && ev.channel_id == 2223 && ev.stage_instance != nullptr &&
           strcmp(ev.stage_instance->topic, "Town Hall") == 0);

  CT_CHECK(ccord_admin_stage_delete(&ar, 2223, "wrap up", &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_STAGE_INSTANCE_UPDATE, &ev, 3000));
  CT_CHECK(ev.stage_deleted && ev.guild_id == 1111 && ev.channel_id == 2223 && ev.stage_instance != nullptr &&
           ev.stage_instance->channel_id == 2223 && ev.stage_instance->guild_id == 1111);

  // 8. Scheduled events: list, create, modify, RSVP add, RSVP remove, delete.
  CT_CHECK(ccord_admin_event_list(&ar, 1111, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_LIST, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.scheduled_event_count == 1 && ev.scheduled_events[0].id == 7701 &&
           strcmp(ev.scheduled_events[0].name, "Town Hall Event") == 0);

  ccord_scheduled_event new_se = {0};
  new_se.guild_id = 1111;
  new_se.channel_id = 2223;
  snprintf(new_se.name, sizeof new_se.name, "Launch Party");
  snprintf(new_se.description, sizeof new_se.description, "Bring snacks");
  new_se.scheduled_start_ms = 1794852000000LL;
  new_se.entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE;
  CT_CHECK(ccord_admin_event_create(&ar, 1111, &new_se, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev, 3000));
  CT_CHECK(!ev.scheduled_event_deleted && ev.guild_id == 1111 && ev.scheduled_event_id == 7702 &&
           ev.scheduled_event_count == 1 && strcmp(ev.scheduled_events[0].name, "Launch Party") == 0);

  ccord_admin_event_edit edit = CCORD_ADMIN_EVENT_EDIT_INIT;
  edit.name = "Launch Party Live";
  edit.status = CCORD_SCHEDULED_EVENT_ACTIVE;
  CT_CHECK(ccord_admin_event_modify(&ar, 1111, 7702, &edit, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev, 3000));
  CT_CHECK(!ev.scheduled_event_deleted && ev.scheduled_event_id == 7702 &&
           ev.scheduled_events[0].status == CCORD_SCHEDULED_EVENT_ACTIVE &&
           strcmp(ev.scheduled_events[0].name, "Launch Party Live") == 0);

  CT_CHECK(ccord_admin_event_rsvp_add(&ar, 1111, 7702, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.scheduled_event_id == 7702 && ev.scheduled_event_user_added);

  CT_CHECK(ccord_admin_event_rsvp_remove(&ar, 1111, 7702, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.scheduled_event_id == 7702 && !ev.scheduled_event_user_added);

  CT_CHECK(ccord_admin_event_delete(&ar, 1111, 7702, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_SCHEDULED_EVENT_UPDATE, &ev, 3000));
  CT_CHECK(ev.scheduled_event_deleted && ev.guild_id == 1111 && ev.scheduled_event_id == 7702 &&
           ev.scheduled_event_count == 1 && ev.scheduled_events[0].id == 7702);

  // 9. Membership screening: the form answer, then the PUT answered with a member object and with a bare 204.
  CT_CHECK(ccord_admin_get_member_verification(&ar, 1111, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MEMBER_VERIFICATION, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.member_verification != nullptr && ev.member_verification->field_count == 1 &&
           strcmp(ev.member_verification->version, "2026-11-01T00:00:00.000000+00:00") == 0 &&
           strcmp(ev.member_verification->description, "Town rules") == 0 &&
           ev.member_verification->fields[0].value_count == 2 &&
           strcmp(ev.member_verification->fields[0].values[1], "No spam") == 0);
  ccord_member_verification ms_form = {
    .version = "2026-11-01T00:00:00.000000+00:00",
    .fields = {{.field_type = CCORD_MEMBER_VERIFICATION_TERMS, .label = "Rules", .required = true,
                .values = {"Be kind", "No spam"}, .value_count = 2}},
    .field_count = 1,
  };
  CT_CHECK(ccord_admin_submit_member_verification(&ar, 1111, &ms_form, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_GUILD_MEMBERS, &ev, 3000));
  CT_CHECK(ev.guild_id == 1111 && ev.guild_member_count == 1 && ev.guild_members[0].user_id == 42 &&
           !ev.guild_members[0].pending && !ev.guild_members[0].omit_pending);
  ms_form.fields[0].label = "silent";
  CT_CHECK(ccord_admin_submit_member_verification(&ar, 1111, &ms_form, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_admin_action(t, &ar, &err) == CCORD_OK);
  CT_CHECK(!poll_live_for(t, CCORD_NET_GUILD_MEMBERS, &ev, 700));   // a 204 changes nothing locally

  // 10. Voice message: POST /attachments -> PUT slot -> POST /messages, with the metadata riding the final POST.
  static const uint8_t k_ogg[4] = {'O', 'g', 'g', 'S'};
  ccord_upload_file vfile = {.filename = "voice-message.ogg", .data = k_ogg, .size = sizeof k_ogg,
                             .duration_ms = 2500, .waveform_b64 = "AAECAwQFBgcICQ=="};
  CT_CHECK(ccord_transport_send_message_with_files(t, 2222, CCORD_ID_NONE, true, "", 0, &vfile, 1, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t, CCORD_NET_MESSAGE_CREATE, &ev, 3000));
  CT_CHECK(ev.message_count == 1 && ev.messages[0].id == 9110);
  if (ev.message_count == 1 && ev.messages[0].rich && ev.messages[0].rich->attachment_count == 1) {
    const ccord_message_rich *vr = ev.messages[0].rich;
    CT_CHECK((vr->flags & CCORD_MSGFLAG_VOICE_MESSAGE) != 0 && vr->attachments[0].duration_ms == 2500 &&
             strcmp(vr->attachments[0].waveform_b64, "AAECAwQFBgcICQ==") == 0);
  } else {
    CT_CHECK(false);
  }
  ccord_upload_file badv = vfile;
  badv.waveform_b64 = "not base64!";
  CT_CHECK(ccord_transport_send_message_with_files(t, 2222, CCORD_ID_NONE, true, "", 0, &badv, 1, &err) == CCORD_ERR_INVALID);

  ccord_transport_destroy(t);
  fd_hit hits[FD_MAX_HITS];
  size_t nhits = fd_stop(&srv, hits, FD_MAX_HITS);

  // Verify wire payloads received by fake_discord.
  size_t cur = 0;
  const fd_hit *h_tts = find_hit(hits, nhits, "POST", "/channels/2222/messages", &cur);
  CT_CHECK(h_tts != nullptr && strstr(h_tts->body, "\"content\":\"heads up\"") != nullptr &&
           strstr(h_tts->body, "\"tts\":true") != nullptr);

  size_t fwd_cur = 0;
  const fd_hit *h_fwd_guild = find_hit(hits, nhits, "POST", "/channels/4444/messages", &fwd_cur);
  CT_CHECK(h_fwd_guild != nullptr &&
           strstr(h_fwd_guild->body,
                  "\"message_reference\":{\"type\":1,\"message_id\":\"9001\",\"channel_id\":\"2222\",\"guild_id\":\"1111\"}") !=
             nullptr);

  const fd_hit *h_fwd_dm = find_hit(hits, nhits, "POST", "/channels/2222/messages", &cur);
  CT_CHECK(h_fwd_dm != nullptr &&
           strstr(h_fwd_dm->body,
                  "\"message_reference\":{\"type\":1,\"message_id\":\"9002\",\"channel_id\":\"4444\"}") != nullptr &&
           strstr(h_fwd_dm->body, "\"guild_id\"") == nullptr);

  CT_CHECK(find_hit(hits, nhits, "POST", "/channels/2222/messages/9003/crosspost", nullptr) != nullptr);

  // The voice upload: one slot request, one PUT of the bytes, and a final POST with the voice metadata.
  const fd_hit *h_slot = find_hit(hits, nhits, "POST", "/channels/2222/attachments", nullptr);
  CT_CHECK(h_slot != nullptr && strstr(h_slot->body, "\"filename\":\"voice-message.ogg\",\"file_size\":4") != nullptr);
  CT_CHECK(find_hit(hits, nhits, "PUT", "/upload/0", nullptr) != nullptr);
  const fd_hit *h_voice = nullptr;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].method, "POST") == 0 && strcmp(hits[i].path, "/channels/2222/messages") == 0 &&
        strstr(hits[i].body, "\"duration_secs\"") != nullptr) {
      h_voice = &hits[i];
    }
  }
  CT_CHECK(h_voice != nullptr && strstr(h_voice->body, "\"uploaded_filename\":\"u0/voice-message.ogg\"") != nullptr &&
           strstr(h_voice->body, "\"duration_secs\":2.5") != nullptr &&
           strstr(h_voice->body, "\"waveform\":\"AAECAwQFBgcICQ==\"") != nullptr &&
           strstr(h_voice->body, "\"flags\":8192") != nullptr);

  // Target 9100 itself must never be acked; instead its predecessor 9004 is acked, and earliest 8990 acks 8989.
  CT_CHECK(find_hit(hits, nhits, "POST", "/channels/2222/messages/9100/ack", nullptr) == nullptr);
  const fd_hit *h_unread_prev = find_hit(hits, nhits, "POST", "/channels/2222/messages/9004/ack", nullptr);
  CT_CHECK(h_unread_prev != nullptr && strcmp(h_unread_prev->body, "{\"manual\":true,\"mention_count\":0}") == 0);
  const fd_hit *h_unread_first = find_hit(hits, nhits, "POST", "/channels/2222/messages/8989/ack", nullptr);
  CT_CHECK(h_unread_first != nullptr && strcmp(h_unread_first->body, "{\"manual\":true,\"mention_count\":0}") == 0);
  const fd_hit *h_unread_fail = find_hit(hits, nhits, "POST", "/channels/4444/messages/9100/ack", nullptr);
  CT_CHECK(h_unread_fail != nullptr && strcmp(h_unread_fail->body, "{\"manual\":true,\"mention_count\":0}") == 0);

  size_t get_proto_count = 0;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].method, "GET") == 0 && strcmp(hits[i].path, "/users/@me/settings-proto/2") == 0) {
      get_proto_count++;
    }
  }
  CT_CHECK(get_proto_count == 1);  // second request was served from cache after PATCH updated disk cache
  const fd_hit *h_patch_proto = find_hit(hits, nhits, "PATCH", "/users/@me/settings-proto/2", nullptr);
  CT_CHECK(h_patch_proto != nullptr && strncmp(h_patch_proto->body, "{\"settings\":\"", 13) == 0);

  // Verify settings-proto/2 disk cache hit also populates raw_proto / raw_proto_len when api_base == CCORD_LIVE_API_BASE.
  if (h_patch_proto) {
    const char * route = "/users/@me/settings-proto/2";
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char *rp = (const unsigned char *)route; *rp; rp++) {
      h ^= (uint64_t)*rp;
      h *= 1099511628211ull;
    }
    char sub1[576], sub2[600], cfile[640];
    snprintf(sub1, sizeof sub1, "%s/ccord", cache_dir);
    snprintf(sub2, sizeof sub2, "%s/ccord/api", cache_dir);
    snprintf(cfile, sizeof cfile, "%s/ccord/api/%016llx.json", cache_dir, (unsigned long long)h);
    (void)mkdir(sub1, 0700);
    (void)mkdir(sub2, 0700);
    FILE *cf = fopen(cfile, "wb");
    CT_CHECK(cf != nullptr);
    if (cf) {
      CT_CHECK(fwrite(h_patch_proto->body, 1, strlen(h_patch_proto->body), cf) == strlen(h_patch_proto->body));
      fclose(cf);
    }
    const char *prev_xdg = getenv("XDG_CACHE_HOME");
    char saved_xdg[512] = "";
    bool had_xdg = prev_xdg != nullptr;
    if (had_xdg) snprintf(saved_xdg, sizeof saved_xdg, "%s", prev_xdg);
    (void)setenv("XDG_CACHE_HOME", cache_dir, 1);

    ccord_live_config ccfg = {
      .token = "REDACTED_TEST_TOKEN",
      .disable_gateway = true,
    };
    ccord_transport *tc = ccord_live_transport_create(&ccfg, &err);
    CT_CHECK(tc != nullptr);
    if (tc) {
      CT_CHECK(ccord_transport_connect(tc, &err) == CCORD_OK);
      CT_CHECK(poll_live_for(tc, CCORD_NET_STATE, &ev, 1000));
      CT_CHECK(ccord_transport_request_expression_data(tc, CCORD_EXPR_SETTINGS_PROTO, nullptr, 0, &err) == CCORD_OK);
      CT_CHECK(poll_live_for(tc, CCORD_NET_EXPRESSION_DATA, &ev, 1000));
      CT_CHECK(ev.expr_kind == CCORD_EXPR_SETTINGS_PROTO && ev.raw_proto != nullptr &&
               ev.raw_proto_len == patched_raw_len && memcmp(ev.raw_proto, patched_raw, patched_raw_len) == 0);
      CT_CHECK(ccord_user_expressions_decode(ev.raw_proto, ev.raw_proto_len, &ux, &err) == CCORD_OK &&
               ux.gif_count == 2);
      ccord_user_expressions_free(&ux);
      ccord_transport_destroy(tc);
    }

    if (had_xdg) {
      (void)setenv("XDG_CACHE_HOME", saved_xdg, 1);
    } else {
      (void)unsetenv("XDG_CACHE_HOME");
    }
    (void)unlink(cfile);
    (void)rmdir(sub2);
    (void)rmdir(sub1);
  }

  const fd_hit *h_inter = find_hit(hits, nhits, "POST", "/interactions", nullptr);
  CT_CHECK(h_inter != nullptr && strstr(h_inter->body, "\"name\":\"Pin Fast\",\"type\":3") != nullptr &&
           strstr(h_inter->body, "\"target_id\":\"9003\"") != nullptr);

  CT_CHECK(find_hit(hits, nhits, "POST", "/stage-instances", nullptr) != nullptr);
  const fd_hit *h_stage_del = find_hit(hits, nhits, "DELETE", "/stage-instances/2223", nullptr);
  CT_CHECK(h_stage_del != nullptr && strcmp(h_stage_del->audit, "wrap%20up") == 0);

  CT_CHECK(find_hit(hits, nhits, "GET", "/guilds/1111/scheduled-events?with_user_count=true", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "POST", "/guilds/1111/scheduled-events", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "PATCH", "/guilds/1111/scheduled-events/7702", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "PUT", "/guilds/1111/scheduled-events/7702/users/@me", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "DELETE", "/guilds/1111/scheduled-events/7702/users/@me", nullptr) != nullptr);
  CT_CHECK(find_hit(hits, nhits, "GET", "/guilds/1111/member-verification?with_guild=true", nullptr) != nullptr);
  size_t ms_cur = 0;
  const fd_hit *h_ms = find_hit(hits, nhits, "PUT", "/guilds/1111/requests/@me", &ms_cur);
  CT_CHECK(h_ms != nullptr && strstr(h_ms->body, "\"version\":\"2026-11-01T00:00:00.000000+00:00\"") != nullptr &&
           strstr(h_ms->body, "\"field_type\":\"TERMS\"") != nullptr && strstr(h_ms->body, "\"response\":true") != nullptr);
  CT_CHECK(find_hit(hits, nhits, "DELETE", "/guilds/1111/scheduled-events/7702", nullptr) != nullptr);

  ccord_free(init_raw);
  ccord_free(patched_raw);
  (void)rmdir(cache_dir);
}

static void ms_cat(ccord_buf *b, const char *s) { (void)ccord_buf_append_str(b, s); }

// Membership screening: the member-verification form (GET answer), the PUT answer path and the `pending` flag on every
// member source (gateway member add/update, GUILD_CREATE, READY merged_members, REST member, message author stub).
static void test_member_screening_json_and_pending(void)
{
  ccord_live_event ev;
  ccord_error err;

  // ---- GET /guilds/{g}/member-verification?with_guild=true ----
  const char *form =
    "{\"version\":\"2022-02-09T21:15:23.570000+00:00\",\"description\":\"Welcome to HQ\","
    "\"form_fields\":[{\"field_type\":\"TERMS\",\"label\":\"Server Rules\",\"required\":true,"
    "\"values\":[\"Be kind\",\"No spam\",7,\"Stay on topic\"],\"automations\":null},"
    "{\"field_type\":\"MULTIPLE_CHOICE\",\"label\":\"skipped\",\"values\":[\"a\"],\"required\":true},"
    "{\"field_type\":\"PARAGRAPH\",\"label\":\"Why join?\",\"required\":false,\"values\":null},"
    "\"junk\",{\"label\":\"No type means terms\"}],"
    "\"guild\":{\"id\":\"77\",\"name\":\"HQ\"}}";
  CT_CHECK(ccord_live_json_member_verification(form, strlen(form), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MEMBER_VERIFICATION && ev.ev.guild_id == 77 && ev.ev.member_verification);
  const ccord_member_verification *mv = ev.ev.member_verification;
  CT_CHECK(strcmp(mv->version, "2022-02-09T21:15:23.570000+00:00") == 0 && strcmp(mv->description, "Welcome to HQ") == 0);
  CT_CHECK(mv->field_count == 3);                                   // multiple choice and the non-object entry are dropped
  CT_CHECK(mv->fields[0].field_type == CCORD_MEMBER_VERIFICATION_TERMS && strcmp(mv->fields[0].label, "Server Rules") == 0 &&
           mv->fields[0].required && mv->fields[0].value_count == 3 && strcmp(mv->fields[0].values[0], "Be kind") == 0 &&
           strcmp(mv->fields[0].values[2], "Stay on topic") == 0);      // the number among the rules is skipped
  CT_CHECK(mv->fields[1].field_type == CCORD_MEMBER_VERIFICATION_TEXT_INPUT && !mv->fields[1].required &&
           mv->fields[1].value_count == 0 && strcmp(mv->fields[1].label, "Why join?") == 0);
  CT_CHECK(mv->fields[2].field_type == CCORD_MEMBER_VERIFICATION_TERMS && mv->fields[2].required &&
           strcmp(mv->fields[2].label, "No type means terms") == 0);
  ccord_live_event_free(&ev);

  // A bare object is a form with nothing in it, never a crash; non-objects and bad args are refused.
  CT_CHECK(ccord_live_json_member_verification("{}", 2, 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.member_verification->field_count == 0 && strcmp(ev.ev.member_verification->version, "") == 0 &&
           strcmp(ev.ev.member_verification->description, "") == 0);
  ccord_live_event_free(&ev);
  CT_CHECK(ccord_live_json_member_verification("[]", 2, 77, &ev, &err) == CCORD_ERR_PARSE);
  CT_CHECK(ccord_live_json_member_verification("{}", 2, 0, &ev, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_json_member_verification("{}", 2, CCORD_DM_GUILD_ID, &ev, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_live_json_member_verification(nullptr, 0, 77, &ev, &err) == CCORD_ERR_INVALID);

  // Everything past the model.h caps is cut: 10 fields -> 8, 20 values -> 16, 130-byte text -> 100 bytes (UTF-8 safe).
  ccord_buf big;
  ccord_buf_init(&big, 65536);
  ms_cat(&big, "{\"version\":\"");
  for (int i = 0; i < 40; i++) ms_cat(&big, "v");
  ms_cat(&big, "\",\"description\":\"");
  for (int i = 0; i < 700; i++) ms_cat(&big, "d");
  ms_cat(&big, "\",\"form_fields\":[");
  for (int f = 0; f < 10; f++) {
    ms_cat(&big, f ? ",{\"field_type\":\"TERMS\",\"label\":\"" : "{\"field_type\":\"TERMS\",\"label\":\"");
    for (int i = 0; i < 130; i++) ms_cat(&big, "\xc3\xa9");   // 260 bytes of é
    ms_cat(&big, "\",\"values\":[");
    for (int v = 0; v < 20; v++) {
      ms_cat(&big, v ? ",\"" : "\"");
      for (int i = 0; i < 130; i++) ms_cat(&big, "r");
      ms_cat(&big, "\"");
    }
    ms_cat(&big, "]}");
  }
  ms_cat(&big, "]}");
  CT_CHECK(ccord_live_json_member_verification(ccord_buf_cstr(&big), big.len, 77, &ev, &err) == CCORD_OK);
  mv = ev.ev.member_verification;
  CT_CHECK(strlen(mv->version) == CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES);
  CT_CHECK(strlen(mv->description) == CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES);
  CT_CHECK(mv->field_count == CCORD_MEMBER_VERIFICATION_MAX_FIELDS);
  CT_CHECK(strlen(mv->fields[0].label) == CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES && ccord_utf8_valid(mv->fields[0].label, strlen(mv->fields[0].label)));
  CT_CHECK(mv->fields[0].value_count == CCORD_MEMBER_VERIFICATION_MAX_VALUES &&
           strlen(mv->fields[0].values[15]) == CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES);
  ccord_live_event_free(&ev);
  ccord_buf_free(&big);

  // ---- pending on every member source ----
  const char *add =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_ADD\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"pending\":true}}";
  CT_CHECK(ccord_live_gateway_decode(add, strlen(add), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].pending && !ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  const char *accepted =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"pending\":false}}";
  CT_CHECK(ccord_live_gateway_decode(accepted, strlen(accepted), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && !ev.members[0].pending && !ev.members[0].omit_pending);   // an explicit false clears it
  ccord_live_event_free(&ev);
  const char *silent =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"nick\":\"B\"}}";
  CT_CHECK(ccord_live_gateway_decode(silent, strlen(silent), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && !ev.members[0].pending && ev.members[0].omit_pending);   // absent: nothing said
  ccord_live_event_free(&ev);
  const char *junk =
    "{\"op\":0,\"t\":\"GUILD_MEMBER_UPDATE\",\"d\":{\"guild_id\":\"77\",\"user\":{\"id\":\"44\",\"username\":\"bob\"},"
    "\"roles\":[],\"pending\":\"yes\"}}";
  CT_CHECK(ccord_live_gateway_decode(junk, strlen(junk), &ev, &err) == CCORD_OK);
  CT_CHECK(!ev.members[0].pending && ev.members[0].omit_pending);                            // a non-bool is not a statement
  ccord_live_event_free(&ev);

  const char *create =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"d\":{\"id\":\"77\",\"name\":\"HQ\",\"members\":["
    "{\"user\":{\"id\":\"43\",\"username\":\"Ada\"},\"roles\":[],\"pending\":true},"
    "{\"user\":{\"id\":\"45\",\"username\":\"Cy\"},\"roles\":[]}],\"channels\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(create, strlen(create), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && ev.member_count == 2);
  if (ev.member_count == 2) {
    CT_CHECK(ev.members[0].user_id == 43 && ev.members[0].pending && !ev.members[0].omit_pending);
    CT_CHECK(ev.members[1].user_id == 45 && !ev.members[1].pending && ev.members[1].omit_pending);
  }
  ccord_live_event_free(&ev);

  const char *ready =
    "{\"op\":0,\"t\":\"READY\",\"d\":{\"user\":{\"id\":\"42\",\"username\":\"selfuser\"},"
    "\"guilds\":[{\"id\":\"100\",\"name\":\"Screened\",\"roles\":[{\"id\":\"100\",\"name\":\"@everyone\"}],"
    "\"channels\":[]}],\"merged_members\":[[{\"user_id\":\"42\",\"roles\":[],\"pending\":true}]]}}";
  CT_CHECK(ccord_live_gateway_decode(ready, strlen(ready), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_SNAPSHOT && ev.snapshot.member_count == 1);
  if (ev.snapshot.member_count == 1) {
    CT_CHECK(ev.snapshot.members[0].user_id == 42 && ev.snapshot.members[0].pending && !ev.snapshot.members[0].omit_pending);
  }
  ccord_live_event_free(&ev);

  // REST member object (and the PUT /requests/@me answer, which takes the same path).
  const char *rest = "{\"user\":{\"id\":\"43\",\"username\":\"ada\"},\"roles\":[],\"pending\":false}";
  CT_CHECK(ccord_live_json_member(rest, strlen(rest), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && !ev.members[0].pending && !ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  const char *rest_pending = "{\"user\":{\"id\":\"43\",\"username\":\"ada\"},\"roles\":[],\"pending\":true}";
  CT_CHECK(ccord_live_json_member(rest_pending, strlen(rest_pending), 77, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.members[0].pending && !ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  // A 204 / join-request answer is not a member object: refused so the caller applies nothing.
  const char *join_req = "{\"id\":\"9\",\"application_status\":\"SUBMITTED\",\"guild_id\":\"77\"}";
  CT_CHECK(ccord_live_json_member(join_req, strlen(join_req), 77, &ev, &err) != CCORD_OK);
  ccord_live_event_free(&ev);

  // MESSAGE_CREATE's partial author member never says anything about screening, even when the member object does.
  const char *msg =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"100\",\"channel_id\":\"200\",\"guild_id\":\"77\","
    "\"author\":{\"id\":\"43\",\"username\":\"ada\"},\"content\":\"hi\",\"member\":{\"nick\":\"Ace\",\"roles\":[],"
    "\"pending\":true}}}";
  CT_CHECK(ccord_live_gateway_decode(msg, strlen(msg), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_MESSAGE_CREATE && ev.member_count == 1);
  CT_CHECK(ev.members[0].user_id == 43 && !ev.members[0].pending && ev.members[0].omit_pending);
  ccord_live_event_free(&ev);
  const char *msg2 =
    "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"101\",\"channel_id\":\"200\",\"guild_id\":\"77\","
    "\"author\":{\"id\":\"43\",\"username\":\"ada\"},\"content\":\"hi\"}}";
  CT_CHECK(ccord_live_gateway_decode(msg2, strlen(msg2), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.member_count == 1 && ev.members[0].omit_pending && !ev.members[0].pending);
  ccord_live_event_free(&ev);
}

// ---- raw media download (the live video path) against a loopback HTTP server ---------------------------------------
//
// live_fetch_media queues a raw GET with bulk=true and max_response=CCORD_LIVE_MAX_MEDIA (live_queue_add_raw); the host
// allow-list only gates live_fetch_media itself, so this drives the same queue request at 127.0.0.1. The server forks a
// handler per connection and answers by path: a fast 20 MiB body, a ~1 MiB/s body, 403, 302, an over-cap body, and a
// header that is held back (to keep raw slots busy).

#define MD_MIB (1024u * 1024u)

static void md_send_all(int fd, const char *p, size_t n)
{
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void md_handle(int cfd)
{
  alarm(60);
  char req[2048];
  size_t len = 0;
  while (len + 1 < sizeof req) {
    ssize_t n = read(cfd, req + len, sizeof req - 1 - len);
    if (n <= 0) return;
    len += (size_t)n;
    req[len] = '\0';
    if (strstr(req, "\r\n\r\n")) break;
  }
  char path[64] = "";
  (void)sscanf(req, "GET %63s", path);
  static char chunk[65536];
  memset(chunk, 0x5a, sizeof chunk);
  char head[256];
  size_t total = 0, step = sizeof chunk, pause_us = 0;
  if (strcmp(path, "/ok20.mp4") == 0) {
    total = 20u * MD_MIB;
  } else if (strcmp(path, "/slow.mp4") == 0) {
    total = 3u * MD_MIB;            // 64 KiB every 62 ms is about 1 MiB/s
    pause_us = 62000;
  } else if (strcmp(path, "/over.mp4") == 0) {
    total = 33u * MD_MIB;           // above CCORD_LIVE_MAX_MEDIA (32 MiB)
  } else if (strcmp(path, "/hold.mp4") == 0) {
    total = 1024;
    sleep(3);
  } else if (strcmp(path, "/small.mp4") == 0) {
    total = 1024;
  } else if (strcmp(path, "/403.mp4") == 0) {
    int n = snprintf(head, sizeof head, "HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno");
    md_send_all(cfd, head, (size_t)n);
    return;
  } else if (strcmp(path, "/302.mp4") == 0) {
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 302 Found\r\nLocation: https://cdn.discordapp.com/elsewhere.mp4\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n");
    md_send_all(cfd, head, (size_t)n);
    return;
  } else {
    int n = snprintf(head, sizeof head, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    md_send_all(cfd, head, (size_t)n);
    return;
  }
  int n = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: %zu\r\n"
                                      "Connection: close\r\n\r\n", total);
  md_send_all(cfd, head, (size_t)n);
  for (size_t sent = 0; sent < total;) {
    size_t c = total - sent < step ? total - sent : step;
    md_send_all(cfd, chunk, c);
    sent += c;
    if (pause_us) usleep((useconds_t)pause_us);
  }
}

static pid_t md_start(int *port)
{
  (void)signal(SIGPIPE, SIG_IGN);
  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) return -1;
  int opt = 1;
  (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t sl = sizeof sin;
  if (bind(lfd, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(lfd, 32) != 0 ||
      getsockname(lfd, (struct sockaddr *)&sin, &sl) != 0) {
    close(lfd);
    return -1;
  }
  *port = ntohs(sin.sin_port);
  pid_t pid = fork();
  if (pid == 0) {
    (void)signal(SIGCHLD, SIG_IGN);
    alarm(120);
    for (;;) {
      int cfd = accept(lfd, nullptr, nullptr);
      if (cfd < 0) _exit(0);
      if (fork() == 0) {
        close(lfd);
        md_handle(cfd);
        close(cfd);
        _exit(0);
      }
      close(cfd);
    }
  }
  close(lfd);
  return pid;
}

typedef struct md_result {
  int calls;
  ccord_status st;
  long http;
  size_t len;
  uint8_t first, last;
} md_result;

static void md_done(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  md_result *r = ud;
  r->calls++;
  r->st = st;
  r->http = http;
  r->len = len;
  r->first = len ? (uint8_t)body[0] : 0;
  r->last = len ? (uint8_t)body[len - 1] : 0;
}

static void md_get(ccord_rest_queue *q, int port, const char *path, md_result *r)
{
  char url[96];
  (void)snprintf(url, sizeof url, "http://127.0.0.1:%d%s", port, path);
  ccord_rest_request rq = ccord_rest_request_make(CCORD_REST_GET, nullptr, CCORD_GOV_VISIBLE);
  rq.full_url = url;
  rq.cb = md_done;
  rq.ud = r;
  rq.bulk = true;                              // as live_queue_add_raw sets it for LIVE_REST_MEDIA
  rq.max_response = CCORD_LIVE_MAX_MEDIA;
  ccord_error err;
  CT_CHECK(ccord_rest_queue_submit(q, &rq, &err) == CCORD_OK);
}

// Pumps until every result has its callback, or `limit_s` real seconds pass. `skew_ms` is added to the clock the
// queue sees (curl keeps real time), so the 45 s pending rule can be crossed without waiting 45 s.
static double md_pump(ccord_rest_queue *q, md_result *const *rs, size_t n, double limit_s, uint64_t skew_ms)
{
  uint64_t t0 = fd_now_ms();
  for (;;) {
    ccord_rest_queue_pump(q, fd_now_ms() + skew_ms);
    bool all = true;
    for (size_t i = 0; i < n; i++) all = all && rs[i]->calls > 0;
    double el = (double)(fd_now_ms() - t0) / 1000.0;
    if (all || el > limit_s) return el;
    usleep(2000);
  }
}

static void test_raw_media_download(void)
{
  int port = 0;
  pid_t srv = md_start(&port);
  CT_CHECK(srv > 0);
  if (srv <= 0) return;
  ccord_rest_config cfg = {.timeout_ms = 15000};
  ccord_error err;
  ccord_rest_queue *q = ccord_rest_queue_create(&cfg, &err);
  CT_CHECK(q != nullptr);
  if (!q) return;

  // A 20 MiB video (under the 32 MiB cap) arrives whole.
  md_result big = {0};
  md_get(q, port, "/ok20.mp4", &big);
  md_result *one[1] = {&big};
  double el = md_pump(q, one, 1, 30.0, 0);
  CT_CHECK(big.calls == 1 && big.st == CCORD_OK && big.http == 200 && big.len == 20u * MD_MIB);
  CT_CHECK(big.first == 0x5a && big.last == 0x5a);
  printf("raw media: 20 MiB fast body took %.1f s\n", el);

  // A body at about 1 MiB/s (3 MiB, 3 s here; 20 MiB would take 20 s, far under the 300 s ceiling and the
  // 2 KiB/s-for-20 s stall rule) is not cut by the stall rule.
  md_result slow = {0};
  md_get(q, port, "/slow.mp4", &slow);
  one[0] = &slow;
  el = md_pump(q, one, 1, 30.0, 0);
  CT_CHECK(slow.calls == 1 && slow.st == CCORD_OK && slow.http == 200 && slow.len == 3u * MD_MIB);
  CT_CHECK(el > 2.0);   // it really was throttled
  printf("raw media: 3 MiB at ~1 MiB/s took %.1f s\n", el);

  // Over the cap: refused as too large, nothing partial delivered.
  md_result over = {0};
  md_get(q, port, "/over.mp4", &over);
  one[0] = &over;
  (void)md_pump(q, one, 1, 30.0, 0);
  // The callback also gets the partial buffer; live_rest_done ignores the body when status != CCORD_OK.
  CT_CHECK(over.calls == 1 && over.st == CCORD_ERR_RANGE);
  CT_CHECK(over.len <= CCORD_LIVE_MAX_MEDIA);

  // 403 and 302 reach the callback as the HTTP status; a redirect is not followed (the Location is never fetched).
  md_result r403 = {0}, r302 = {0};
  md_get(q, port, "/403.mp4", &r403);
  md_get(q, port, "/302.mp4", &r302);
  md_result *two[2] = {&r403, &r302};
  (void)md_pump(q, two, 2, 10.0, 0);
  CT_CHECK(r403.calls == 1 && r403.st == CCORD_OK && r403.http == 403);
  CT_CHECK(r302.calls == 1 && r302.st == CCORD_OK && r302.http == 302 && r302.len == 0);

  // Four held downloads fill the raw slots; a fifth video waits behind them for longer than 45 s of queue time
  // (the queue's clock is skewed by 60 s). It must be started when a slot frees, not dropped as stale.
  md_result hold[4] = {{0}}, fifth = {0};
  for (int i = 0; i < 4; i++) md_get(q, port, "/hold.mp4", &hold[i]);
  md_get(q, port, "/small.mp4", &fifth);
  uint64_t t = fd_now_ms();
  ccord_rest_queue_pump(q, t);
  ccord_rest_queue_pump(q, t + 60000u);
  CT_CHECK(fifth.calls == 0);   // pre-fix: already delivered as CCORD_ERR_TIMEOUT here
  for (int i = 0; i < 8000 && (fifth.calls == 0 || hold[3].calls == 0); i++) {
    ccord_rest_queue_pump(q, fd_now_ms() + 60000u);
    usleep(2000);
  }
  CT_CHECK(fifth.calls == 1 && fifth.st == CCORD_OK && fifth.http == 200 && fifth.len == 1024);
  for (int i = 0; i < 4; i++) CT_CHECK(hold[i].calls == 1 && hold[i].st == CCORD_OK && hold[i].len == 1024);

  ccord_rest_queue_destroy(q);
  (void)kill(srv, SIGTERM);
  (void)waitpid(srv, nullptr, 0);
}

// ---- per-account API cache, warm history file, history stale-while-revalidate ---------------------------------------

// History GET n (0-based, counted inside the forked server) answers with n+1 messages, so a test can tell which
// response (cached file or which network answer) it received by message_count.
static size_t respond_hist_count(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)ud;
  (void)idx;
  static int gets = 0;
  if (strcmp(req->method, "GET") != 0 || strstr(req->path, "/messages?limit=") == nullptr) {
    return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"unhandled route\"}");
  }
  char body[1024] = "[";
  size_t n = (size_t)gets++ + 1u;
  for (size_t i = 0; i < n; i++) {
    char one[200];
    snprintf(one, sizeof one,
             "%s{\"id\":\"%zu\",\"channel_id\":\"2222\",\"author\":{\"id\":\"42\",\"username\":\"me\"},"
             "\"content\":\"m%zu\",\"timestamp\":\"2026-11-10T11:5%zu:00Z\"}",
             i ? "," : "", 9000 - i, i, i);
    strncat(body, one, sizeof body - strlen(body) - 1);
  }
  strncat(body, "]", sizeof body - strlen(body) - 1);
  return fd_reply(out, cap, 200, "OK", nullptr, body);
}

static ccord_transport *hist_transport(const char *api_base, const char *cache_dir)
{
  ccord_error err;
  ccord_live_config cfg = {
    .token = "REDACTED_TEST_TOKEN",
    .api_base = api_base,
    .cache_dir = cache_dir,
    .disable_gateway = true,
    .request_timeout_ms = 3000,
  };
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  ccord_net_event ev;
  if (t && (ccord_transport_connect(t, &err) != CCORD_OK || !poll_live_for(t, CCORD_NET_STATE, &ev, 3000))) {
    ccord_transport_destroy(t);
    return nullptr;
  }
  return t;
}

// Reads the first history event; returns its message count, or -1 when none arrives within the timeout.
static int next_history_count(ccord_transport *t, uint64_t timeout_ms)
{
  ccord_net_event ev;
  if (!poll_live_for(t, CCORD_NET_HISTORY, &ev, timeout_ms)) return -1;
  return (int)ev.message_count;
}

static unsigned mode_bits(const char *p)
{
  struct stat st;
  return stat(p, &st) == 0 ? (unsigned)(st.st_mode & 0777) : 0xFFFFu;
}

static size_t count_files(const char *dir)
{
  size_t n = 0;
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] != '.') n++;
  }
  closedir(d);
  return n;
}

static void test_account_scoped_cache_and_history_revalidate(void)
{
  char root[512];
  if (!make_temp(root, sizeof root)) {
    fprintf(stderr, "test_live_transport: no TMPDIR, skipping account cache test\n");
    return;
  }
  (void)umask(0);  // files must still come out 0600 / 0700
  char dir_a[600], dir_b[600], file[700], api_a[700], api_b[700];
  snprintf(dir_a, sizeof dir_a, "%s/accounts/aaaa", root);
  snprintf(dir_b, sizeof dir_b, "%s/accounts/bbbb", root);
  snprintf(api_a, sizeof api_a, "%s/api", dir_a);
  snprintf(api_b, sizeof api_b, "%s/api", dir_b);

  // ccord_live_history_cache_path
  CT_CHECK(ccord_live_history_cache_path("/d/acc", 2222, file, sizeof file) &&
           strcmp(file, "/d/acc/warm/history/2222.json") == 0);
  CT_CHECK(!ccord_live_history_cache_path(nullptr, 2222, file, sizeof file));
  CT_CHECK(!ccord_live_history_cache_path("", 2222, file, sizeof file));
  CT_CHECK(!ccord_live_history_cache_path("/d/acc", 2222, file, 8));

  fd_server srv;
  CT_CHECK(fd_start(&srv, respond_hist_count, nullptr));
  char hfile_a[700], hfile_b[700];
  CT_CHECK(ccord_live_history_cache_path(dir_a, 2222, hfile_a, sizeof hfile_a));
  CT_CHECK(ccord_live_history_cache_path(dir_b, 2222, hfile_b, sizeof hfile_b));

  // Account A, cold: no cache, so only the network answer (1 message) arrives; it is persisted 0600 in a 0700 dir.
  ccord_error err;
  ccord_transport *ta = hist_transport(srv.base, dir_a);
  CT_CHECK(ta != nullptr);
  if (ta) {
    CT_CHECK(ccord_transport_request_history(ta, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(ta, 3000) == 1);
    CT_CHECK(count_files(api_a) == 1);
    CT_CHECK(mode_bits(api_a) == 0700 && mode_bits(dir_a) == 0700);
    CT_CHECK(mode_bits(hfile_a) == 0600);
    char pdir[700];
    snprintf(pdir, sizeof pdir, "%s/warm/history", dir_a);
    CT_CHECK(mode_bits(pdir) == 0700);
    DIR *d = opendir(api_a);
    struct dirent *e;
    while (d && (e = readdir(d)) != nullptr) {
      if (e->d_name[0] == '.') continue;
      char f[1100];
      snprintf(f, sizeof f, "%s/%s", api_a, e->d_name);
      CT_CHECK(mode_bits(f) == 0600);
    }
    if (d) closedir(d);
    ccord_transport_destroy(ta);
  }

  // Account B asks for the same REST path: A's cached page must not be delivered. The first (and only) event is the
  // network answer (2 messages); B gets its own cache file and A's file is untouched.
  ccord_transport *tb = hist_transport(srv.base, dir_b);
  CT_CHECK(tb != nullptr);
  if (tb) {
    CT_CHECK(ccord_transport_request_history(tb, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(tb, 3000) == 2);
    CT_CHECK(count_files(api_b) == 1);
    ccord_net_event extra;
    CT_CHECK(!poll_live_for(tb, CCORD_NET_HISTORY, &extra, 150));  // nothing else was served from disk
    ccord_transport_destroy(tb);
  }
  char one_a[1024] = "", one_b[1024] = "";
  for (int k = 0; k < 2; k++) {
    const char *dir = k ? api_b : api_a;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != nullptr) {
      if (e->d_name[0] != '.') snprintf(k ? one_b : one_a, 1024, "%s/%s", dir, e->d_name);
    }
    if (d) closedir(d);
  }
  CT_CHECK(one_a[0] && one_b[0] && strcmp(one_a, one_b) != 0);
  CT_CHECK(strcmp(strrchr(one_a, '/'), strrchr(one_b, '/')) == 0);  // same key, different account directory
  ccord_error rerr;
  size_t la = 0, lb = 0;
  char *ca = ccord_file_read_max(one_a, 1 << 20, &la, &rerr);
  char *cb = ccord_file_read_max(one_b, 1 << 20, &lb, &rerr);
  CT_CHECK(ca && cb && la > 0 && lb > la);  // A holds the 1-message page, B the 2-message page
  ccord_free(ca);
  ccord_free(cb);

  // Account A again (a warm start): the fresh cached page (1 message) is delivered first, and the network is still
  // asked: the second event is the live answer (3 messages). No TTL read-through.
  ta = hist_transport(srv.base, dir_a);
  CT_CHECK(ta != nullptr);
  if (ta) {
    CT_CHECK(ccord_transport_request_history(ta, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(ta, 3000) == 1);
    CT_CHECK(next_history_count(ta, 3000) == 3);
    ccord_transport_destroy(ta);
  }
  fd_hit hits[FD_MAX_HITS];
  size_t nhits = fd_stop(&srv, hits, FD_MAX_HITS);
  size_t gets = 0;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].method, "GET") == 0 && strstr(hits[i].path, "/messages?limit=") != nullptr) gets++;
  }
  CT_CHECK(gets == 3);  // A cold, B cold, A warm: the cache hit still went to the network

  // Warm history files are pruned to the 8 newest.
  char hdir[700], extra_file[800];
  snprintf(hdir, sizeof hdir, "%s/warm/history", dir_a);
  for (int i = 0; i < 12; i++) {
    snprintf(extra_file, sizeof extra_file, "%s/%d.json", hdir, 100 + i);
    FILE *f = fopen(extra_file, "wb");
    if (f) {
      fputs("[]", f);
      fclose(f);
    }
  }
  fd_server srv2;
  CT_CHECK(fd_start(&srv2, respond_hist_count, nullptr));
  ta = hist_transport(srv2.base, dir_a);
  CT_CHECK(ta != nullptr);
  if (ta) {
    CT_CHECK(ccord_transport_request_history(ta, 2222, 50, &err) == CCORD_OK);
    CT_CHECK(next_history_count(ta, 3000) == 3);   // stale page saved by the earlier run
    CT_CHECK(next_history_count(ta, 3000) == 1);   // live answer from the fresh server
    ccord_transport_destroy(ta);
  }
  (void)fd_stop(&srv2, hits, FD_MAX_HITS);
  CT_CHECK(count_files(hdir) == 8);
  CT_CHECK(mode_bits(hfile_a) == 0600);  // the newest (just written) file survived the prune

  ccord_error rm;
  CT_CHECK(ccord_rmtree(root, &rm) == CCORD_OK);
}

static void test_network_event_hook(void)
{
  (void)unsetenv(CCORD_LIVE_TOKEN_ENV);
  ccord_error err;

  // A transport whose vtable has no hook: UNSUPPORTED, no crash; a null transport is the same.
  static const ccord_transport_vtable bare_vt = {.name = "bare"};
  ccord_transport bare = {.vt = &bare_vt};
  CT_CHECK(ccord_transport_network_event(&bare, CCORD_LINK_RESUMED, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_transport_network_event(nullptr, CCORD_LINK_CHANGED, &err) == CCORD_ERR_UNSUPPORTED);
  CT_CHECK(ccord_transport_network_event(&bare, CCORD_LINK_CHANGED, nullptr) == CCORD_ERR_UNSUPPORTED);

  // Live, no gateway (connected in the offline mode): the event reaches live_network_event and is a no-op.
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN", .disable_gateway = true};
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(t->vt->network_event != nullptr);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_connect(t, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_NONE, &err) == CCORD_OK);
  ccord_transport_destroy(t);

  // Live with a gateway that was never started (state DISCONNECTED: wake verdict IGNORED): OK, nothing happens.
  ccord_live_config gcfg = {.token = "REDACTED_TEST_TOKEN", .gateway_url = "ws://127.0.0.1:1/"};
  t = ccord_live_transport_create(&gcfg, &err);
  CT_CHECK(t != nullptr);
  if (!t) return;
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_CHANGED, &err) == CCORD_OK);
  // Started (CONNECTING toward a closed port): the event is still delivered and returns OK. The verdict itself is
  // pinned by the gateway task's tests once ccord_gateway_wake exists; after disconnect it is a no-op again.
  if (ccord_transport_connect(t, &err) == CCORD_OK) {
    CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
    ccord_transport_disconnect(t);
    CT_CHECK(ccord_transport_network_event(t, CCORD_LINK_RESUMED, &err) == CCORD_OK);
  }
  ccord_transport_destroy(t);
}

// ---- status through settings-proto/1 ----
// Base proto the fake server holds: field 3 "abc", status online + show_current_game, field 99 varint.
#define SP1_BASE_B64 "GgNhYmNaDgoICgZvbmxpbmUaAggBmAZC"

typedef struct sp1_cfg {
  bool fail_get;
} sp1_cfg;

static size_t respond_sp1(void *ud, int idx, const fd_hit *req, char *out, size_t cap)
{
  (void)idx;
  const sp1_cfg *cfg = ud;
  if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/users/@me/settings-proto/1") == 0) {
    if (cfg->fail_get) return fd_reply(out, cap, 500, "Internal Server Error", nullptr, "{\"message\":\"boom\"}");
    return fd_reply(out, cap, 200, "OK", nullptr, "{\"settings\":\"" SP1_BASE_B64 "\"}");
  }
  if (strcmp(req->method, "PATCH") == 0 && strcmp(req->path, "/users/@me/settings-proto/1") == 0) {
    return fd_reply(out, cap, 200, "OK", nullptr, req->body);
  }
  return fd_reply(out, cap, 404, "Not Found", nullptr, "{\"message\":\"unknown\"}");
}

static void sp1_pump(ccord_transport *t, int ms)
{
  ccord_net_event ev;
  for (int i = 0; i < ms / 3; i++) {
    (void)ccord_transport_poll(t, &ev);
    usleep(3000);
  }
}

static void test_status_proto_write_and_updates(void)
{
  ccord_error err;
  // Gateway side: a type-1 update carries its bytes (and "partial"); one with folders keeps both.
  ccord_live_event gev;
  const char *upd = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"partial\":true,\"settings\":{\"type\":1,"
                    "\"proto\":\"WggKBgoEaWRsZQ==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(upd, strlen(upd), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_USER_SETTINGS_PROTO && gev.ev.settings_proto1_len == 10 &&
           gev.ev.settings_proto1_partial && gev.ev.settings_proto1 && gev.ev.settings_proto1[0] == 0x5a);
  ccord_proto_status dec;
  CT_CHECK(ccord_user_status_decode(gev.ev.settings_proto1, gev.ev.settings_proto1_len, &dec, &err) == CCORD_OK &&
           dec.present && strcmp(dec.status, "idle") == 0);
  ccord_live_event_free(&gev);
  const char *full = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"settings\":{\"type\":1,"
                     "\"proto\":\"choKFAgeCAoSAgg3GgUKA09wcyIDCP8BCgIIFA==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(full, strlen(full), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_GUILD_LAYOUT && gev.ev.settings_proto1_len > 0 && !gev.ev.settings_proto1_partial);
  ccord_live_event_free(&gev);
  const char *t2 = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"settings\":{\"type\":2,\"proto\":\"WggKBgoEaWRsZQ==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(t2, strlen(t2), &gev, &err) == CCORD_OK && gev.ev.settings_proto1_len == 0);
  // ...and rides as raw_proto for the favourites path (type 2 is FrecencyUserSettings), with its partial flag.
  CT_CHECK(gev.ev.type == CCORD_NET_USER_SETTINGS_PROTO && gev.ev.raw_proto_len == 10 && !gev.ev.settings_proto1_partial);
  ccord_live_event_free(&gev);
  const char *t2p = "{\"op\":0,\"t\":\"USER_SETTINGS_PROTO_UPDATE\",\"d\":{\"partial\":true,\"settings\":{\"type\":2,"
                    "\"proto\":\"WggKBgoEaWRsZQ==\"}}}";
  CT_CHECK(ccord_live_gateway_decode(t2p, strlen(t2p), &gev, &err) == CCORD_OK && gev.ev.raw_proto &&
           gev.ev.settings_proto1_partial);
  ccord_live_event_free(&gev);

  // SESSIONS_REPLACE: the "all" session is the overall status; a lone session stands for it; "unknown" says nothing.
  const char *sr1 = "{\"op\":0,\"t\":\"SESSIONS_REPLACE\",\"d\":[{\"session_id\":\"abc\",\"status\":\"online\"},"
                    "{\"session_id\":\"all\",\"status\":\"dnd\",\"activities\":[]}]}";
  CT_CHECK(ccord_live_gateway_decode(sr1, strlen(sr1), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_SESSIONS_REPLACE && gev.ev.presence == CCORD_PRESENCE_DND);
  ccord_live_event_free(&gev);
  const char *sr2 = "{\"op\":0,\"t\":\"SESSIONS_REPLACE\",\"d\":[{\"session_id\":\"abc\",\"status\":\"idle\"}]}";
  CT_CHECK(ccord_live_gateway_decode(sr2, strlen(sr2), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type == CCORD_NET_SESSIONS_REPLACE && gev.ev.presence == CCORD_PRESENCE_IDLE);
  ccord_live_event_free(&gev);
  const char *sr3 = "{\"op\":0,\"t\":\"SESSIONS_REPLACE\",\"d\":[{\"session_id\":\"all\",\"status\":\"unknown\"}]}";
  CT_CHECK(ccord_live_gateway_decode(sr3, strlen(sr3), &gev, &err) == CCORD_OK);
  CT_CHECK(gev.ev.type != CCORD_NET_SESSIONS_REPLACE);
  ccord_live_event_free(&gev);

  // REST side: no base known -> GET settings-proto/1 first, then PATCH built on it; the next write reuses the answer.
  sp1_cfg cfgs = {0};
  fd_server srv;
  CT_CHECK(fd_start(&srv, respond_sp1, &cfgs));
  ccord_live_config cfg = {.token = "REDACTED_TEST_TOKEN", .api_base = srv.base, .disable_gateway = true,
                           .request_timeout_ms = 3000};
  ccord_transport *t = ccord_live_transport_create(&cfg, &err);
  CT_CHECK(t != nullptr && ccord_transport_connect(t, &err) == CCORD_OK);
  ccord_net_event ev;
  CT_CHECK(poll_live_for(t, CCORD_NET_STATE, &ev, 3000) && ev.state == CCORD_CONN_CONNECTED);

  ccord_proto_status want = {0};
  want.has_status = true;
  memcpy(want.status, "dnd", 4);
  CT_CHECK(ccord_transport_set_status_proto(t, &want, &err) == CCORD_OK);
  sp1_pump(t, 600);
  ccord_proto_status bad = want;
  memcpy(bad.status, "busy", 5);
  CT_CHECK(ccord_transport_set_status_proto(t, &bad, &err) == CCORD_ERR_INVALID);   // refused before any request
  ccord_proto_status want2 = {0};
  want2.has_status = true;
  memcpy(want2.status, "invisible", 10);
  want2.has_custom = true;
  memcpy(want2.custom_text, "brb", 4);
  CT_CHECK(ccord_transport_set_status_proto(t, &want2, &err) == CCORD_OK);
  sp1_pump(t, 600);
  ccord_transport_destroy(t);

  fd_hit hits[FD_MAX_HITS];
  size_t nhits = fd_stop(&srv, hits, FD_MAX_HITS);
  size_t gets = 0, patches = 0;
  const fd_hit *p1 = nullptr, *p2 = nullptr;
  for (size_t i = 0; i < nhits; i++) {
    if (strcmp(hits[i].path, "/users/@me/settings-proto/1") != 0) continue;
    if (strcmp(hits[i].method, "GET") == 0) gets++;
    if (strcmp(hits[i].method, "PATCH") == 0) {
      patches++;
      if (!p1) p1 = &hits[i];
      else if (!p2) p2 = &hits[i];
    }
  }
  CT_CHECK(gets == 1 && patches == 2);   // the first write fetched the base; the second built on the first's bytes
  char *exp1 = nullptr, *exp2 = nullptr;
  CT_CHECK(ccord_user_status_patch_b64(SP1_BASE_B64, strlen(SP1_BASE_B64), &want, &exp1, &err) == CCORD_OK);
  CT_CHECK(exp1 && ccord_user_status_patch_b64(exp1, strlen(exp1), &want2, &exp2, &err) == CCORD_OK);
  char want_body1[1024], want_body2[1024];
  snprintf(want_body1, sizeof want_body1, "{\"settings\":\"%s\"}", exp1 ? exp1 : "");
  snprintf(want_body2, sizeof want_body2, "{\"settings\":\"%s\"}", exp2 ? exp2 : "");
  CT_CHECK(p1 && strcmp(p1->body, want_body1) == 0);
  CT_CHECK(p2 && strcmp(p2->body, want_body2) == 0);
  if (exp1) {
    ccord_proto_status chk;
    CT_CHECK(ccord_user_status_decode_b64(exp1, strlen(exp1), &chk, &err) == CCORD_OK && strcmp(chk.status, "dnd") == 0);
  }
  ccord_free(exp1);
  ccord_free(exp2);

  // A failed GET leaves no base: nothing is PATCHed (a guess would overwrite the account's settings).
  sp1_cfg bad_cfg = {.fail_get = true};
  fd_server srv2;
  CT_CHECK(fd_start(&srv2, respond_sp1, &bad_cfg));
  ccord_live_config cfg2 = {.token = "REDACTED_TEST_TOKEN", .api_base = srv2.base, .disable_gateway = true,
                            .request_timeout_ms = 3000};
  ccord_transport *t2c = ccord_live_transport_create(&cfg2, &err);
  CT_CHECK(t2c != nullptr && ccord_transport_connect(t2c, &err) == CCORD_OK);
  CT_CHECK(poll_live_for(t2c, CCORD_NET_STATE, &ev, 3000));
  CT_CHECK(ccord_transport_set_status_proto(t2c, &want, &err) == CCORD_OK);
  sp1_pump(t2c, 600);
  ccord_transport_destroy(t2c);
  size_t nh2 = fd_stop(&srv2, hits, FD_MAX_HITS);
  size_t patches2 = 0;
  for (size_t i = 0; i < nh2; i++) {
    if (strcmp(hits[i].method, "PATCH") == 0) patches2++;
  }
  CT_CHECK(nh2 >= 1 && patches2 == 0);
}

static void test_guild_load_role_gated_and_self_roles(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY without inline merged_members (e.g., PRIORITIZED_READY_PAYLOAD where merged_members is deferred to
  //    READY_SUPPLEMENTAL) must preserve role-gated channels and parent categories whose children are visible.
  const char *ready_no_roles =
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"guilds\":[{\"id\":\"900\",\"properties\":{\"id\":\"900\",\"name\":\"Community\",\"owner_id\":\"5\"},"
    "\"roles\":[{\"id\":\"900\",\"name\":\"@everyone\",\"permissions\":\"0\"},"
    "{\"id\":\"901\",\"name\":\"Member\",\"permissions\":\"1024\"}],"
    "\"channels\":["
    "{\"id\":\"910\",\"type\":4,\"name\":\"Main\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
    "{\"id\":\"911\",\"type\":0,\"parent_id\":\"910\",\"name\":\"general\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"},"
    "{\"id\":\"901\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]}"
    "]}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_no_roles, strlen(ready_no_roles), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.guild_count == 1);
  CT_CHECK(strcmp(ev.snapshot.guilds[0].name, "Community") == 0);
  // Both parent category 910 and child channel 911 must remain present when self_roles wasn't delivered yet.
  CT_CHECK(ev.snapshot.channel_count == 2);
  ccord_live_event_free(&ev);

  // 2. When self_roles ARE known and deny VIEW_CHANNEL on a parent category, the category stays if a child is visible.
  const char *ready_cat_child =
    "{\"op\":0,\"t\":\"READY\",\"s\":2,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\"},"
    "\"merged_members\":[[{\"user_id\":\"42\",\"roles\":[\"901\"]}]],"
    "\"guilds\":[{\"id\":\"900\",\"properties\":{\"id\":\"900\",\"name\":\"Community\",\"owner_id\":\"5\"},"
    "\"roles\":[{\"id\":\"900\",\"name\":\"@everyone\",\"permissions\":\"0\"},"
    "{\"id\":\"901\",\"name\":\"Member\",\"permissions\":\"0\"}],"
    "\"channels\":["
    "{\"id\":\"910\",\"type\":4,\"name\":\"Hidden Cat\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
    "{\"id\":\"911\",\"type\":0,\"parent_id\":\"910\",\"name\":\"open-child\","
    "\"permission_overwrites\":[{\"id\":\"901\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]},"
    "{\"id\":\"912\",\"type\":0,\"parent_id\":\"910\",\"name\":\"secret-mod\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]}"
    "]}]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_cat_child, strlen(ready_cat_child), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.guild_count == 1 && ev.snapshot.channel_count == 2);
  CT_CHECK(ev.snapshot.channels[0].id == 910 && ev.snapshot.channels[1].id == 911);
  ccord_live_event_free(&ev);

  // 3. ccord_live_gateway_decode_ex collects self_roles on GUILD_CREATE when self_id is supplied.
  const char *guild_create =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"s\":3,\"d\":{\"id\":\"900\",\"name\":\"Community\",\"owner_id\":\"5\","
    "\"roles\":[{\"id\":\"900\",\"name\":\"@everyone\",\"permissions\":\"0\"},"
    "{\"id\":\"901\",\"name\":\"Member\",\"permissions\":\"0\"}],"
    "\"members\":[{\"user\":{\"id\":\"42\",\"username\":\"me\"},\"roles\":[\"901\"]}],"
    "\"channels\":[{\"id\":\"911\",\"type\":0,\"name\":\"general\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"},"
    "{\"id\":\"901\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]},"
    "{\"id\":\"912\",\"type\":0,\"name\":\"secret\","
    "\"permission_overwrites\":[{\"id\":\"900\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]}]}}";
  CT_CHECK(ccord_live_gateway_decode_ex(guild_create, strlen(guild_create), 42, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT);
  CT_CHECK(ev.snap_channel_count == 1 && ev.snap_channels[0].id == 911);
  ccord_live_event_free(&ev);
}

static void test_guild_profile_and_primary_guild_clan_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. Guild profile wrapped under "profile", integer brand_color_primary, and fallback missing name -> unavailable.
  const char *wrapped = "{\"profile\":{\"id\":\"900\",\"name\":\"Tag Server\",\"tag\":\"TAG\","
                        "\"badge\":\"bhash\",\"brand_color_primary\":16711680,"
                        "\"online_count\":12,\"member_count\":99}}";
  CT_CHECK(ccord_live_json_guild_profile(wrapped, strlen(wrapped), 900, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_GUILD_PROFILE && ev.ev.guild_profile.guild_id == 900);
  CT_CHECK(strcmp(ev.ev.guild_profile.name, "Tag Server") == 0);
  CT_CHECK(strcmp(ev.ev.guild_profile.tag, "TAG") == 0);
  CT_CHECK(ev.ev.guild_profile.has_brand_color && ev.ev.guild_profile.brand_color == 0xFF0000u);
  CT_CHECK(!ev.ev.guild_profile.unavailable && ev.ev.guild_profile.badge_url[0] != '\0');
  ccord_live_event_free(&ev);

  const char *empty_name = "{\"id\":\"901\",\"tag\":\"TAG\"}";
  CT_CHECK(ccord_live_json_guild_profile(empty_name, strlen(empty_name), 901, &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.guild_profile.unavailable);
  ccord_live_event_free(&ev);

  // 2. Partial "clan" without identity_guild_id merges missing identity_guild_id from "primary_guild".
  const char *ready_clan =
    "{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{\"user\":{\"id\":\"42\",\"username\":\"me\","
    "\"clan\":{\"tag\":\"TAG\",\"identity_enabled\":true},"
    "\"primary_guild\":{\"identity_guild_id\":\"900\",\"identity_enabled\":true,\"tag\":\"TAG\",\"badge\":\"bhash\"}},"
    "\"guilds\":[]}}";
  CT_CHECK(ccord_live_gateway_decode(ready_clan, strlen(ready_clan), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.snapshot.user_count >= 1);
  CT_CHECK(strcmp(ev.snapshot.users[0].clan_tag, "TAG") == 0 && ev.snapshot.users[0].clan_guild_id == 900);
  ccord_live_event_free(&ev);
}

static void test_ready_supplemental_and_passive_voice_states_json(void)
{
  ccord_live_event ev;
  ccord_error err;

  // 1. READY_SUPPLEMENTAL carries guild voice_states (omitted from prioritized READY).
  const char *rsupp =
    "{\"op\":0,\"t\":\"READY_SUPPLEMENTAL\",\"s\":2,\"d\":{"
    "\"guilds\":["
    "{\"id\":\"900\",\"voice_states\":["
    "{\"user_id\":\"101\",\"channel_id\":\"911\",\"self_mute\":true,\"self_stream\":true,\"suppress\":false},"
    "{\"user_id\":\"102\",\"channel_id\":null}"
    "]},"
    "{\"id\":\"901\",\"voice_states\":["
    "{\"user_id\":\"103\",\"channel_id\":\"921\",\"self_video\":true,"
    "\"member\":{\"user\":{\"id\":\"103\",\"username\":\"carol\",\"global_name\":\"Carol\"}}}"
    "]}"
    "],"
    "\"merged_members\":[[{\"user\":{\"id\":\"101\",\"username\":\"alice\",\"global_name\":\"Alice\"}}]]}}";
  CT_CHECK(ccord_live_gateway_decode(rsupp, strlen(rsupp), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATES);
  CT_CHECK(ev.ev.guild_id == CCORD_ID_NONE && !ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 2);
  CT_CHECK(ev.ev.voice_states[0].guild_id == 900 && ev.ev.voice_states[0].channel_id == 911);
  CT_CHECK(ev.ev.voice_states[0].user_id == 101 && ev.ev.voice_states[0].self_mute && ev.ev.voice_states[0].self_stream);
  CT_CHECK(ev.ev.voice_states[1].guild_id == 901 && ev.ev.voice_states[1].channel_id == 921);
  CT_CHECK(ev.ev.voice_states[1].user_id == 103 && ev.ev.voice_states[1].self_video);
  CT_CHECK(ev.ev.user_count == 2);
  ccord_live_event_free(&ev);

  // Empty READY_SUPPLEMENTAL is valid and emits no event.
  const char *rsupp_empty = "{\"op\":0,\"t\":\"READY_SUPPLEMENTAL\",\"s\":3,\"d\":{\"guilds\":[{\"id\":\"900\",\"voice_states\":[]}]}}";
  CT_CHECK(ccord_live_gateway_decode(rsupp_empty, strlen(rsupp_empty), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_NONE);
  ccord_live_event_free(&ev);

  // 2. PASSIVE_UPDATE_V2 carries updated_voice_states, removed_voice_states, and updated_members.
  const char *pv2 =
    "{\"op\":0,\"t\":\"PASSIVE_UPDATE_V2\",\"s\":4,\"d\":{"
    "\"guild_id\":\"900\","
    "\"updated_members\":[{\"user\":{\"id\":\"104\",\"username\":\"dave\",\"global_name\":\"Dave\"}}],"
    "\"updated_voice_states\":["
    "{\"user_id\":\"104\",\"channel_id\":\"911\",\"self_deaf\":true},"
    "{\"user_id\":\"101\",\"channel_id\":null}"
    "],"
    "\"removed_voice_states\":[\"105\"]}}";
  CT_CHECK(ccord_live_gateway_decode(pv2, strlen(pv2), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATES);
  CT_CHECK(ev.ev.guild_id == 900 && !ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 3);
  CT_CHECK(ev.ev.voice_states[0].user_id == 104 && ev.ev.voice_states[0].channel_id == 911 && ev.ev.voice_states[0].self_deaf);
  CT_CHECK(ev.ev.voice_states[1].user_id == 101 && ev.ev.voice_states[1].channel_id == CCORD_ID_NONE);
  CT_CHECK(ev.ev.voice_states[2].user_id == 105 && ev.ev.voice_states[2].channel_id == CCORD_ID_NONE);
  CT_CHECK(ev.ev.user_count == 1 && ev.ev.users[0].id == 104 && strcmp(ev.ev.users[0].name, "Dave") == 0);
  ccord_live_event_free(&ev);

  // 3. PASSIVE_UPDATE_V1 carries authoritative guild voice_states.
  const char *pv1 =
    "{\"op\":0,\"t\":\"PASSIVE_UPDATE_V1\",\"s\":5,\"d\":{\"guild_id\":\"900\",\"voice_states\":["
    "{\"user_id\":\"104\",\"channel_id\":\"911\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(pv1, strlen(pv1), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.ev.type == CCORD_NET_VOICE_STATES && ev.ev.guild_id == 900 && ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 1 && ev.ev.voice_states[0].user_id == 104);
  ccord_live_event_free(&ev);

  // 4. GUILD_CREATE surfaces voice_states with replace_voice_states=true; GUILD_UPDATE without voice_states keeps false.
  const char *gcreate =
    "{\"op\":0,\"t\":\"GUILD_CREATE\",\"s\":6,\"d\":{\"id\":\"900\",\"name\":\"G\","
    "\"channels\":[{\"id\":\"911\",\"type\":2,\"name\":\"Voice\"}],"
    "\"voice_states\":[{\"user_id\":\"104\",\"channel_id\":\"911\"}]}}";
  CT_CHECK(ccord_live_gateway_decode(gcreate, strlen(gcreate), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && ev.ev.replace_voice_states);
  CT_CHECK(ev.ev.voice_state_count == 1 && ev.ev.voice_states[0].user_id == 104 && ev.ev.voice_states[0].channel_id == 911);
  ccord_live_event_free(&ev);

  const char *gupdate = "{\"op\":0,\"t\":\"GUILD_UPDATE\",\"s\":7,\"d\":{\"id\":\"900\",\"name\":\"G2\"}}";
  CT_CHECK(ccord_live_gateway_decode(gupdate, strlen(gupdate), &ev, &err) == CCORD_OK);
  CT_CHECK(ev.guild_op == CCORD_LIVE_GUILD_UPSERT && !ev.ev.replace_voice_states && ev.ev.voice_state_count == 0);
  ccord_live_event_free(&ev);
}

int main(void)
{
  test_token_env();
  test_token_file();
  test_token_save_load_delete();
  test_multi_account_keyring();
  test_payment_routes_blocked();
  test_user_video_url_allowed();
  test_rest_url_and_headers();
  test_rest_authorization_header();
  test_gateway_message();
  test_gateway_controls_and_state();
  test_gateway_ready_snapshot();
  test_gateway_ready_large_nodes();
  test_ready_reserves_dm_slot();
  test_gateway_payload_builders();
  test_gateway_session_lifecycle();
  test_gateway_guild_events();
  test_gateway_member_events();
  test_gateway_permission_data();
  test_gateway_notify_settings();
  test_polls_components_and_modals_json();
  test_slash_commands_ephemeral_reply_and_uploads_json();
  test_search_pins_threads_and_burst_reactions_json();
  test_m3b_folders_invite_and_discovery_json();
  test_m3c_social_presence_requests_and_profile_json();
  test_m4a_guild_settings_incidents_vanity_and_audit_changes_json();
  test_m4b_expressions_json_and_gateway();
  test_m4c_members_prune_and_automod_json();
  test_discord_error_message();
  test_message_reply_attachments_embeds();
  test_rich_message_decode();
  test_rich_decode_details();
  test_rich_decode_edges();
  test_gateway_voice_and_stream();
  test_voice_stream_payload_builders();
  test_gateway_presence_builder();
  test_voice_gateway_builders_and_decode();
  test_voice_gateway_state_machine();
  test_gateway_multi_camera();
  test_gateway_camera_idle_demotion();
  test_gateway_camera_legacy_path();
  test_voice_media_framing();
  test_json_search_upload_and_dm();
  test_ready_v2_properties_permissions_sorting_and_dms();
  test_ready_merged_members_and_message_edge_cases();
  test_guild_folders_proto_dm_read_state_and_user_profile();
  test_forum_json_and_gateway_events();
  test_rest_queue_rate_limit_and_pins();
  test_history_json();
  test_live_transport_create();
  test_fetch_media_range_gate();
  test_interaction_subcommands_and_context_menus();
  test_member_screening_json_and_pending();
  test_loopback_messaging_proto_stage_and_events();
  test_raw_media_download();
  test_account_scoped_cache_and_history_revalidate();
  test_network_event_hook();
  test_status_proto_write_and_updates();
  test_guild_load_role_gated_and_self_roles();
  test_guild_profile_and_primary_guild_clan_json();
  test_ready_supplemental_and_passive_voice_states_json();
  return CT_RESULT("test_live_transport");
}
