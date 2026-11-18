add_library(ccord_theme STATIC theme.c)
add_library(ccord::theme ALIAS ccord_theme)
target_include_directories(ccord_theme PUBLIC include)
target_link_libraries(ccord_theme PUBLIC ccord_core)
ccord_strict_warnings(ccord_theme)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_theme SOURCES tests/test_theme.c LIBS ccord_theme LABELS unit)
endif()
