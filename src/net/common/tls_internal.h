// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Shared strict TLS setup, one-time libcurl initialization, and public-IP SSRF socket filter.
#include <curl/curl.h>
#include <stddef.h>

#include <ccord/core/os.h>

void ccord_curl_global_init_once(void);
void ccord_curl_apply_tls(CURL *curl);
void ccord_curl_apply_public_opensocket(CURL *curl);
