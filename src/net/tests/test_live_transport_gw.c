// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "live_transport_harness.h"
#include <ccord/net/voice_crypto.h>

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
  const char *expected_mode = ccord_voice_aes_hw_available() ? "\"mode\":\"aead_aes256_gcm_rtpsize\""
                                                             : "\"mode\":\"aead_xchacha20_poly1305_rtpsize\"";
  CT_CHECK(strstr(cap.frames[1], expected_mode) != nullptr);

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


int main(void)
{
  test_token_save_load_delete();
  test_multi_account_keyring();
  test_ready_reserves_dm_slot();
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
  test_rest_queue_rate_limit_and_pins();
  return CT_RESULT("test_live_transport_gw");
}
