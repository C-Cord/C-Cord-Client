// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/base64.h>

#include <ccord/core/alloc.h>

#include <stdckdint.h>

size_t ccord_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
  static const char k_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!out || out_cap == 0) return 0;
  if (in_len == 0) {
    out[0] = '\0';
    return 0;
  }
  if (!in) return 0;
  size_t groups = (in_len + 2u) / 3u;
  size_t need = 0;
  if (ckd_mul(&need, groups, 4u) || ckd_add(&need, need, 1u) || need > out_cap) return 0;
  size_t o = 0;
  for (size_t i = 0; i < in_len; i += 3u) {
    uint32_t v = (uint32_t)in[i] << 16u;
    if (i + 1u < in_len) v |= (uint32_t)in[i + 1u] << 8u;
    if (i + 2u < in_len) v |= (uint32_t)in[i + 2u];
    out[o++] = k_tab[(v >> 18u) & 63u];
    out[o++] = k_tab[(v >> 12u) & 63u];
    out[o++] = (i + 1u < in_len) ? k_tab[(v >> 6u) & 63u] : '=';
    out[o++] = (i + 2u < in_len) ? k_tab[v & 63u] : '=';
  }
  out[o] = '\0';
  return o;
}

char *ccord_base64_encode_alloc(const uint8_t *in, size_t in_len, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (in_len > 0 && !in) return nullptr;
  size_t groups = (in_len + 2u) / 3u;
  size_t cap = 0;
  if (ckd_mul(&cap, groups, 4u) || ckd_add(&cap, cap, 1u)) return nullptr;
  char *buf = ccord_malloc(cap);
  if (!buf) return nullptr;
  size_t n = ccord_base64_encode(in, in_len, buf, cap);
  if (in_len > 0 && n == 0) {
    ccord_free(buf);
    return nullptr;
  }
  if (out_len) *out_len = n;
  return buf;
}

static int b64_val(unsigned char c)
{
  if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
  if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
  if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

bool ccord_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (in_len > 0 && !in) return false;
  uint32_t acc = 0;
  unsigned bits = 0;
  size_t o = 0;
  bool saw_pad = false;
  for (size_t i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (c == '=') {
      saw_pad = true;
      continue;
    }
    if (saw_pad) return false;
    int v = b64_val(c);
    if (v < 0) return false;
    acc = (acc << 6u) | (uint32_t)v;
    bits += 6u;
    if (bits >= 8u) {
      bits -= 8u;
      if (o >= out_cap) return false;
      if (out) out[o] = (uint8_t)((acc >> bits) & 0xFFu);
      o++;
    }
  }
  if (bits == 6u) return false; // 1 trailing base64 char cannot form a byte
  if (out_len) *out_len = o;
  return true;
}

uint8_t *ccord_base64_decode_alloc(const char *in, size_t in_len, size_t *out_len)
{
  if (out_len) *out_len = 0;
  if (in_len > 0 && !in) return nullptr;
  size_t cap = ccord_base64_decoded_max(in_len);
  size_t alloc_sz = 0;
  if (ckd_add(&alloc_sz, cap, 1u)) return nullptr;
  uint8_t *buf = ccord_malloc(alloc_sz);
  if (!buf) return nullptr;
  size_t n = 0;
  if (!ccord_base64_decode(in, in_len, buf, cap, &n)) {
    ccord_free(buf);
    return nullptr;
  }
  buf[n] = 0;
  if (out_len) *out_len = n;
  return buf;
}
