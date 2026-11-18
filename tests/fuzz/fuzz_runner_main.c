// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Deterministic driver for the fuzz harnesses in default builds (no libFuzzer). It feeds
// LLVMFuzzerTestOneInput a fixed adversarial seed corpus, structured edge-case inputs, mutations of the
// corpus, and splitmix64-random inputs from several alphabets. The sequence is fully reproducible, so a
// failure is a stable regression. Any ASan/UBSan report, leak or crash aborts the process and fails CTest.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define FUZZ_BUF_CAP 4096u
#define FUZZ_RANDOM_INPUTS 2000u

static uint64_t rng_state = 0x243F6A8885A308D3ULL;

static uint64_t fuzz_rand(void)
{
  uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static size_t fuzz_count;

static void run(const uint8_t *data, size_t size)
{
  (void)LLVMFuzzerTestOneInput(data, size);
  fuzz_count++;
}

typedef struct fuzz_seed {
  const uint8_t *data;
  size_t len;
} fuzz_seed;

#define S(x) { (const uint8_t *)(x), sizeof(x) - 1u }

// Fixed seeds. Deliberately short and token-free (see cmake/CheckHygiene.cmake).
static const fuzz_seed seeds[] = {
  S(""),
  S("{"),
  S("}"),
  S("{}"),
  S("[]"),
  S("[1,2,3]"),
  S("{\"a\":1}"),
  S("{\"a\":}"),
  S("{\"settings\":\"\"}"),
  S("{\"settings\":\"AA==\"}"),
  S("{\"op\":10,\"d\":{\"heartbeat_interval\":41250}}"),
  S("{\"op\":0,\"t\":\"READY\",\"s\":1,\"d\":{}}"),
  S("{\"op\":9,\"d\":false}"),
  S("{\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"1\",\"content\":\"hi\"}}"),
  S("{\"code\":50001,\"message\":\"Missing Access\"}"),
  S("[{\"id\":\"1\"},{\"id\":\"2\"}]"),
  S("{\"pruned\":3}"),
  S("{\"rules\":[]}"),
  S("null"),
  S("true"),
  S("1e999"),
  S("-0"),
  S("\"\\uD800\""),
  S("\"\\u0000\""),
  S("\"\\xff\""),
  // base64 (standard + url-safe + whitespace + bad padding)
  S("AA=="),
  S("aGVsbG8="),
  S("aGVsbG8"),
  S("_-8="),
  S("A A=="),
  S("===="),
  S("A"),
  // INI / theme fragments
  S("[colors]\nbg_primary = #112233\n"),
  S("[metrics]\nfont_size_base = 16\n"),
  S("[fonts]\nregular = a.ttf\n"),
  S("key = value\n"),
  S("[colors]\nbg_primary = #112233\nbg_primary = #445566\n"),
  S("#112233"),
  S("#11223344"),
  S("#gggggg"),
  // snowflake / id boundaries
  S("0"),
  S("1"),
  S("123456789012345678"),
  S("18446744073709551615"),
  S("18446744073709551616"),
  S("99999999999999999999"),
  S("12a45"),
  // markdown
  S("**bold** _it_ ||spoiler|| `code` [link](https://x.y)"),
  S("# heading\n> quote\n- item"),
  // UTF-8 edge sequences (overlong, surrogate, > U+10FFFF, truncated, stray continuation)
  S("\xC0\x80"),
  S("\xED\xA0\x80"),
  S("\xF4\x90\x80\x80"),
  S("\xF0\x9F"),
  S("\x80"),
  S("\xFE\xFF"),
  S("\xF0\x9F\x98\x80"),
  S("a\xC2\xA0z"),
};

// Binary seeds: protobuf-ish tags/varints and Annex-B start codes.
static const uint8_t proto_varint[] = { 0x0A, 0x00 };
static const uint8_t proto_overflow[] = { 0x0A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F };
static const uint8_t proto_map[] = { 0x2A, 0x08, 0x0A, 0x01, 0x61, 0x12, 0x03, 0x08, 0x01, 0x10 };
static const uint8_t annexb_sps[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0A };
static const uint8_t annexb_multi[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0A,
                                        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
                                        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00 };
static const uint8_t annexb_short[] = { 0x00, 0x00, 0x01, 0x65, 0x01 };
static const uint8_t fu_a[] = { 0x7C, 0x85, 0x01, 0x02, 0x03 };
static const uint8_t stap_a[] = { 0x18, 0x00, 0x02, 0x67, 0x42, 0x00, 0x02, 0x68, 0xCE };

static const fuzz_seed binary_seeds[] = {
  { proto_varint, sizeof proto_varint },
  { proto_overflow, sizeof proto_overflow },
  { proto_map, sizeof proto_map },
  { annexb_sps, sizeof annexb_sps },
  { annexb_multi, sizeof annexb_multi },
  { annexb_short, sizeof annexb_short },
  { fu_a, sizeof fu_a },
  { stap_a, sizeof stap_a },
};

static void run_seeds(void)
{
  for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
    run(seeds[i].data, seeds[i].len);
  }
  for (size_t i = 0; i < sizeof binary_seeds / sizeof binary_seeds[0]; i++) {
    run(binary_seeds[i].data, binary_seeds[i].len);
  }
}

// Structured inputs that are awkward to spell as literals: deep nesting, long runs, oversized NALs.
static void run_structured(void)
{
  static uint8_t buf[FUZZ_BUF_CAP];

  // Deep array nesting (past the default JSON depth limit).
  for (size_t i = 0; i < 256; i++) { buf[i] = '['; }
  for (size_t i = 0; i < 256; i++) { buf[256 + i] = ']'; }
  run(buf, 512);

  // Deep object nesting.
  size_t n = 0;
  for (size_t i = 0; i < 128 && n + 5 <= FUZZ_BUF_CAP; i++) { memcpy(buf + n, "{\"a\":", 5); n += 5; }
  buf[n++] = '0';
  for (size_t i = 0; i < 128 && n < FUZZ_BUF_CAP; i++) { buf[n++] = '}'; }
  run(buf, n);

  // Long string value.
  n = 0;
  memcpy(buf, "{\"s\":\"", 6); n = 6;
  for (size_t i = 0; i < 3000; i++) { buf[n++] = 'A'; }
  buf[n++] = '"'; buf[n++] = '}';
  run(buf, n);

  // Long runs of high bytes (invalid UTF-8 / varint continuation).
  memset(buf, 0xFF, 3000);
  run(buf, 3000);
  memset(buf, 0x80, 3000);
  run(buf, 3000);

  // Length-delimited protobuf field claiming a huge length.
  buf[0] = 0x0A;
  memset(buf + 1, 0xFF, 9);
  buf[10] = 0x00;
  run(buf, 11);

  // A large NAL to force FU-A fragmentation, framed by SPS + IDR.
  n = 0;
  memcpy(buf + n, annexb_sps, sizeof annexb_sps); n += sizeof annexb_sps;
  for (size_t i = 0; i < 3000; i++) { buf[n++] = (uint8_t)(i & 0xFF); }
  memcpy(buf + n, annexb_multi + 16, 8); n += 8;   // IDR start code + header
  run(buf, n);

  // Long base64-alphabet run.
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (size_t i = 0; i < 3000; i++) { buf[i] = (uint8_t)b64[i % (sizeof b64 - 1u)]; }
  run(buf, 3000);

  // Many short INI entries.
  n = 0;
  memcpy(buf, "[colors]\n", 9); n = 9;
  for (size_t i = 0; i < 200 && n + 24 <= FUZZ_BUF_CAP; i++) {
    int w = snprintf((char *)buf + n, FUZZ_BUF_CAP - n, "k%zu = #112233\n", i);
    if (w <= 0 || (size_t)w >= FUZZ_BUF_CAP - n) { break; }
    n += (size_t)w;
  }
  run(buf, n);
}

// Truncations, single-bit flips and NUL insertions over the string seeds.
static void run_mutations(void)
{
  static uint8_t buf[FUZZ_BUF_CAP];
  for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
    const fuzz_seed *s = &seeds[i];
    if (s->len == 0 || s->len + 1 > FUZZ_BUF_CAP) { continue; }
    memcpy(buf, s->data, s->len);

    if (s->len >= 2) { run(buf, s->len / 2); }
    run(buf, 1);

    memcpy(buf, s->data, s->len);
    size_t bit = (size_t)(fuzz_rand() % (s->len * 8u));
    buf[bit / 8u] = (uint8_t)(buf[bit / 8u] ^ (uint8_t)(1u << (bit % 8u)));
    run(buf, s->len);

    memcpy(buf, s->data, s->len);
    size_t at = (size_t)(fuzz_rand() % (s->len + 1u));
    memmove(buf + at + 1, buf + at, s->len - at);
    buf[at] = 0;
    run(buf, s->len + 1);
  }
}

static uint8_t pick_byte(uint64_t alphabet)
{
  static const char jsonish[] = "{}[]\":,0123456789abcdefnul \t\n";
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
  uint64_t r = fuzz_rand();
  switch (alphabet) {
    case 0: return (uint8_t)(r & 0xFFu);
    case 1: return (uint8_t)(0x20u + (r % 0x5Fu));
    case 2: return (uint8_t)jsonish[r % (sizeof jsonish - 1u)];
    default: return (uint8_t)b64[r % (sizeof b64 - 1u)];
  }
}

static void run_random(void)
{
  static uint8_t buf[FUZZ_BUF_CAP];
  for (uint64_t i = 0; i < FUZZ_RANDOM_INPUTS; i++) {
    size_t len = (size_t)(fuzz_rand() % (FUZZ_BUF_CAP + 1u));
    uint64_t alphabet = fuzz_rand() % 4u;
    for (size_t j = 0; j < len; j++) { buf[j] = pick_byte(alphabet); }
    run(buf, len);
  }
}

int main(void)
{
  static const uint8_t empty = 0;
  run(&empty, 0);
  run_seeds();
  run_structured();
  run_mutations();
  run_random();
  printf("fuzz_runner: %zu inputs, no failures\n", fuzz_count);
  return 0;
}
