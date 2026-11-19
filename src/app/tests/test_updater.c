// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "updater.h"

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

int main(void)
{
  test_semver();
  test_parse_assets();
  test_sha256_file();
  return failures == 0 ? 0 : 1;
}
