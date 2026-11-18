# M6 test fragment — owned by m6-live.
# Registers tests/test_m6_json.c (user settings / auth sessions / connections JSON decoders) and
# tests/test_m6_user_settings.c (the same data end-to-end through the live transport's gateway and REST queue).
# Included from src/net/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/net.
# discord_json.c only compiles with CCORD_LIVE_ACCOUNT, so the tests need the same guard.

if(CCORD_LIVE_ACCOUNT)
  ccord_add_test(NAME test_m6_json SOURCES tests/test_m6_json.c LIBS ccord_net LABELS unit)
  # M6 account settings/sessions/connections through the live transport (loopback HTTP + Gateway).
  ccord_add_test(NAME test_m6_user_settings SOURCES tests/test_m6_user_settings.c LIBS ccord_net LABELS integration)
endif()
