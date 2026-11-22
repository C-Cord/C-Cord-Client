// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "updater.h"

#include <ccord/net/http_fetch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr)                                                               \
  do {                                                                            \
    if (!(expr)) {                                                                \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);    \
      failures++;                                                                 \
    }                                                                             \
  } while (0)

static void test_semver(void)
{
  CHECK(ccord_updater_semver_compare("0.1.0", "0.2.0") < 0);
  CHECK(ccord_updater_semver_compare("v0.2.0", "0.2.0") == 0);
  CHECK(ccord_updater_semver_compare("0.1.0+7ead76b", "v0.1.0") == 0);
  CHECK(ccord_updater_semver_compare("0.1.0+7ead76b", "v0.1.1") < 0);
  CHECK(ccord_updater_semver_compare("0.10.0", "0.2.0") > 0);
  CHECK(ccord_updater_semver_compare("1.0.0-rc.1", "1.0.0") < 0);
  CHECK(ccord_updater_semver_compare("1.0.0-rc.1", "1.0.0-rc.2") < 0);
  CHECK(ccord_updater_semver_compare("1.0.0-beta.2", "1.0.0-rc.1") < 0);
}

static void test_parse_assets(void)
{
  static const char k_json[] =
      "{"
      "  \"release\": {"
      "    \"tag\": \"v0.3.0\","
      "    \"html_url\": \"https://github.com/C-Cord/C-Cord-Client/releases/tag/v0.3.0\","
      "    \"assets\": ["
      "      {\"name\":\"C-Cord-0.3.0-linux-x86_64.AppImage\",\"os\":\"linux\",\"arch\":\"x86_64\",\"format\":\"appimage\",\"size_bytes\":123456,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"download_url\":\"https://github.com/C-Cord/C-Cord-Client/releases/download/v0.3.0/C-Cord-0.3.0-linux-x86_64.AppImage\"},"
      "      {\"name\":\"c-cord_0.3.0_amd64.deb\",\"os\":\"linux\",\"arch\":\"x86_64\",\"format\":\"deb\",\"size_bytes\":55555,\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"download_url\":\"https://github.com/C-Cord/C-Cord-Client/releases/download/v0.3.0/c-cord_0.3.0_amd64.deb\"},"
      "      {\"name\":\"C-Cord-0.3.0-windows-x86_64-setup.exe\",\"os\":\"windows\",\"arch\":\"x86_64\",\"format\":\"exe\",\"size_bytes\":99999,\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"download_url\":\"https://github.com/C-Cord/C-Cord-Client/releases/download/v0.3.0/C-Cord-0.3.0-windows-x86_64-setup.exe\"},"
      "      {\"name\":\"C-Cord-0.3.0-windows-x86_64-portable.zip\",\"os\":\"windows\",\"arch\":\"x86_64\",\"format\":\"zip\",\"size_bytes\":88888,\"sha256\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\",\"download_url\":\"https://github.com/C-Cord/C-Cord-Client/releases/download/v0.3.0/C-Cord-0.3.0-windows-x86_64-portable.zip\"},"
      "      {\"name\":\"C-Cord-0.3.0-macos-arm64.dmg\",\"os\":\"macos\",\"arch\":\"arm64\",\"format\":\"dmg\",\"size_bytes\":77777,\"sha256\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\",\"download_url\":\"https://github.com/C-Cord/C-Cord-Client/releases/download/v0.3.0/C-Cord-0.3.0-macos-arm64.dmg\"}"
      "    ]"
      "  }"
      "}";

  ccord_update_parsed p;
  CHECK(ccord_updater_parse_latest(k_json, sizeof k_json - 1, "linux", "x86_64", CCORD_INSTALL_APPIMAGE,
                                   "https://c-cord.org", &p));
  CHECK(p.found);
  CHECK(strcmp(p.tag, "v0.3.0") == 0);
  CHECK(strcmp(p.asset_name, "C-Cord-0.3.0-linux-x86_64.AppImage") == 0);
  CHECK(strcmp(p.download_url, "https://c-cord.org/download/v0.3.0/C-Cord-0.3.0-linux-x86_64.AppImage") == 0);

  // Native Linux package installs must never select a self-update binary.
  CHECK(ccord_updater_parse_latest(k_json, sizeof k_json - 1, "linux", "x86_64", CCORD_INSTALL_SYSTEM_PKG,
                                   "https://c-cord.org", &p));
  CHECK(!p.found);

  // Windows prefers .exe setup installer over .zip portable.
  CHECK(ccord_updater_parse_latest(k_json, sizeof k_json - 1, "windows", "x86_64", CCORD_INSTALL_WINDOWS,
                                   "https://c-cord.org", &p));
  CHECK(p.found);
  CHECK(strcmp(p.asset_name, "C-Cord-0.3.0-windows-x86_64-setup.exe") == 0);
  CHECK(strcmp(p.download_url, "https://c-cord.org/download/v0.3.0/C-Cord-0.3.0-windows-x86_64-setup.exe") == 0);

  // macOS arm64 picks .dmg.
  CHECK(ccord_updater_parse_latest(k_json, sizeof k_json - 1, "macos", "arm64", CCORD_INSTALL_MACOS,
                                   "https://c-cord.org", &p));
  CHECK(p.found);
  CHECK(strcmp(p.asset_name, "C-Cord-0.3.0-macos-arm64.dmg") == 0);

  // Security regressions (P9): missing/malformed sha256 or traversal segment must be rejected.
  static const char k_no_sha[] =
      "{\"release\":{\"tag\":\"v0.3.0\",\"assets\":["
      "{\"name\":\"C-Cord-0.3.0.AppImage\",\"os\":\"linux\",\"arch\":\"x86_64\",\"format\":\"appimage\","
      "\"download_url\":\"https://evil.example/payload.AppImage\"}]}}";
  CHECK(ccord_updater_parse_latest(k_no_sha, sizeof k_no_sha - 1, "linux", "x86_64", CCORD_INSTALL_APPIMAGE,
                                   "https://c-cord.org", &p));
  CHECK(!p.found);

  static const char k_bad_seg[] =
      "{\"release\":{\"tag\":\"../v0.3.0\",\"assets\":["
      "{\"name\":\"C-Cord-0.3.0.AppImage\",\"os\":\"linux\",\"arch\":\"x86_64\",\"format\":\"appimage\","
      "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}}";
  CHECK(ccord_updater_parse_latest(k_bad_seg, sizeof k_bad_seg - 1, "linux", "x86_64", CCORD_INSTALL_APPIMAGE,
                                   "https://c-cord.org", &p));
  CHECK(!p.found);
}

static void test_http_allowlist(void)
{
  // Security regression (P8): strict HTTPS host allowlist blocks userinfo/backslash/port tricks.
  CHECK(ccord_http_url_allowed("https://c-cord.org/api/v1/latest"));
  CHECK(ccord_http_url_allowed("https://plugins.c-cord.org/d/foo.cce"));
  CHECK(ccord_http_url_allowed("https://github.com/C-Cord/C-Cord-Client/releases"));
  CHECK(ccord_http_url_allowed("https://objects.githubusercontent.com/pkg"));
  CHECK(!ccord_http_url_allowed("http://c-cord.org/api/v1/latest"));
  CHECK(!ccord_http_url_allowed("https://c-cord.org@evil.example/x"));
  CHECK(!ccord_http_url_allowed("https://c-cord.org\\@evil.example/x"));
  CHECK(!ccord_http_url_allowed("https://c-cord.org:8443/x"));
  CHECK(!ccord_http_url_allowed("https://evil.example/x"));
}

static void test_sha256_file(void)
{
  const char *tmp = "test_updater_sha256.tmp";
  FILE *f = fopen(tmp, "wb");
  CHECK(f != nullptr);
  if (f) {
    fwrite("abc", 1, 3, f);
    fclose(f);
  }
  char hex[65] = {0};
  CHECK(ccord_updater_sha256_file(tmp, hex));
  CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
  remove(tmp);
}

static void test_apply_check_result(void)
{
  static const char k_json[] =
      "{"
      "  \"release\": {"
      "    \"tag\": \"v0.3.0\","
      "    \"html_url\": \"https://github.com/C-Cord/C-Cord-Client/releases/tag/v0.3.0\","
      "    \"assets\": ["
      "      {\"name\":\"C-Cord-0.3.0-linux-x86_64.AppImage\",\"os\":\"linux\",\"arch\":\"x86_64\",\"format\":\"appimage\",\"size_bytes\":123456,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"
      "    ]"
      "  }"
      "}";

  // System package: notifies of new version without allowing in-app install.
  ccord_update_parsed p = {0};
  CHECK(ccord_updater_parse_latest(k_json, sizeof k_json - 1, "linux", "x86_64", CCORD_INSTALL_SYSTEM_PKG,
                                   "https://c-cord.org", &p));
  CHECK(!p.found);
  ccord_ui_update_info ui = {0};
  CHECK(!ccord_updater_apply_check_result("0.1.0", CCORD_INSTALL_SYSTEM_PKG, &p, true, &ui));
  CHECK(ui.state == CCORD_UI_UPDATE_AVAILABLE);
  CHECK(!ui.self_updatable);
  CHECK(!ui.can_install);
  CHECK(strcmp(ui.latest_version, "v0.3.0") == 0);
  CHECK(strcmp(ui.release_url, "https://github.com/C-Cord/C-Cord-Client/releases/tag/v0.3.0") == 0);
  CHECK(strstr(ui.status_text, "package manager") != nullptr);

  // Android: notifies of new version without allowing in-app install.
  ccord_ui_update_info aui = {0};
  CHECK(!ccord_updater_apply_check_result("0.1.0", CCORD_INSTALL_ANDROID, &p, true, &aui));
  CHECK(aui.state == CCORD_UI_UPDATE_AVAILABLE);
  CHECK(!aui.self_updatable);
  CHECK(!aui.can_install);
  CHECK(strstr(aui.status_text, "F-Droid") != nullptr);

  // System package already up to date.
  ccord_ui_update_info utd = {0};
  CHECK(!ccord_updater_apply_check_result("0.3.0", CCORD_INSTALL_SYSTEM_PKG, &p, true, &utd));
  CHECK(utd.state == CCORD_UI_UPDATE_UP_TO_DATE);
  CHECK(!utd.can_install);

  // AppImage with verified asset: notifies AND enables in-app install.
  ccord_update_parsed pa = {0};
  CHECK(ccord_updater_parse_latest(k_json, sizeof k_json - 1, "linux", "x86_64", CCORD_INSTALL_APPIMAGE,
                                   "https://c-cord.org", &pa));
  CHECK(pa.found);
  ccord_ui_update_info aiu = {0};
  CHECK(ccord_updater_apply_check_result("0.1.0", CCORD_INSTALL_APPIMAGE, &pa, true, &aiu));
  CHECK(aiu.state == CCORD_UI_UPDATE_AVAILABLE);
  CHECK(aiu.self_updatable);
  CHECK(aiu.can_install);

  // Self-updatable install without a verified matching asset: notifies, but refuses in-app install.
  ccord_update_parsed p_unverified = pa;
  p_unverified.found = false;
  ccord_ui_update_info nou = {0};
  CHECK(!ccord_updater_apply_check_result("0.1.0", CCORD_INSTALL_APPIMAGE, &p_unverified, true, &nou));
  CHECK(nou.state == CCORD_UI_UPDATE_AVAILABLE);
  CHECK(nou.self_updatable);
  CHECK(!nou.can_install);

  // Traversal tag must not trigger an update notification or bogus release URL.
  static const char k_bad_tag[] = "{\"release\":{\"tag\":\"../v0.3.0\",\"assets\":[]}}";
  ccord_update_parsed pb = {0};
  CHECK(ccord_updater_parse_latest(k_bad_tag, sizeof k_bad_tag - 1, "linux", "x86_64", CCORD_INSTALL_SYSTEM_PKG,
                                   "https://c-cord.org", &pb));
  CHECK(pb.release_url[0] == '\0');
  ccord_ui_update_info bui = {0};
  CHECK(!ccord_updater_apply_check_result("0.1.0", CCORD_INSTALL_SYSTEM_PKG, &pb, true, &bui));
  CHECK(bui.state == CCORD_UI_UPDATE_UP_TO_DATE);
  CHECK(!bui.can_install);
}

int main(void)
{
  test_semver();
  test_parse_assets();
  test_apply_check_result();
  test_http_allowlist();
  test_sha256_file();
  return failures == 0 ? 0 : 1;
}
