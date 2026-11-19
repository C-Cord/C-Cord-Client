// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Shared strict TLS setup and one-time libcurl initialization for ccord_net.
// On Android, OpenSSL has no /etc/ssl/certs/ca-certificates.crt default: prefer an explicit
// bundle ($CCORD_CA_BUNDLE or $SSL_CERT_FILE extracted from APK assets), falling back to
// Android 14+ Conscrypt APEX certs (/apex/com.android.conscrypt/cacerts) and legacy
// /system/etc/security/cacerts.
#include <curl/curl.h>
#include <stddef.h>

#include <ccord/core/os.h>

// Thread-safe one-time curl_global_init(CURL_GLOBAL_DEFAULT), implemented in discord_rest.c.
void ccord_curl_global_init_once(void);

// Applies strict TLS verification and cached CA bundle / CA path to `curl`, implemented in discord_rest.c.
void ccord_curl_apply_tls(CURL *curl);

// Internal zero-copy handoff for non-sensitive, unshared REST responses (callable only from inside a
// ccord_rest_callback on `q`). Caller takes ownership of the returned ccord_malloc'd buffer and frees it
// with ccord_free. Returns nullptr if the response cannot be transferred.
struct ccord_rest_queue;
char *ccord_rest_take_active_resp(struct ccord_rest_queue *q, size_t *out_len);
