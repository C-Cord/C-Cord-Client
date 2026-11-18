// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/net/voice_audio.h>

#include "ccord_test.h"

static void test_vad_and_mixer(void)
{
  ccord_vad vad;
  ccord_vad_init(&vad, CCORD_VAD_DEFAULT_THRESHOLD_DB, 60u);

  int16_t silence[960 * 2] = {0};
  CT_CHECK(ccord_pcm_rms_dbfs(silence, 960, 2) <= CCORD_VAD_FLOOR_DB + 0.1f);
  CT_CHECK(!ccord_vad_process(&vad, silence, 960, 2, 20u));

  int16_t loud[960 * 2];
  for (size_t i = 0; i < 960 * 2; i++) loud[i] = (i & 1u) ? 4000 : -4000;
  CT_CHECK(ccord_vad_process(&vad, loud, 960, 2, 20u));
  CT_CHECK(vad.active);

  // Hangover keeps VAD active for 60 ms (until 3 x 20ms frames of silence complete)
  CT_CHECK(ccord_vad_process(&vad, silence, 960, 2, 20u));
  CT_CHECK(ccord_vad_process(&vad, silence, 960, 2, 20u));
  CT_CHECK(!ccord_vad_process(&vad, silence, 960, 2, 20u));

  // Push-to-talk overrides VAD threshold
  vad.ptt_enabled = true;
  vad.ptt_held = false;
  CT_CHECK(!ccord_vad_process(&vad, loud, 960, 2, 20u));
  vad.ptt_held = true;
  CT_CHECK(ccord_vad_process(&vad, silence, 960, 2, 20u));

  // Saturating mixer
  int16_t acc[4] = {30000, -30000, 1000, -1000};
  const int16_t add[4] = {10000, -10000, 500, 2000};
  ccord_mix_s16(acc, add, 4);
  CT_CHECK(acc[0] == 32767);
  CT_CHECK(acc[1] == -32768);
  CT_CHECK(acc[2] == 1500);
  CT_CHECK(acc[3] == 1000);
}

static void test_jitter_buffer(void)
{
  ccord_jitter *j = ccord_jitter_create(3u);
  CT_CHECK(j != nullptr);
  if (!j) return;

  uint8_t out[64];
  size_t len = 0;
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_EMPTY);

  const uint8_t p10[2] = {0x10, 0x0A};
  const uint8_t p11[2] = {0x10, 0x0B};
  const uint8_t p12[2] = {0x10, 0x0C};
  const uint8_t p14[2] = {0x10, 0x0E};

  // Push out-of-order: 11, then 10 (prefill < 3 -> BUFFERING)
  CT_CHECK(ccord_jitter_push(j, 11u, p11, sizeof p11));
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_BUFFERING);
  CT_CHECK(ccord_jitter_push(j, 10u, p10, sizeof p10));
  CT_CHECK(!ccord_jitter_push(j, 10u, p10, sizeof p10)); // duplicate rejected
  CT_CHECK(ccord_jitter_push(j, 12u, p12, sizeof p12));

  // Now 3 packets buffered -> pops in order 10, 11, 12
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_PACKET);
  CT_CHECK(len == 2 && out[1] == 0x0Au);
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_PACKET);
  CT_CHECK(len == 2 && out[1] == 0x0Bu);
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_PACKET);
  CT_CHECK(len == 2 && out[1] == 0x0Cu);

  // Gap at seq 13 with seq 14 buffered -> CCORD_JITTER_LOST then CCORD_JITTER_PACKET(14)
  CT_CHECK(ccord_jitter_push(j, 14u, p14, sizeof p14));
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_LOST);
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_PACKET);
  CT_CHECK(len == 2 && out[1] == 0x0Eu);
  CT_CHECK(ccord_jitter_pop(j, out, sizeof out, &len) == CCORD_JITTER_EMPTY);

  ccord_jitter_destroy(j);
}

static void test_ssrc_map_and_speaking_tracker(void)
{
  ccord_ssrc_map map;
  ccord_ssrc_map_init(&map);
  CT_CHECK(ccord_ssrc_map_set(&map, 1111u, 1002u));
  CT_CHECK(ccord_ssrc_map_set(&map, 2222u, 1003u));
  CT_CHECK(ccord_ssrc_map_user(&map, 1111u) == 1002u);
  CT_CHECK(ccord_ssrc_map_user(&map, 2222u) == 1003u);
  ccord_ssrc_map_remove_user(&map, 1002u);
  CT_CHECK(ccord_ssrc_map_user(&map, 1111u) == CCORD_ID_NONE);
  ccord_ssrc_map_free(&map);

  ccord_speaking spk;
  ccord_speaking_init(&spk);
  ccord_id uid = CCORD_ID_NONE;
  bool on = false;

  // Remote user audio activity + timeout
  ccord_speaking_note_audio(&spk, 1002u, 1000u);
  CT_CHECK(ccord_speaking_is(&spk, 1002u, 1100u));
  CT_CHECK(ccord_speaking_poll(&spk, 1100u, &uid, &on) && uid == 1002u && on);
  CT_CHECK(!ccord_speaking_poll(&spk, 1100u, &uid, &on));
  CT_CHECK(!ccord_speaking_is(&spk, 1002u, 1300u));
  CT_CHECK(ccord_speaking_poll(&spk, 1300u, &uid, &on) && uid == 1002u && !on);

  // Forced self VAD state
  ccord_speaking_set_forced(&spk, 1001u, true, 2000u);
  CT_CHECK(ccord_speaking_poll(&spk, 2000u, &uid, &on) && uid == 1001u && on);
  ccord_speaking_set_forced(&spk, 1001u, false, 2040u);
  CT_CHECK(ccord_speaking_poll(&spk, 2040u, &uid, &on) && uid == 1001u && !on);
}

int main(void)
{
  test_vad_and_mixer();
  test_jitter_buffer();
  test_ssrc_map_and_speaking_tracker();
  return CT_RESULT("test_voice_audio");
}
