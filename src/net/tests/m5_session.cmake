# M5 test fragment — owned by m5-session-mock.
# Included from src/net/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/net.
ccord_add_test(NAME test_session_voice SOURCES tests/test_session_voice.c LIBS ccord_session LABELS integration)
