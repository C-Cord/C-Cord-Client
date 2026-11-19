// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/random.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

ccord_status ccord_random_bytes(void *buf, size_t len)
{
  if (!buf && len > 0) return CCORD_ERR_INVALID;
  if (len == 0) return CCORD_OK;
  unsigned char *p = buf;
  while (len > 0) {
    ULONG chunk = len > 0x7FFFFFFFu ? 0x7FFFFFFFu : (ULONG)len;
    if (BCryptGenRandom(nullptr, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      return CCORD_ERR_IO;
    }
    p += chunk;
    len -= chunk;
  }
  return CCORD_OK;
}
#elif defined(__APPLE__) || defined(__ANDROID__)
#include <stdlib.h>

ccord_status ccord_random_bytes(void *buf, size_t len)
{
  if (!buf && len > 0) return CCORD_ERR_INVALID;
  if (len > 0) arc4random_buf(buf, len);
  return CCORD_OK;
}
#else
#include <errno.h>
#include <sys/random.h>

ccord_status ccord_random_bytes(void *buf, size_t len)
{
  if (!buf && len > 0) {
    return CCORD_ERR_INVALID;
  }
  unsigned char *p = buf;
  size_t done = 0;
  while (done < len) {
    ssize_t n = getrandom(p + done, len - done, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return CCORD_ERR_IO;
    }
    if (n == 0) {
      return CCORD_ERR_IO;
    }
    done += (size_t)n;
  }
  return CCORD_OK;
}
#endif
