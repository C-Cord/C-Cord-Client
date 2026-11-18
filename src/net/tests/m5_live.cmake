# M5 test fragment — owned by m5-live-transport.
# Part 1 (m5-live-transport): tests/test_voice_loopback.c (Go Live media loopback).
# Part 2 registers tests/test_live_calls.c here when it lands.
# Included from src/net/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/net.

if(CCORD_LIVE_ACCOUNT)
  ccord_add_test(NAME test_voice_loopback SOURCES tests/test_voice_loopback.c LIBS ccord_net LABELS integration)
  ccord_add_test(NAME test_live_calls SOURCES tests/test_live_calls.c LIBS ccord_session LABELS integration)
endif()
