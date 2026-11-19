// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Shared strict TLS setup for every libcurl handle in ccord_net.
// On Android, OpenSSL has no /etc/ssl/certs/ca-certificates.crt default: prefer an explicit
// bundle ($CCORD_CA_BUNDLE or $SSL_CERT_FILE extracted from APK assets), falling back to
// Android 14+ Conscrypt APEX certs (/apex/com.android.conscrypt/cacerts) and legacy
// /system/etc/security/cacerts.
#include <curl/curl.h>
#include <stdlib.h>

#include <ccord/core/os.h>

static inline void ccord_curl_apply_tls(CURL *curl)
{
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  const char *cafile = getenv("CCORD_CA_BUNDLE");
  if (!cafile || !cafile[0]) cafile = getenv("SSL_CERT_FILE");
  if (cafile && cafile[0] && ccord_path_is_file(cafile)) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, cafile);
    return;
  }
#if CCORD_OS_ANDROID
  if (ccord_path_is_dir("/apex/com.android.conscrypt/cacerts")) {
    curl_easy_setopt(curl, CURLOPT_CAPATH, "/apex/com.android.conscrypt/cacerts");
  } else if (ccord_path_is_dir("/system/etc/security/cacerts")) {
    curl_easy_setopt(curl, CURLOPT_CAPATH, "/system/etc/security/cacerts");
  }
#endif
}
