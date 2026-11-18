# M5 test fragment — owned by m5-voice-core.
# Registers: tests/test_voice_dsp.c, tests/test_voice_core.c.
# Included from src/net/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/net.

# voice_dsp.c is registered in CCORD_NET_SOURCES (src/net/CMakeLists.txt) so production builds
# without tests still link it; this fragment only registers the tests.

ccord_add_test(NAME test_voice_dsp SOURCES tests/test_voice_dsp.c LIBS ccord_net LABELS unit)
if(CCORD_LIVE_ACCOUNT)
  ccord_add_test(NAME test_voice_core SOURCES tests/test_voice_core.c LIBS ccord_net LABELS integration)
  ccord_add_test(NAME test_voice_resume SOURCES tests/test_voice_resume.c LIBS ccord_net LABELS integration)
endif()
