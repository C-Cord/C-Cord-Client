// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/core/sha256.h>

#include <string.h>

#include "ccord_test.h"

static void to_hex(const unsigned char digest[32], char out[65])
{
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    out[i * 2] = digits[digest[i] >> 4];
    out[i * 2 + 1] = digits[digest[i] & 0x0Fu];
  }
  out[64] = '\0';
}

static void check(const void *data, size_t len, const char *expected)
{
  unsigned char digest[32];
  char hex[65];
  ccord_sha256(data, len, digest);
  to_hex(digest, hex);
  CT_CHECK(strcmp(hex, expected) == 0);
}

static void test_fips_vectors(void)
{
  check("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  check("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const char two_block[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  check(two_block, strlen(two_block), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  char million[1000];
  memset(million, 'a', sizeof million);
  ccord_sha256_ctx ctx;
  ccord_sha256_init(&ctx);
  for (int i = 0; i < 1000; i++) {
    ccord_sha256_update(&ctx, million, sizeof million);
  }
  unsigned char digest[32];
  ccord_sha256_final(&ctx, digest);
  char hex[65];
  to_hex(digest, hex);
  CT_CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);

  // final() wipes the context.
  bool zero = true;
  const unsigned char *raw = (const unsigned char *)&ctx;
  for (size_t i = 0; i < sizeof ctx; i++) {
    if (raw[i] != 0) {
      zero = false;
    }
  }
  CT_CHECK(zero);
}

static void test_streaming_matches_one_shot(void)
{
  const char message[] = "the quick brown fox jumps over the lazy dog";
  unsigned char one_shot[32];
  ccord_sha256(message, strlen(message), one_shot);

  ccord_sha256_ctx ctx;
  ccord_sha256_init(&ctx);
  ccord_sha256_update(&ctx, message, 7);
  ccord_sha256_update(&ctx, message + 7, 0);
  ccord_sha256_update(&ctx, message + 7, strlen(message) - 7);
  unsigned char streamed[32];
  ccord_sha256_final(&ctx, streamed);
  CT_CHECK(memcmp(one_shot, streamed, 32) == 0);
}

int main(void)
{
  test_fips_vectors();
  test_streaming_matches_one_shot();
  return CT_RESULT("test_core_sha256");
}
