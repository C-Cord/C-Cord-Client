# M6 test fragment — owned by m6-session-mock.
# Registers tests/test_session_user_settings.c (account settings, device sessions, connections).
# Included from src/session/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/session.
ccord_add_test(NAME test_session_user_settings SOURCES tests/test_session_user_settings.c
               LIBS ccord_session LABELS unit)
if(CCORD_LIVE_ACCOUNT)
  # test_session_user_settings also drives a live-transport session against the fake Discord and fake gateway.
  target_compile_definitions(test_session_user_settings PRIVATE CCORD_LIVE_ACCOUNT=1)
endif()
