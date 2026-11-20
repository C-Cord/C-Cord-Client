// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "live_transport_harness.h"

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


int main(void)
{
  test_token_env();
  test_token_file();
  test_payment_routes_blocked();
  test_user_video_url_allowed();
  test_rest_url_and_headers();
  test_rest_authorization_header();
  test_gateway_message();
  test_gateway_controls_and_state();
  test_gateway_ready_snapshot();
  test_gateway_ready_large_nodes();
  test_gateway_payload_builders();
  test_gateway_session_lifecycle();
  test_gateway_guild_events();
  test_message_reply_attachments_embeds();
  test_rich_message_decode();
  test_rich_decode_details();
  test_rich_decode_edges();
  test_gateway_voice_and_stream();
  test_voice_stream_payload_builders();
  test_history_json();
  test_live_transport_create();
  test_fetch_media_range_gate();
  return CT_RESULT("test_live_auth_policy");
}
