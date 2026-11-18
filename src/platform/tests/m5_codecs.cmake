# M5 test fragment — owned by m5-codecs.
# Registers: tests/test_video_decode.c, tests/test_sound_decode.c (committed tiny fixtures).
# Included from src/platform/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/platform.
# NOTE: video_decode.c / sound_decode.c and the libavformat link are production sources, registered
# unconditionally in src/platform/CMakeLists.txt (the app media coordinator calls them in prod builds).

ccord_add_test(NAME test_video_decode
  SOURCES tests/test_video_decode.c
  LIBS ccord_platform
  LABELS unit)

ccord_add_test(NAME test_sound_decode
  SOURCES tests/test_sound_decode.c
  LIBS ccord_platform
  LABELS unit)
