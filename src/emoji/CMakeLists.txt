add_library(ccord_emoji STATIC emoji.c emoji_data.c)
add_library(ccord::emoji ALIAS ccord_emoji)
target_include_directories(ccord_emoji PUBLIC include PRIVATE .)
ccord_strict_warnings(ccord_emoji)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_emoji SOURCES tests/test_emoji.c LIBS ccord_emoji LABELS unit)
endif()
