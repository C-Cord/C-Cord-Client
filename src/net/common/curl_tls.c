// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include "common/tls_internal.h"

#include <ccord/core/os.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__ANDROID__) && !defined(__APPLE__) && !defined(_WIN32) && defined(__has_include)
#if __has_include(<threads.h>) && !defined(__STDC_NO_THREADS__)
#define CCORD_REST_HAVE_C11_THREADS 1
#include <threads.h>
#endif
#endif

typedef enum tls_ca_mode : uint8_t {
  TLS_CA_DEFAULT = 0,
  TLS_CA_FILE = 1,
  TLS_CA_DIR = 2,
} tls_ca_mode;

static tls_ca_mode g_tls_ca_mode = TLS_CA_DEFAULT;
static char g_tls_ca_path[4096];

static void tls_ca_resolve_impl(void)
{
  const char *cafile = getenv("CCORD_CA_BUNDLE");
  if (!cafile || !cafile[0]) cafile = getenv("SSL_CERT_FILE");
  if (cafile && cafile[0] && ccord_path_is_file(cafile)) {
    size_t n = strlen(cafile);
    if (n < sizeof g_tls_ca_path) {
      memcpy(g_tls_ca_path, cafile, n + 1u);
      g_tls_ca_mode = TLS_CA_FILE;
      return;
    }
  }
#if CCORD_OS_ANDROID
  static const char k_apex[] = "/apex/com.android.conscrypt/cacerts";
  static const char k_legacy[] = "/system/etc/security/cacerts";
  if (ccord_path_is_dir(k_apex)) {
    memcpy(g_tls_ca_path, k_apex, sizeof k_apex);
    g_tls_ca_mode = TLS_CA_DIR;
  } else if (ccord_path_is_dir(k_legacy)) {
    memcpy(g_tls_ca_path, k_legacy, sizeof k_legacy);
    g_tls_ca_mode = TLS_CA_DIR;
  }
#endif
}

#if defined(CCORD_REST_HAVE_C11_THREADS)
static once_flag g_curl_init_once = ONCE_FLAG_INIT;
static once_flag g_tls_ca_once = ONCE_FLAG_INIT;

static void curl_global_init_cb(void) { (void)curl_global_init(CURL_GLOBAL_DEFAULT); }

void ccord_curl_global_init_once(void) { call_once(&g_curl_init_once, curl_global_init_cb); }

static void tls_ca_ensure_resolved(void) { call_once(&g_tls_ca_once, tls_ca_resolve_impl); }
#else
static _Atomic int g_curl_init_state = 0;  // 0 = uninit, 1 = running, 2 = done
static _Atomic int g_tls_ca_state = 0;

void ccord_curl_global_init_once(void)
{
  if (atomic_load_explicit(&g_curl_init_state, memory_order_acquire) == 2) return;
  int expected = 0;
  if (atomic_compare_exchange_strong_explicit(&g_curl_init_state, &expected, 1,
                                              memory_order_acq_rel, memory_order_acquire)) {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
    atomic_store_explicit(&g_curl_init_state, 2, memory_order_release);
  } else {
    while (atomic_load_explicit(&g_curl_init_state, memory_order_acquire) != 2) {
      ccord_sleep_ms(1);
    }
  }
}

static void tls_ca_ensure_resolved(void)
{
  if (atomic_load_explicit(&g_tls_ca_state, memory_order_acquire) == 2) return;
  int expected = 0;
  if (atomic_compare_exchange_strong_explicit(&g_tls_ca_state, &expected, 1,
                                              memory_order_acq_rel, memory_order_acquire)) {
    tls_ca_resolve_impl();
    atomic_store_explicit(&g_tls_ca_state, 2, memory_order_release);
  } else {
    while (atomic_load_explicit(&g_tls_ca_state, memory_order_acquire) != 2) {
      ccord_sleep_ms(1);
    }
  }
}
#endif

void ccord_curl_apply_tls(CURL *curl)
{
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  tls_ca_ensure_resolved();
  if (g_tls_ca_mode == TLS_CA_FILE) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, g_tls_ca_path);
  } else if (g_tls_ca_mode == TLS_CA_DIR) {
    curl_easy_setopt(curl, CURLOPT_CAPATH, g_tls_ca_path);
  }
}


static bool ipv4_is_blocked(uint32_t ip)
{
  // 0.0.0.0/8 (unspecified / "this network")
  if ((ip & 0xFF000000u) == 0x00000000u) return true;
  // 10.0.0.0/8 (RFC 1918)
  if ((ip & 0xFF000000u) == 0x0A000000u) return true;
  // 100.64.0.0/10 (RFC 6598 CGNAT)
  if ((ip & 0xFFC00000u) == 0x64400000u) return true;
  // 127.0.0.0/8 (loopback)
  if ((ip & 0xFF000000u) == 0x7F000000u) return true;
  // 169.254.0.0/16 (link-local / cloud metadata)
  if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return true;
  // 172.16.0.0/12 (RFC 1918)
  if ((ip & 0xFFF00000u) == 0xAC100000u) return true;
  // 192.168.0.0/16 (RFC 1918)
  if ((ip & 0xFFFF0000u) == 0xC0A80000u) return true;
  // 224.0.0.0/4 (multicast) and 240.0.0.0/4 (reserved / broadcast)
  if ((ip & 0xE0000000u) == 0xE0000000u) return true;
  return false;
}

static curl_socket_t rest_opensocket_cb(void *clientp, curlsocktype purpose, struct curl_sockaddr *address)
{
  (void)clientp;
  (void)purpose;
  if (address->family == AF_INET && address->addrlen >= sizeof(struct sockaddr_in)) {
    struct sockaddr_in sin;
    memcpy(&sin, &address->addr, sizeof sin);
    uint32_t ip = ntohl(sin.sin_addr.s_addr);
    if (ipv4_is_blocked(ip)) return CURL_SOCKET_BAD;
  } else if (address->family == AF_INET6 && address->addrlen >= sizeof(struct sockaddr_in6)) {
    struct sockaddr_in6 sin6;
    memcpy(&sin6, &address->addr, sizeof sin6);
    const uint8_t *a = sin6.sin6_addr.s6_addr;
    bool all_zero_12 = true;
    for (int i = 0; i < 12; i++) {
      if (a[i] != 0) { all_zero_12 = false; break; }
    }
    // :: unspecified and ::1 loopback
    if (all_zero_12 && a[12] == 0 && a[13] == 0 && a[14] == 0 && (a[15] == 0 || a[15] == 1)) {
      return CURL_SOCKET_BAD;
    }
    // ::ffff:0:0/96 IPv4-mapped IPv6
    bool is_v4mapped = true;
    for (int i = 0; i < 10; i++) {
      if (a[i] != 0) { is_v4mapped = false; break; }
    }
    if (is_v4mapped && a[10] == 0xff && a[11] == 0xff) {
      uint32_t v4 = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16) | ((uint32_t)a[14] << 8) | (uint32_t)a[15];
      if (ipv4_is_blocked(v4)) return CURL_SOCKET_BAD;
    }
    // 64:ff9b::/96 NAT64 well-known prefix
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xff && a[3] == 0x9b &&
        a[4] == 0 && a[5] == 0 && a[6] == 0 && a[7] == 0 &&
        a[8] == 0 && a[9] == 0 && a[10] == 0 && a[11] == 0) {
      uint32_t v4 = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16) | ((uint32_t)a[14] << 8) | (uint32_t)a[15];
      if (ipv4_is_blocked(v4)) return CURL_SOCKET_BAD;
    }
    // fc00::/7 Unique Local Address (ULA)
    if ((a[0] & 0xfe) == 0xfc) return CURL_SOCKET_BAD;
    // fe80::/10 link-local
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return CURL_SOCKET_BAD;
    // ff00::/8 multicast
    if (a[0] == 0xff) return CURL_SOCKET_BAD;
  } else {
    return CURL_SOCKET_BAD;
  }
  return socket(address->family, address->socktype, address->protocol);
}

void ccord_curl_apply_public_opensocket(CURL *curl)
{
  if (curl) curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, rest_opensocket_cb);
}

